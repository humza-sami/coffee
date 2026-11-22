#include "coffee/model.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>

namespace coffee {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kMaxYamlNodes = 200'000;  // bounds alias expansion
constexpr size_t kMaxYamlDepth = 64;
constexpr size_t kMaxArgs = 256;
constexpr size_t kMaxArgBytes = 8192;
constexpr size_t kMaxPathsPerTask = 256;
constexpr int64_t kMaxCpus = 1024;
constexpr int64_t kMaxMemoryMb = 4'194'304;  // 4 TiB, a sanity bound
constexpr int64_t kMaxTimeoutSeconds = 604'800;
constexpr int64_t kMaxAttempts = 100;

[[noreturn]] void fail(const std::string& field, const std::string& message) { throw WorkloadError(field, message); }

// ------------------------------------------------------------------ YAML -> JSON

bool is_canonical_int(const std::string& s) {
  size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
  if (i >= s.size()) return false;
  if (s[i] == '0') return s.size() == i + 1 && i == 0;  // "0" only; no "-0", no leading zeros
  for (; i < s.size(); i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

json yaml_to_json(const YAML::Node& node, size_t depth, size_t& visited) {
  if (++visited > kMaxYamlNodes) fail("", "YAML document expands to too many nodes");
  if (depth > kMaxYamlDepth) fail("", "YAML nesting deeper than " + std::to_string(kMaxYamlDepth));
  switch (node.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
      return nullptr;
    case YAML::NodeType::Scalar: {
      const std::string& text = node.Scalar();
      bool quoted = node.Tag() == "!";
      if (!quoted) {
        if (text.empty() || text == "~" || text == "null") return nullptr;
        if (is_canonical_int(text)) {
          int64_t v = 0;
          auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
          if (ec == std::errc() && p == text.data() + text.size()) return v;
        }
      }
      return text;
    }
    case YAML::NodeType::Sequence: {
      json arr = json::array();
      for (const auto& child : node) arr.push_back(yaml_to_json(child, depth + 1, visited));
      return arr;
    }
    case YAML::NodeType::Map: {
      json obj = json::object();
      for (const auto& kv : node) {
        if (!kv.first.IsScalar()) fail("", "mapping keys must be scalars");
        std::string key = kv.first.Scalar();
        if (obj.contains(key)) fail(key, "duplicate key");
        obj[key] = yaml_to_json(kv.second, depth + 1, visited);
      }
      return obj;
    }
  }
  return nullptr;
}

json decode_yaml(const std::string& text) {
  if (text.size() > kMaxWorkloadBytes) fail("", "workload larger than " + std::to_string(kMaxWorkloadBytes) + " bytes");
  std::vector<YAML::Node> docs;
  try {
    docs = YAML::LoadAll(text);
  } catch (const YAML::Exception& e) {
    fail("", std::string("malformed YAML: ") + e.what());
  } catch (const std::exception& e) {
    fail("", std::string("cannot parse YAML: ") + e.what());
  }
  if (docs.empty()) fail("", "empty workload document");
  if (docs.size() > 1) fail("", "multiple YAML documents are not supported");
  size_t visited = 0;
  return yaml_to_json(docs[0], 0, visited);
}

// ------------------------------------------------------------------ normalization helpers

std::string idx(const std::string& base, size_t i) { return base + "[" + std::to_string(i) + "]"; }
std::string sub(const std::string& base, const std::string& key) { return base.empty() ? key : base + "." + key; }

const json& require_object(const json& v, const std::string& field) {
  if (!v.is_object()) fail(field, "must be a mapping");
  return v;
}

void reject_unknown(const json& obj, const std::string& field, std::initializer_list<const char*> allowed) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    bool ok = false;
    for (const char* a : allowed) ok = ok || it.key() == a;
    if (!ok) fail(sub(field, it.key()), "unknown field");
  }
}

std::string as_string(const json& v, const std::string& field) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
  fail(field, "must be a string");
}

bool valid_name(const std::string& s) {
  if (s.empty() || s.size() > 63) return false;
  auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  if (!alnum(s[0])) return false;
  for (char c : s)
    if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
  return true;
}

