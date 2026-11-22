#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>

#include "coffee/model.hpp"

using namespace coffee;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const char* kMinimal = R"(
apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: demo}
spec:
  tasks:
    - id: a
      command: [echo, hello]
)";

std::string with_tasks(const std::string& tasks) {
  return "apiVersion: coffee/v1alpha1\nkind: Workload\nmetadata:\n  name: demo\nspec:\n  tasks:\n" + tasks;
}

void expect_invalid(const std::string& yaml, const std::string& field_part, const std::string& msg_part = "") {
  try {
    parse_workload_text(yaml);
    FAIL("expected WorkloadError for field " << field_part);
  } catch (const WorkloadError& e) {
    INFO(e.what());
    CHECK(e.code() == "invalid_workload");
    CHECK(e.field().find(field_part) != std::string::npos);
    CHECK(std::string(e.what()).find(msg_part) != std::string::npos);
  }
}
}  // namespace

TEST_CASE("minimal workload gets documented defaults", "[model]") {
  json w = parse_workload_text(kMinimal);
  CHECK(w["apiVersion"] == "coffee/v1alpha1");
  CHECK(w["kind"] == "Workload");
  CHECK(w["metadata"]["name"] == "demo");
  const auto& t = w["spec"]["tasks"][0];
  CHECK(t["id"] == "a");
  CHECK(t["command"] == json::array({"echo", "hello"}));
  CHECK(t["depends_on"] == json::array());
  CHECK(t["inputs"] == json::array());
  CHECK(t["outputs"] == json::array());
  CHECK(t["resources"] == json({{"cpus", 1}, {"memory_mb", 256}}));
  CHECK(t["timeout_seconds"] == 300);
  CHECK(t["max_attempts"] == 3);
}

TEST_CASE("full task fields are preserved and normalization is idempotent", "[model]") {
  std::string y = with_tasks(R"(    - id: first
      command: ["sh", "-c", "cat in.txt > out.txt"]
      inputs: [{source: data/in.txt, target: in.txt}]
      outputs: [out.txt, "sub/o2.bin"]
      resources: {cpus: 2, memory_mb: 512}
      timeout_seconds: 10
      max_attempts: 5
    - id: second
      command: [true]
      depends_on: [first]
)");
  json w = parse_workload_text(y);
  const auto& t = w["spec"]["tasks"][0];
  CHECK(t["inputs"][0] == json({{"source", "data/in.txt"}, {"target", "in.txt"}}));
  CHECK(t["outputs"] == json::array({"out.txt", "sub/o2.bin"}));
  CHECK(t["resources"]["cpus"] == 2);
  CHECK(t["max_attempts"] == 5);
  CHECK(w["spec"]["tasks"][1]["depends_on"] == json::array({"first"}));
  CHECK(normalize_workload(w) == w);
}

TEST_CASE("plain numeric argv items stay strings", "[model]") {
  json w = parse_workload_text(with_tasks("    - id: a\n      command: [sleep, 1, 007]\n"));
  CHECK(w["spec"]["tasks"][0]["command"] == json::array({"sleep", "1", "007"}));
}

TEST_CASE("malformed and oversized YAML is rejected with a controlled error", "[model]") {
  CHECK_THROWS_AS(parse_workload_text("a: [unclosed"), WorkloadError);
  CHECK_THROWS_AS(parse_workload_text("\t- bad: : :"), WorkloadError);
  CHECK_THROWS_AS(parse_workload_text(""), WorkloadError);
  CHECK_THROWS_AS(parse_workload_text("- just\n- a list\n"), WorkloadError);
  CHECK_THROWS_AS(parse_workload_text(std::string(kMaxWorkloadBytes + 1, '#')), WorkloadError);
  CHECK_THROWS_AS(parse_workload_text(std::string(200000, '[')), WorkloadError);  // deep nesting must not crash
  CHECK_THROWS_AS(parse_workload_text(std::string(kMinimal) + "---\nkind: Other\n"), WorkloadError);  // multi-document
}

