// Coordinator routes used by Beans: claim, renew, verified input download, upload staging, logs and fenced completion.
#include <fcntl.h>
#include <unistd.h>

#include <set>

#include "server_internal.hpp"

namespace coffee::detail {

namespace {

const char* kId = "([A-Za-z0-9][A-Za-z0-9._-]{0,62})";
const char* kLease = "(lease-[0-9a-f]{48})";

std::string route(const std::string& tail) { return std::string("/v1/") + tail; }

std::string lease_scope(const std::string& lease_id) { return lease_id; }

// A lease-scoped request must come from the owning Bean while the attempt is still current.
LeaseResult require_lease(Store& store, const httplib::Request& req, std::string* bean_out = nullptr) {
  std::string bean = bean_header(req);
  if (bean_out) *bean_out = bean;
  return store.lease_status(req.matches[1], bean);
}

json claim_json(const Claim& c, int64_t lease_ms) {
  return {{"job_id", c.job_id}, {"task_id", c.task_id}, {"attempt", c.attempt}, {"lease_id", c.lease_id},
          {"lease_expires_at_ms", c.lease_expires_at_ms}, {"lease_ms", lease_ms}, {"deadline_at_ms", c.deadline_at_ms},
          {"cpus", c.cpus}, {"memory_mb", c.memory_mb}, {"timeout_seconds", c.timeout_seconds}, {"task", c.task}, {"inputs", c.inputs}};
}

// outputs must name each declared output exactly once with a well-formed {path,size,sha256}
void validate_outputs(const json& declared, const json& outputs, uint64_t max_bytes) {
  if (!outputs.is_array()) throw Error("invalid_outputs", "'outputs' must be an array");
  std::set<std::string> want, got;
  for (const auto& d : declared) want.insert(d.get<std::string>());
  for (const auto& o : outputs) {
    if (!o.is_object() || !o.contains("path") || !o["path"].is_string() || !o.contains("size") || !o["size"].is_number_integer() || !o.contains("sha256") ||
        !o["sha256"].is_string())
      throw Error("invalid_outputs", "each output needs {path, size, sha256}");
    const std::string path = o["path"].get<std::string>();
    if (auto why = check_safe_relative_path(path)) throw Error("invalid_outputs", "unsafe output path '" + path + "': " + *why);
    if (!want.count(path)) throw Error("invalid_outputs", "'" + path + "' is not a declared output of this task");
    if (!got.insert(path).second) throw Error("invalid_outputs", "duplicate output '" + path + "'");
    if (!is_sha256_hex(o["sha256"].get<std::string>())) throw Error("invalid_outputs", "malformed sha256 for '" + path + "'");
    if (o["size"].get<int64_t>() < 0 || static_cast<uint64_t>(o["size"].get<int64_t>()) > max_bytes) throw Error("invalid_outputs", "size of '" + path + "' is out of range");
  }
  for (const auto& w : want)
    if (!got.count(w)) throw Error("invalid_outputs", "declared output '" + w + "' is missing from the completion");
}

}  // namespace

void register_lease_routes(httplib::Server& s, ServerCtx& ctx) {
  Store* store = ctx.store;
  ArtifactStore* artifacts = ctx.artifacts;
  const int64_t lease_ms = ctx.opts.lease_ms;
  const uint64_t max_artifact = ctx.opts.max_artifact_bytes;

  s.Post(route(std::string("beans/") + kId + "/claim"), guard([store, lease_ms](const httplib::Request& req, httplib::Response& res) {
    std::optional<Claim> c;
    try {
      c = store->claim(req.matches[1]);
    } catch (const Error& e) {
      if (e.code() == "not_found") return send_error(res, "unknown_bean", "unknown Bean; register first");
      throw;
    }
    send_json(res, 200, {{"claim", c ? claim_json(*c, lease_ms) : json(nullptr)}});
  }));

  s.Post(route(std::string("leases/") + kLease + "/renew"), guard_reader([store](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    LeaseResult r = store->renew(req.matches[1], get_string(b, "bean_id", "", true));
    if (!r.ok()) return send_lease_error(res, r);
    send_json(res, 200, {{"status", "ok"}, {"lease_expires_at_ms", r.lease_expires_at_ms}, {"lease_ms", r.lease_expires_at_ms - store->now_ms()}});
  }));

  // verified input download for the current attempt only; the lease selects the artifact, never the caller
  s.Get(route(std::string("leases/") + kLease + "/inputs/([0-9]{1,5})"), guard([&ctx, store](const httplib::Request& req, httplib::Response& res) {
    LeaseResult lr = require_lease(*store, req);
    if (!lr.ok()) return send_lease_error(res, lr);
    auto info = store->lease_info(req.matches[1]);
    if (!info) return send_error(res, "unknown_lease", "no such lease");
    size_t index = std::stoul(req.matches[2]);
    if (index >= info->inputs.size()) return send_error(res, "no_such_input", "the task has no input " + std::string(req.matches[2]));
    const json& in = info->inputs[index];
    if (!in["sha256"].is_string()) return send_error(res, "no_such_input", "input has no recorded digest");
    const std::string sha = in["sha256"].get<std::string>();
    const uint64_t size = in["size"].get<uint64_t>();
    BlobKind kind = in["kind"] == "dependency" ? BlobKind::Output : BlobKind::Input;
    ctx.artifacts->verify(kind, sha, size);  // Error("artifact_corrupt") -> 500, never the bytes
    auto path = ctx.artifacts->find(kind, sha, size);
    int fd = path ? ::open(path->c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (fd < 0) throw Error("artifact_corrupt", "stored artifact " + sha + " is unavailable");
    res.status = 200;
    res.set_header(protocol::kHeaderSha256, sha);
    res.set_content_provider(
        size, "application/octet-stream",
        [fd](size_t offset, size_t length, httplib::DataSink& sink) {
          char buf[65536];
          ssize_t r = ::pread(fd, buf, std::min(length, sizeof buf), static_cast<off_t>(offset));
          return r > 0 && sink.write(buf, static_cast<size_t>(r));
        },
        [fd](bool) { ::close(fd); });
  }));

  // staged upload of one output blob: verified while streaming, re-fenced on completion of the transfer
  s.Put(route(std::string("leases/") + kLease + "/uploads/([0-9a-f]{64})"), guard_reader([store, artifacts](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    const std::string lease = req.matches[1], sha = req.matches[2];
    auto pin = artifacts->pin_scope(lease_scope(lease));
    LeaseResult lr = require_lease(*store, req);
    if (!lr.ok()) return send_lease_error(res, lr);
    auto up = artifacts->begin_upload(lease_scope(lease));
    bool over = false;
    bool whole = rd([&](const char* d, size_t n) {
      try {
        up->write(d, n);
      } catch (const Error&) {
        over = true;
        return false;
      }
      return true;
    });
    if (over) {
      res.set_header("Connection", "close");
      return send_error(res, "too_large", "artifact exceeds the limit of " + std::to_string(artifacts->max_artifact_bytes()) + " bytes");
    }
    if (!whole) return;  // interrupted transfer: nothing is staged, the temp file is removed
    // The owner may have cancelled or the lease expired while bytes were in flight: do not stage for such attempts.
    lr = store->lease_status(lease, bean_header(req));
    if (!lr.ok()) {
      artifacts->discard_scope(lease_scope(lease));
      return send_lease_error(res, lr);
    }
    artifacts->stage(*up, sha, up->size());
    send_json(res, 201, {{"sha256", sha}, {"size", up->size()}});
  }));

  s.Put(route(std::string("leases/") + kLease + "/logs/(stdout|stderr)"), guard_reader([store, artifacts](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    LeaseResult lr = require_lease(*store, req);
    if (!lr.ok()) return send_lease_error(res, lr);
    std::string data;
    bool over = false;
    bool whole = rd([&](const char* d, size_t n) {
      if (data.size() + n > protocol::kMaxLogBytes) return over = true, false;
      data.append(d, n);
      return true;
    });
    if (over) return send_error(res, "too_large", "log exceeds " + std::to_string(protocol::kMaxLogBytes) + " bytes");
    if (!whole) return;
    auto info = store->lease_info(req.matches[1]);
    if (!info) return send_error(res, "unknown_lease", "no such lease");
    artifacts->write_log(info->job_id, info->task_id, info->attempt, req.matches[2], data);
    send_json(res, 200, {{"size", data.size()}});
  }));

  s.Post(route(std::string("leases/") + kLease + "/complete"), guard_reader([store, artifacts, max_artifact](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& rd) {
    json b = read_json_body(req, rd);
    const std::string lease = req.matches[1];
    auto pin = artifacts->pin_scope(lease_scope(lease));
    const std::string bean = get_string(b, "bean_id", "", true);
    const bool success = get_bool(b, "success", false);
    auto info = store->lease_info(lease);
    if (!info) return send_error(res, "unknown_lease", "no such lease");
    if (info->bean_id != bean) return send_error(res, "wrong_bean", "lease belongs to a different Bean");

    CompleteRequest cr;
    cr.lease_id = lease;
    cr.bean_id = bean;
    cr.success = success;
    cr.exit_code = static_cast<int>(get_int(b, "exit_code", success ? 0 : 1));
    cr.reason = get_string(b, "reason", "");
    if (cr.reason.size() > 2000) cr.reason.resize(2000);
    cr.retryable = get_bool(b, "retryable", true);
    if (success) {
      json outputs = b.contains("outputs") ? b["outputs"] : json::array();
      validate_outputs(info->task["outputs"], outputs, max_artifact);
      // Fence first: a stale/cancelled/expired attempt is refused before any file is touched.
      LeaseResult pre = store->lease_status(lease, bean);
      if (!pre.ok()) {
        artifacts->discard_scope(lease_scope(lease));
        return send_lease_error(res, pre);
      }
      for (const auto& o : outputs)
        if (!artifacts->staged(lease_scope(lease), o["sha256"].get<std::string>(), o["size"].get<uint64_t>()))
          throw Error("missing_upload", "output '" + o["path"].get<std::string>() + "' has not been uploaded and verified for this attempt");
      cr.outputs = outputs;
      // Slow work (re-hash, link, fsync of data and directory) happens HERE, outside the Store mutex and its
      // transaction, so a stalled filesystem cannot block renewals of unrelated leases. Any failure throws before
      // metadata is touched. The blobs are immutable and content-addressed: if the lease expires or is cancelled
      // meanwhile they simply stay unreferenced, and the short fenced commit below re-validates lease, clock and
      // deadline before recording anything.
      for (const auto& o : outputs) artifacts->publish_staged(BlobKind::Output, lease_scope(lease), o["sha256"].get<std::string>(), o["size"].get<uint64_t>());
    }
    LeaseResult r = store->complete(cr);
    artifacts->discard_scope(lease_scope(lease));
    if (!r.ok()) return send_lease_error(res, r);
    send_json(res, 200, {{"status", "ok"}, {"task_state", r.task_state}});
  }));
}

}  // namespace coffee::detail