std::string name_field(const json& obj, const std::string& key, const std::string& field) {
  if (!obj.contains(key)) fail(field, "required");
  std::string s = as_string(obj.at(key), field);
  if (!valid_name(s)) fail(field, "must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
  return s;
}

int64_t int_field(const json& obj, const std::string& key, const std::string& field, int64_t def, int64_t lo, int64_t hi) {
  if (!obj.contains(key)) return def;
  const json& v = obj.at(key);
  if (!v.is_number_integer()) fail(field, "must be an integer");
  int64_t x = v.get<int64_t>();
  if (x < lo || x > hi) fail(field, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
  return x;
}

std::string safe_path(const json& v, const std::string& field) {
  std::string p = as_string(v, field);
  if (auto why = check_safe_relative_path(p)) fail(field, *why);
  return p;
}

bool paths_overlap(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto prefix = [](const std::string& x, const std::string& y) { return y.size() > x.size() && y.compare(0, x.size(), x) == 0 && y[x.size()] == '/'; };
  return prefix(a, b) || prefix(b, a);
}

// Reject duplicates / file-vs-directory conflicts between a new path and previously seen ones.
void check_destination(const std::vector<std::string>& seen, const std::string& p, const std::string& field, const char* what) {
  for (const auto& s : seen) {
    if (s == p) fail(field, std::string("duplicate ") + what + " '" + p + "'");
    if (paths_overlap(s, p)) fail(field, std::string(what) + " '" + p + "' conflicts with '" + s + "' (one would be inside the other)");
  }
}

json normalize_task(const json& raw, size_t i, const NormalizeOptions& options) {
  const std::string f = idx("spec.tasks", i);
  require_object(raw, f);
  reject_unknown(raw, f, {"id", "command", "depends_on", "inputs", "outputs", "resources", "timeout_seconds", "max_attempts"});
  json t = json::object();
  t["id"] = name_field(raw, "id", sub(f, "id"));

  // command (argv)
  const std::string cf = sub(f, "command");
  if (!raw.contains("command")) fail(cf, "required");
  const json& cmd = raw.at("command");
  if (!cmd.is_array()) fail(cf, "must be a list of argv strings");
  if (cmd.empty()) fail(cf, "must be a non-empty list");
  if (cmd.size() > kMaxArgs) fail(cf, "more than " + std::to_string(kMaxArgs) + " arguments");
  json argv = json::array();
  for (size_t k = 0; k < cmd.size(); k++) {
    std::string a = as_string(cmd[k], idx(cf, k));
    if (k == 0 && a.empty()) fail(idx(cf, k), "program must not be empty");
    if (a.size() > kMaxArgBytes) fail(idx(cf, k), "argument longer than " + std::to_string(kMaxArgBytes) + " bytes");
    if (a.find('\0') != std::string::npos) fail(idx(cf, k), "argument contains NUL");
    argv.push_back(std::move(a));
  }
  t["command"] = std::move(argv);

  // depends_on (unknown/cycle checks happen once all ids are known)
  const std::string df = sub(f, "depends_on");
  json deps = json::array();
  if (raw.contains("depends_on")) {
    if (!raw.at("depends_on").is_array()) fail(df, "must be a list of task ids");
    std::set<std::string> seen;
    const auto& d = raw.at("depends_on");
    for (size_t k = 0; k < d.size(); k++) {
      std::string dep = as_string(d[k], idx(df, k));
      if (dep == t["id"].get<std::string>()) fail(idx(df, k), "task cannot depend on itself");
      if (!seen.insert(dep).second) fail(idx(df, k), "duplicate dependency '" + dep + "'");
      deps.push_back(dep);
    }
  }
  t["depends_on"] = std::move(deps);

  // inputs
  const std::string inf = sub(f, "inputs");
  json inputs = json::array();
  std::vector<std::string> targets;
  if (raw.contains("inputs")) {
    if (!raw.at("inputs").is_array()) fail(inf, "must be a list");
    const auto& in = raw.at("inputs");
    if (in.size() > kMaxPathsPerTask) fail(inf, "too many inputs");
    for (size_t k = 0; k < in.size(); k++) {
      const std::string kf = idx(inf, k);
      require_object(in[k], kf);
      reject_unknown(in[k], kf, {"source", "from_task", "output", "target"});
      const bool has_source = in[k].contains("source"), has_from = in[k].contains("from_task");
      if (has_source == has_from) fail(kf, "exactly one of 'source' or 'from_task' is required");
      if (in[k].contains("output") && !has_from) fail(sub(kf, "output"), "only valid together with from_task");
      if (!in[k].contains("target")) fail(sub(kf, "target"), "required");
      std::string target = safe_path(in[k]["target"], sub(kf, "target"));
      check_destination(targets, target, sub(kf, "target"), "input target");
      targets.push_back(target);
      if (has_from) {
        std::string from = name_field(in[k], "from_task", sub(kf, "from_task"));
        if (from == t["id"].get<std::string>()) fail(sub(kf, "from_task"), "a task cannot consume its own output ('" + from + "' is the task itself)");
        if (!in[k].contains("output")) fail(sub(kf, "output"), "required with from_task");
        std::string output = safe_path(in[k]["output"], sub(kf, "output"));
        inputs.push_back({{"from_task", from}, {"output", output}, {"target", target}});
        continue;
      }
      std::string source = safe_path(in[k]["source"], sub(kf, "source"));
      if (options.source_root) {
        fs::path resolved;
        try {
          resolved = confine_path(*options.source_root, source);
        } catch (const Error& e) {
          fail(sub(kf, "source"), e.what());
        }
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) fail(sub(kf, "source"), "input file '" + source + "' does not exist or is not a regular file");
      }
      inputs.push_back({{"source", source}, {"target", target}});
    }
  }
  t["inputs"] = std::move(inputs);

  // outputs
  const std::string of = sub(f, "outputs");
  json outputs = json::array();
  if (raw.contains("outputs")) {
    if (!raw.at("outputs").is_array()) fail(of, "must be a list of relative paths");
    const auto& out = raw.at("outputs");
    if (out.size() > kMaxPathsPerTask) fail(of, "too many outputs");
    std::vector<std::string> seen;
    for (size_t k = 0; k < out.size(); k++) {
      std::string p = safe_path(out[k], idx(of, k));
      for (const auto& tg : targets)
        if (paths_overlap(tg, p)) fail(idx(of, k), "output '" + p + "' conflicts with input target '" + tg + "'");
      check_destination(seen, p, idx(of, k), "output");
      seen.push_back(p);
      outputs.push_back(p);
    }
  }
  t["outputs"] = std::move(outputs);

  // resources and limits
  json res = json::object();
  const std::string rf = sub(f, "resources");
  json rraw = json::object();
  if (raw.contains("resources")) {
    rraw = require_object(raw.at("resources"), rf);
    reject_unknown(rraw, rf, {"cpus", "memory_mb"});
  }
  res["cpus"] = int_field(rraw, "cpus", sub(rf, "cpus"), 1, 1, kMaxCpus);
  res["memory_mb"] = int_field(rraw, "memory_mb", sub(rf, "memory_mb"), 256, 1, kMaxMemoryMb);
  t["resources"] = std::move(res);
  t["timeout_seconds"] = int_field(raw, "timeout_seconds", sub(f, "timeout_seconds"), 300, 1, kMaxTimeoutSeconds);
  t["max_attempts"] = int_field(raw, "max_attempts", sub(f, "max_attempts"), 3, 1, kMaxAttempts);
  return t;
}

void check_dependency_graph(const json& tasks) {
  std::map<std::string, size_t> index;
  for (size_t i = 0; i < tasks.size(); i++) index[tasks[i]["id"].get<std::string>()] = i;
  for (size_t i = 0; i < tasks.size(); i++) {
    const auto& deps = tasks[i]["depends_on"];
    for (size_t k = 0; k < deps.size(); k++)
      if (!index.count(deps[k].get<std::string>()))
        fail(idx(sub(idx("spec.tasks", i), "depends_on"), k), "unknown task '" + deps[k].get<std::string>() + "'");
  }
  // dependency-output inputs: the producer must be an explicit dependency and declare the output
  for (size_t i = 0; i < tasks.size(); i++) {
    const auto& inputs = tasks[i]["inputs"];
    for (size_t k = 0; k < inputs.size(); k++) {
      if (!inputs[k].contains("from_task")) continue;
      const std::string from = inputs[k]["from_task"].get<std::string>(), output = inputs[k]["output"].get<std::string>();
      const std::string kf = idx(sub(idx("spec.tasks", i), "inputs"), k);
      auto it = index.find(from);
      bool listed = false;
      for (const auto& d : tasks[i]["depends_on"]) listed = listed || d.get<std::string>() == from;
      if (it == index.end() || !listed) fail(sub(kf, "from_task"), "producer '" + from + "' must be listed in this task's depends_on");
      bool declared = false;
      for (const auto& o : tasks[it->second]["outputs"]) declared = declared || o.get<std::string>() == output;
      if (!declared) fail(sub(kf, "output"), "'" + output + "' is not one of the declared outputs of task '" + from + "'");
    }
  }
  // iterative DFS cycle detection
  enum { White, Grey, Black };
  std::vector<int> color(tasks.size(), White);
  std::vector<size_t> path;
  std::function<void(size_t)> visit = [&](size_t u) {
    color[u] = Grey;
    path.push_back(u);
    for (const auto& d : tasks[u]["depends_on"]) {
      size_t v = index[d.get<std::string>()];
      if (color[v] == Grey) {
        std::string msg = "dependency cycle: ";
        auto start = std::find(path.begin(), path.end(), v);
        for (auto it = start; it != path.end(); ++it) msg += tasks[*it]["id"].get<std::string>() + " -> ";
        fail("spec.tasks", msg + tasks[v]["id"].get<std::string>());
      }
      if (color[v] == White) visit(v);
    }
    path.pop_back();
    color[u] = Black;
  };
  for (size_t i = 0; i < tasks.size(); i++)
    if (color[i] == White) visit(i);
}

}  // namespace