TEST_CASE("schema header is enforced", "[model]") {
  std::string base = kMinimal;
  auto replace = [&](const std::string& a, const std::string& b) {
    std::string s = base;
    s.replace(s.find(a), a.size(), b);
    return s;
  };
  expect_invalid(replace("coffee/v1alpha1", "coffee/v9"), "apiVersion", "unsupported");
  expect_invalid(replace("kind: Workload", "kind: Job"), "kind");
  expect_invalid(replace("{name: demo}", "{name: \"bad name\"}"), "metadata.name");
  expect_invalid(replace("metadata: {name: demo}\n", ""), "metadata");
  expect_invalid(replace("spec:", "extra: 1\nspec:"), "extra", "unknown");
  expect_invalid(replace("command: [echo, hello]", "command: [echo]\n      bogus: 1"), "spec.tasks[0].bogus", "unknown");
}

TEST_CASE("task id, command and list validation", "[model]") {
  expect_invalid(with_tasks("    - id: a\n      command: [x]\n    - id: a\n      command: [x]\n"), "spec.tasks[1].id", "duplicate");
  expect_invalid(with_tasks("    - id: \"a b\"\n      command: [x]\n"), "spec.tasks[0].id");
  expect_invalid(with_tasks("    - id: a\n      command: []\n"), "spec.tasks[0].command", "non-empty");
  expect_invalid(with_tasks("    - id: a\n      command: \"echo hi\"\n"), "spec.tasks[0].command", "list");
  expect_invalid(with_tasks("    - id: a\n"), "spec.tasks[0].command", "required");
  expect_invalid(with_tasks("    - id: a\n      command: [\"\"]\n"), "spec.tasks[0].command[0]");
  expect_invalid(with_tasks("    - command: [x]\n"), "spec.tasks[0].id", "required");
  CHECK_THROWS_AS(parse_workload_text("apiVersion: coffee/v1alpha1\nkind: Workload\nmetadata: {name: d}\nspec:\n  tasks: []\n"), WorkloadError);
}

TEST_CASE("resources and limits must be positive integers", "[model]") {
  auto task = [](const std::string& extra) { return with_tasks("    - id: a\n      command: [x]\n" + extra); };
  expect_invalid(task("      resources: {cpus: 0}\n"), "resources.cpus");
  expect_invalid(task("      resources: {cpus: -1}\n"), "resources.cpus");
  expect_invalid(task("      resources: {cpus: 1.5}\n"), "resources.cpus", "integer");
  expect_invalid(task("      resources: {cpus: lots}\n"), "resources.cpus");
  expect_invalid(task("      resources: {memory_mb: 0}\n"), "resources.memory_mb");
  expect_invalid(task("      resources: {gpus: 1}\n"), "resources.gpus", "unknown");
  expect_invalid(task("      timeout_seconds: 0\n"), "timeout_seconds");
  expect_invalid(task("      max_attempts: 0\n"), "max_attempts");
  expect_invalid(task("      max_attempts: 1000\n"), "max_attempts");
  expect_invalid(task("      resources: {cpus: 99999999999999999999}\n"), "resources.cpus");
}

TEST_CASE("dependencies: unknown, self, duplicate and cycles are rejected", "[model]") {
  auto two = [](const std::string& da, const std::string& db) {
    return with_tasks("    - id: a\n      command: [x]\n      depends_on: " + da + "\n    - id: b\n      command: [x]\n      depends_on: " + db + "\n");
  };
  expect_invalid(two("[ghost]", "[]"), "spec.tasks[0].depends_on[0]", "unknown");
  expect_invalid(two("[a]", "[]"), "spec.tasks[0].depends_on", "itself");
  expect_invalid(two("[b, b]", "[]"), "spec.tasks[0].depends_on", "duplicate");
  expect_invalid(two("[b]", "[a]"), "spec.tasks", "cycle");
  CHECK_NOTHROW(parse_workload_text(two("[b]", "[]")));  // forward references are fine
  std::string three = with_tasks("    - id: a\n      command: [x]\n      depends_on: [c]\n    - id: b\n      command: [x]\n      depends_on: [a]\n    - id: c\n      command: [x]\n      depends_on: [b]\n");
  expect_invalid(three, "spec.tasks", "cycle");
}

