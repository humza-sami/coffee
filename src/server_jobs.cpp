// Coordinator routes: Beans, jobs, accepted outputs, logs, staged inputs.
#include <fcntl.h>
#include <unistd.h>

#include <set>

#include "server_internal.hpp"

namespace coffee::detail {

namespace {

const char* kId = "([A-Za-z0-9][A-Za-z0-9._-]{0,62})";

std::string route(const std::string& tail) { return std::string("/v1/") + tail; }

// Stream a published blob after re-verifying its digest (a tampered blob is never served).
void serve_blob(ServerCtx& ctx, BlobKind kind, const std::string& sha, uint64_t size, httplib::Response& res) {
  ctx.artifacts->verify(kind, sha, size);
  auto path = ctx.artifacts->find(kind, sha, size);
  if (!path) throw Error("artifact_corrupt", "stored artifact " + sha + " is missing");
  int fd = ::open(path->c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw Error("artifact_corrupt", "stored artifact " + sha + " cannot be opened");
  res.status = 200;
  res.set_header(protocol::kHeaderSha256, sha);
  res.set_content_provider(
      size, "application/octet-stream",
      [fd](size_t offset, size_t length, httplib::DataSink& sink) {
        char buf[65536];
        size_t want = std::min(length, sizeof buf);
        ssize_t r = ::pread(fd, buf, want, static_cast<off_t>(offset));
        if (r <= 0) return false;
        return sink.write(buf, static_cast<size_t>(r));
      },
      [fd](bool) { ::close(fd); });
}

json bean_json(Store& store, const std::string& id) {
  for (auto& b : store.list_beans())
    if (b.id == id) return b.to_json();
  return nullptr;
}

}  // namespace

void register_bean_routes(httplib::Server& s, ServerCtx& ctx) {
  Store* store = ctx.store;
  s.Post(route("beans"), guard_reader([store](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    BeanRegistration reg;
    reg.id = get_string(b, "id", "");
    reg.name = get_string(b, "name", "");
    int64_t cpus = get_int(b, "cpus", 0, true);
    if (cpus < 1 || cpus > 4096) throw Error("invalid_argument", "'cpus' must be between 1 and 4096");
    reg.cpus = static_cast<int>(cpus);
    reg.memory_mb = get_int(b, "memory_mb", 0, true);
    if (b.contains("attributes")) {
      if (!b["attributes"].is_object()) throw Error("invalid_argument", "'attributes' must be an object");
      reg.attributes = b["attributes"];
    }
    reg.attributes["remote_addr"] = req.remote_addr;
    send_json(res, 200, store->register_bean(reg).to_json());
  }));
  s.Get(route("beans"), guard([store](const httplib::Request&, httplib::Response& res) {
    json out = json::array();
    for (auto& b : store->list_beans()) out.push_back(b.to_json());
    send_json(res, 200, {{"beans", out}});
  }));
  s.Post(route(std::string("beans/") + kId + "/heartbeat"), guard([store](const httplib::Request& req, httplib::Response& res) {
    if (!store->heartbeat(req.matches[1])) return send_error(res, "unknown_bean", "unknown Bean; register again");
    send_json(res, 200, {{"ok", true}});
  }));
  s.Post(route(std::string("beans/") + kId + "/drain"), guard_reader([store](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    std::string id = req.matches[1];
    if (!store->set_draining(id, get_bool(b, "draining", true))) return send_error(res, "unknown_bean", "unknown Bean '" + id + "'");
    send_json(res, 200, bean_json(*store, id));
  }));
}

void register_job_routes(httplib::Server& s, ServerCtx& ctx) {
  Store* store = ctx.store;
  ArtifactStore* artifacts = ctx.artifacts;
  const uint64_t max_artifact = ctx.opts.max_artifact_bytes;

  // staged inputs: content addressed, verified while streaming
  s.Put(route("inputs/([^/]+)"), guard_reader([&ctx, artifacts](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    const std::string sha = req.matches[1];
    if (!is_sha256_hex(sha)) return send_error(res, "invalid_argument", "digest must be 64 lowercase hex characters");
    const std::string scope = "inputs-" + random_hex(8);
    auto pin = artifacts->pin_scope(scope);
    auto up = artifacts->begin_upload(scope);
    bool over = false;
    bool complete = rd([&](const char* d, size_t n) {
      try {
        up->write(d, n);
      } catch (const Error&) {
        over = true;
        return false;
      }
      return true;
    });
    if (over) return send_error(res, "too_large", "input exceeds the artifact limit of " + std::to_string(ctx.opts.max_artifact_bytes) + " bytes");
    if (!complete) return;  // transfer interrupted: the Upload destructor removes the temp file
    const uint64_t size = up->size();
    // Always go through the common verified, durable publication: an existing valid blob is not proof that an earlier
    // PUT ever finished its file and directory sync, so reuse re-syncs (and repairs) before it is acknowledged.
    const bool existed = artifacts->find(BlobKind::Input, sha, size).has_value();
    PublishOutcome outcome = artifacts->publish(BlobKind::Input, *up, sha, size);
    if (outcome == PublishOutcome::Created && !existed) return send_json(res, 201, {{"sha256", sha}, {"size", size}, {"created", true}});
    json body = {{"sha256", sha}, {"size", size}, {"created", false}};
    if (outcome == PublishOutcome::Repaired) body["repaired"] = true;
    send_json(res, 200, body);
  }));

  s.Post(route("jobs"), guard_reader([store, artifacts, max_artifact](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    if (!b.contains("workload")) throw Error("invalid_argument", "'workload' is required");
    json workload = normalize_workload(b["workload"]);  // WorkloadError -> 400 invalid_workload before any side effect
    json files = b.contains("files") ? b["files"] : json::object();
    if (!files.is_object()) throw Error("invalid_argument", "'files' must be an object");
    std::set<std::string> sources;
    for (const auto& t : workload["spec"]["tasks"])
      for (const auto& in : t["inputs"])
        if (in.contains("source")) sources.insert(in["source"].get<std::string>());
    for (const auto& src : sources)
      if (!files.contains(src)) throw Error("invalid_argument", "no digest was provided in 'files' for input source '" + src + "'");
    for (auto it = files.begin(); it != files.end(); ++it)
      if (!sources.count(it.key())) throw Error("invalid_argument", "'files' names '" + it.key() + "', which is not an input source of any task");
    json missing = json::array();
    for (auto it = files.begin(); it != files.end(); ++it) {
      const json& f = it.value();
      if (!f.is_object() || !f.contains("sha256") || !f["sha256"].is_string() || !f.contains("size") || !f["size"].is_number_integer() || f["size"].get<int64_t>() < 0)
        throw Error("invalid_argument", "files['" + it.key() + "'] needs sha256 and size");
      if (static_cast<uint64_t>(f["size"].get<int64_t>()) > max_artifact) throw Error("too_large", "input '" + it.key() + "' exceeds the artifact limit");
      if (!is_sha256_hex(f["sha256"].get<std::string>())) throw Error("invalid_argument", "files['" + it.key() + "'].sha256 is malformed");
      // Referencing an input requires proof that it is intact and durable now (not merely present): a caller can submit a
      // manifest after a failed PUT. Absent/damaged => missing_input; a failed sync throws and nothing is recorded.
      if (!artifacts->ensure_durable(BlobKind::Input, f["sha256"].get<std::string>(), static_cast<uint64_t>(f["size"].get<int64_t>()))) missing.push_back(it.key());
    }
    if (!missing.empty()) {
      json body = protocol::error_body("missing_input", "input files have not been uploaded (PUT /v1/inputs/<sha256> first)");
      body["error"]["missing"] = missing;
      return send_json(res, 409, body);
    }
    SubmitResult r = store->submit(workload, get_string(b, "job_id", ""), files);
    send_json(res, r.created ? 201 : 200, {{"job_id", r.job_id}, {"created", r.created}, {"task_count", r.task_count}});
  }));

  s.Get(route("jobs"), guard([store](const httplib::Request&, httplib::Response& res) { send_json(res, 200, {{"jobs", store->list_jobs()}}); }));
  s.Get(route(std::string("jobs/") + kId), guard([store](const httplib::Request& req, httplib::Response& res) {
    auto st = store->job_status(req.matches[1]);
    if (!st) return send_error(res, "unknown_job", "unknown job '" + std::string(req.matches[1]) + "'");
    send_json(res, 200, *st);
  }));
  s.Post(route(std::string("jobs/") + kId + "/cancel"), guard_reader([store](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    std::string reason = get_string(b, "reason", "cancelled by owner");
    if (reason.size() > 500) throw Error("invalid_argument", "'reason' longer than 500 bytes");
    CancelResult r = store->cancel(req.matches[1], get_string(b, "task_id", ""), reason);
    if (!r.found) return send_error(res, "unknown_job", "unknown job or task");
    send_json(res, 200, {{"job_state", r.job_state}, {"tasks_cancelled", r.tasks_cancelled}, {"attempts_cancelled", r.attempts_cancelled}});
  }));
  s.Get(route(std::string("jobs/") + kId + "/tasks/" + kId + "/explain"), guard([store](const httplib::Request& req, httplib::Response& res) {
    auto e = store->explain_task(req.matches[1], req.matches[2]);
    if (!e) return send_error(res, "unknown_task", "unknown job or task");
    send_json(res, 200, *e);
  }));

  // accepted outputs: only paths recorded in the task's committed result can be fetched
  s.Get(route(std::string("jobs/") + kId + "/tasks/" + kId + "/outputs"), guard([&ctx, store](const httplib::Request& req, httplib::Response& res) {
    auto st = store->job_status(req.matches[1]);
    if (!st) return send_error(res, "unknown_job", "unknown job");
    const std::string want = req.get_param_value("path");
    for (const auto& t : (*st)["tasks"]) {
      if (t["id"] != std::string(req.matches[2])) continue;
      if (t["result"].is_array())
        for (const auto& o : t["result"])
          if (o["path"] == want) return serve_blob(ctx, BlobKind::Output, o["sha256"].get<std::string>(), o["size"].get<uint64_t>(), res);
      return send_error(res, "no_such_output", "task has no accepted output '" + want + "'");
    }
    send_error(res, "unknown_task", "unknown task");
  }));

  s.Get(route(std::string("jobs/") + kId + "/tasks/" + kId + "/logs"), guard([store, artifacts](const httplib::Request& req, httplib::Response& res) {
    auto st = store->job_status(req.matches[1]);
    if (!st) return send_error(res, "unknown_job", "unknown job");
    std::string stream = req.has_param("stream") ? req.get_param_value("stream") : "stdout";
    if (stream != "stdout" && stream != "stderr") return send_error(res, "invalid_argument", "stream must be stdout or stderr");
    for (const auto& t : (*st)["tasks"]) {
      if (t["id"] != std::string(req.matches[2])) continue;
      int attempt = static_cast<int>(t["attempts_used"].get<int64_t>());
      if (req.has_param("attempt")) {
        std::string a = req.get_param_value("attempt");
        if (a.empty() || a.size() > 6 || a.find_first_not_of("0123456789") != std::string::npos) return send_error(res, "invalid_argument", "attempt must be a positive integer");
        attempt = std::stoi(a);
      }
      std::filesystem::path p = artifacts->log_path(req.matches[1], req.matches[2], attempt, stream);
      std::error_code ec;
      if (!std::filesystem::is_regular_file(p, ec)) return send_error(res, "no_logs", "no " + stream + " log for attempt " + std::to_string(attempt));
      res.status = 200;
      res.set_content(read_file_limited(p, protocol::kMaxLogBytes + 1), "text/plain; charset=utf-8");
      return;
    }
    send_error(res, "unknown_task", "unknown task");
  }));
}

}  // namespace coffee::detail