json normalize_workload(const json& doc, const NormalizeOptions& options) {
  require_object(doc, "");
  reject_unknown(doc, "", {"apiVersion", "kind", "metadata", "spec"});
  if (!doc.contains("apiVersion")) fail("apiVersion", "required");
  std::string api = as_string(doc["apiVersion"], "apiVersion");
  if (api != kApiVersion) fail("apiVersion", "unsupported apiVersion '" + api + "' (supported: " + kApiVersion + ")");
  if (!doc.contains("kind")) fail("kind", "required");
  if (as_string(doc["kind"], "kind") != "Workload") fail("kind", "must be 'Workload'");
  if (!doc.contains("metadata")) fail("metadata", "required");
  require_object(doc["metadata"], "metadata");
  reject_unknown(doc["metadata"], "metadata", {"name"});
  std::string name = name_field(doc["metadata"], "name", "metadata.name");
  if (!doc.contains("spec")) fail("spec", "required");
  require_object(doc["spec"], "spec");
  reject_unknown(doc["spec"], "spec", {"tasks"});
  if (!doc["spec"].contains("tasks")) fail("spec.tasks", "required");
  const json& raw_tasks = doc["spec"]["tasks"];
  if (!raw_tasks.is_array()) fail("spec.tasks", "must be a list");
  if (raw_tasks.empty()) fail("spec.tasks", "must contain at least one task");
  if (raw_tasks.size() > kMaxTasks) fail("spec.tasks", "more than " + std::to_string(kMaxTasks) + " tasks");

  json tasks = json::array();
  std::set<std::string> ids;
  for (size_t i = 0; i < raw_tasks.size(); i++) {
    json t = normalize_task(raw_tasks[i], i, options);
    if (!ids.insert(t["id"].get<std::string>()).second) fail(sub(idx("spec.tasks", i), "id"), "duplicate task id '" + t["id"].get<std::string>() + "'");
    tasks.push_back(std::move(t));
  }
  check_dependency_graph(tasks);
  return {{"apiVersion", kApiVersion}, {"kind", "Workload"}, {"metadata", {{"name", name}}}, {"spec", {{"tasks", std::move(tasks)}}}};
}

json parse_workload_text(const std::string& yaml, const std::optional<fs::path>& source_root) {
  NormalizeOptions options;
  options.source_root = source_root;
  return normalize_workload(decode_yaml(yaml), options);
}

json parse_workload(const fs::path& path) {
  std::string text;
  try {
    text = read_file_limited(path, kMaxWorkloadBytes);
  } catch (const Error& e) {
    if (e.code() == "too_large") fail("", e.what());
    throw;
  }
  std::error_code ec;
  fs::path root = fs::absolute(path, ec).parent_path();
  return parse_workload_text(text, root);
}

}  // namespace coffee