TEST_CASE("paths: traversal, absolute, duplicate destinations", "[model]") {
  auto task = [](const std::string& extra) { return with_tasks("    - id: a\n      command: [x]\n" + extra); };
  expect_invalid(task("      outputs: [../escape]\n"), "outputs[0]");
  expect_invalid(task("      outputs: [/etc/passwd]\n"), "outputs[0]");
  expect_invalid(task("      outputs: [o, o]\n"), "outputs[1]", "duplicate");
  expect_invalid(task("      outputs: [d, d/f]\n"), "outputs[1]", "conflict");
  expect_invalid(task("      inputs: [{source: ../x, target: t}]\n"), "inputs[0].source");
  expect_invalid(task("      inputs: [{source: s, target: /abs}]\n"), "inputs[0].target");
  expect_invalid(task("      inputs: [{source: s}]\n"), "inputs[0].target", "required");
  expect_invalid(task("      inputs: [{source: s1, target: t}, {source: s2, target: t}]\n"), "inputs[1].target", "duplicate");
  expect_invalid(task("      inputs: [{source: s, target: t}]\n      outputs: [t]\n"), "outputs[0]", "input");
}

TEST_CASE("input sources are checked against the workload directory", "[model]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-model-" + random_hex(6));
  auto outside = fs::temp_directory_path() / ("coffee-test-model-out-" + random_hex(6));
  fs::create_directories(dir / "data");
  fs::create_directories(outside);
  { std::ofstream(dir / "data/in.txt") << "x"; }
  { std::ofstream(outside / "secret") << "s"; }
  fs::create_symlink(outside / "secret", dir / "data/link");
  auto write = [&](const std::string& src) {
    std::ofstream(dir / "w.yaml") << with_tasks("    - id: a\n      command: [x]\n      inputs: [{source: " + src + ", target: t}]\n");
  };
  write("data/in.txt");
  CHECK_NOTHROW(parse_workload(dir / "w.yaml"));
  write("data/missing.txt");
  CHECK_THROWS_AS(parse_workload(dir / "w.yaml"), WorkloadError);
  write("data/link");
  CHECK_THROWS_AS(parse_workload(dir / "w.yaml"), WorkloadError);
  write("data");  // a directory is not a regular file
  CHECK_THROWS_AS(parse_workload(dir / "w.yaml"), WorkloadError);
  CHECK_THROWS_AS(parse_workload(dir / "absent.yaml"), Error);
  fs::remove_all(dir);
  fs::remove_all(outside);
}

TEST_CASE("normalize_workload accepts API JSON and rejects wrong types", "[model]") {
  json doc = {{"apiVersion", "coffee/v1alpha1"}, {"kind", "Workload"}, {"metadata", {{"name", "j"}}},
              {"spec", {{"tasks", json::array({{{"id", "t"}, {"command", json::array({"true"})}}})}}}};
  CHECK(normalize_workload(doc)["spec"]["tasks"][0]["max_attempts"] == 3);
  doc["spec"]["tasks"][0]["command"] = 5;
  CHECK_THROWS_AS(normalize_workload(doc), WorkloadError);
  CHECK_THROWS_AS(normalize_workload(json::array()), WorkloadError);
  CHECK_THROWS_AS(normalize_workload(json(nullptr)), WorkloadError);
}

TEST_CASE("shipped example validates", "[model]") {
  json w = parse_workload(fs::path(COFFEE_TEST_SOURCE_DIR) / "examples/coffee.yaml");
  CHECK(w["spec"]["tasks"].size() >= 2);
}

// ------------------------------------------------------------------ dependency-output inputs (additive alpha amendment)

namespace {
const char* kTwoStep = R"(
    - id: produce
      command: [sh, -c, "tr a-z A-Z < in.txt > up.txt"]
      inputs: [{source: data/part-1.txt, target: in.txt}]
      outputs: [up.txt, extra/log.txt]
    - id: consume
      command: [sh, -c, "wc -c < up.txt > n.txt"]
      depends_on: [produce]
      inputs:
        - {from_task: produce, output: up.txt, target: in/up.txt}
      outputs: [n.txt]
)";
}

TEST_CASE("from_task input normalizes to {from_task, output, target} and is idempotent", "[model][deps]") {
  json w = parse_workload_text(with_tasks(kTwoStep));
  const json& in = w["spec"]["tasks"][1]["inputs"][0];
  CHECK(in == json{{"from_task", "produce"}, {"output", "up.txt"}, {"target", "in/up.txt"}});
  CHECK(w["spec"]["tasks"][0]["inputs"][0] == json{{"source", "data/part-1.txt"}, {"target", "in.txt"}});
  CHECK(normalize_workload(w) == w);
}

TEST_CASE("from_task inputs reject malformed or unauthorized references", "[model][deps]") {
  auto consumer = [](const std::string& input, const std::string& depends = "depends_on: [produce]") {
    return with_tasks(R"(
    - id: produce
      command: [true]
      outputs: [up.txt]
    - id: consume
      command: [true]
      )" + depends + R"(
      inputs: [)" + input + "]\n");
  };
  expect_invalid(consumer("{from_task: produce, output: up.txt}"), "inputs[0].target", "required");
  expect_invalid(consumer("{from_task: produce, target: x}"), "inputs[0].output", "required");
  expect_invalid(consumer("{source: s, output: up.txt, target: x}"), "inputs[0].output", "from_task");
  expect_invalid(consumer("{source: s, from_task: produce, output: up.txt, target: x}"), "inputs[0]", "exactly one");
  expect_invalid(consumer("{target: x}"), "inputs[0]", "exactly one");
  expect_invalid(consumer("{from_task: produce, output: up.txt, target: x, extra: 1}"), "inputs[0].extra", "unknown");
  expect_invalid(consumer("{from_task: nope, output: up.txt, target: x}", "depends_on: [produce]"), "inputs[0].from_task", "depends_on");
  expect_invalid(consumer("{from_task: produce, output: up.txt, target: x}", "depends_on: []"), "inputs[0].from_task", "depends_on");
  expect_invalid(consumer("{from_task: produce, output: other.txt, target: x}"), "inputs[0].output", "declared outputs");
  expect_invalid(consumer("{from_task: produce, output: ../up.txt, target: x}"), "inputs[0].output", "..");
  expect_invalid(consumer("{from_task: produce, output: up.txt, target: /abs}"), "inputs[0].target", "relative");
  expect_invalid(consumer("{from_task: produce, output: up.txt, target: ../x}"), "inputs[0].target", "..");
  expect_invalid(consumer("{from_task: consume, output: up.txt, target: x}"), "inputs[0].from_task", "itself");
  expect_invalid(consumer("{from_task: 'bad id!', output: up.txt, target: x}"), "inputs[0].from_task", "match");
}

TEST_CASE("from_task and source targets share the destination conflict rules", "[model][deps]") {
  expect_invalid(with_tasks(R"(
    - {id: produce, command: [true], outputs: [up.txt]}
    - id: consume
      command: [true]
      depends_on: [produce]
      inputs:
        - {source: s.txt, target: same}
        - {from_task: produce, output: up.txt, target: same}
)"), "inputs[1].target", "duplicate");
  expect_invalid(with_tasks(R"(
    - {id: produce, command: [true], outputs: [up.txt]}
    - id: consume
      command: [true]
      depends_on: [produce]
      inputs:
        - {from_task: produce, output: up.txt, target: dir}
      outputs: [dir/x]
)"), "outputs[0]", "conflicts");
}

TEST_CASE("from_task producer cycles are still rejected before any side effect", "[model][deps]") {
  expect_invalid(with_tasks(R"(
    - {id: a, command: [true], outputs: [o], depends_on: [b], inputs: [{from_task: b, output: o, target: x}]}
    - {id: b, command: [true], outputs: [o], depends_on: [a], inputs: [{from_task: a, output: o, target: x}]}
)"), "spec.tasks", "cycle");
}
