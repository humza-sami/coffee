#include "coffee/server.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

#include "server_internal.hpp"

namespace coffee {

namespace detail {

void send_json(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, const std::string& code, const std::string& message, const std::string& field) {
  send_json(res, protocol::status_for_code(code), protocol::error_body(code, message, field));
}

void send_lease_error(httplib::Response& res, const LeaseResult& r) {
  std::string code = to_string(r.status);
  if (r.status == LeaseStatus::UnknownLease) code = "unknown_lease";
  send_error(res, code, r.detail.empty() ? code : r.detail);
  if (r.status != LeaseStatus::Ok) res.set_header("Connection", "close");  // an unread request body must not poison keep-alive
}

namespace {
void handle_exception(httplib::Response& res) {
  try {
    throw;
  } catch (const WorkloadError& e) {
    send_error(res, "invalid_workload", e.detail(), e.field());
  } catch (const Error& e) {
    int status = protocol::status_for_code(e.code());
    if (e.code() == "artifact_corrupt") {
      send_json(res, 500, protocol::error_body("artifact_corrupt", e.what()));
    } else if (status == 500) {
      std::fprintf(stderr, "coffee serve: internal error (%s): %s\n", e.code().c_str(), e.what());
      send_json(res, 500, protocol::error_body("internal_error", "internal error"));
    } else {
      send_error(res, e.code(), e.what());
    }
  } catch (const nlohmann::json::exception& e) {
    send_error(res, "invalid_argument", std::string("malformed field: ") + e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "coffee serve: unexpected error: %s\n", e.what());
    send_error(res, "internal_error", "internal error");
  } catch (...) {
    send_error(res, "internal_error", "internal error");
  }
}
}  // namespace

httplib::Server::Handler guard(Handler h) {
  return [h = std::move(h)](const httplib::Request& req, httplib::Response& res) {
    try {
      h(req, res);
    } catch (...) {
      handle_exception(res);
    }
  };
}

httplib::Server::HandlerWithContentReader guard_reader(ReaderHandler h) {
  return [h = std::move(h)](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
    try {
      h(req, res, reader);
    } catch (...) {
      handle_exception(res);
    }
  };
}

json read_json_body(const httplib::Request&, const httplib::ContentReader& reader, size_t limit) {
  std::string body;
  bool over = false;
  bool ok = reader([&](const char* data, size_t n) {
    if (body.size() + n > limit) {
      over = true;
      return false;
    }
    body.append(data, n);
    return true;
  });
  if (over) throw Error("too_large", "request body exceeds " + std::to_string(limit) + " bytes");
  if (!ok) throw Error("invalid_json", "request body could not be read");
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded()) throw Error("invalid_json", "request body is not valid JSON");
  if (!j.is_object()) throw Error("invalid_json", "request body must be a JSON object");
  return j;
}

std::string bean_header(const httplib::Request& req) { return req.get_header_value(protocol::kHeaderBean); }

std::string get_string(const json& j, const char* key, const std::string& def, bool required) {
  if (!j.contains(key) || j[key].is_null()) {
    if (required) throw Error("invalid_argument", std::string("'") + key + "' is required");
    return def;
  }
  if (!j[key].is_string()) throw Error("invalid_argument", std::string("'") + key + "' must be a string");
  return j[key].get<std::string>();
}

int64_t get_int(const json& j, const char* key, int64_t def, bool required) {
  if (!j.contains(key) || j[key].is_null()) {
    if (required) throw Error("invalid_argument", std::string("'") + key + "' is required");
    return def;
  }
  if (!j[key].is_number_integer()) throw Error("invalid_argument", std::string("'") + key + "' must be an integer");
  return j[key].get<int64_t>();
}

bool get_bool(const json& j, const char* key, bool def) {
  if (!j.contains(key) || j[key].is_null()) return def;
  if (!j[key].is_boolean()) throw Error("invalid_argument", std::string("'") + key + "' must be a boolean");
  return j[key].get<bool>();
}

}  // namespace detail

// ------------------------------------------------------------------ Coordinator

namespace {
bool constant_time_equal(const std::string& a, const std::string& b) {
  unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}
}  // namespace

struct Coordinator::Impl {
  detail::ServerCtx ctx;
  std::unique_ptr<Store> store;
  std::unique_ptr<ArtifactStore> artifacts;
  std::string token;
  httplib::Server server;
  std::thread serve_thread, reaper_thread;
  std::atomic<bool> stopping{false};
  bool started = false;
};

Coordinator::Coordinator(CoordinatorOptions options) : impl_(std::make_unique<Impl>()) {
  if (!protocol::is_loopback_host(options.host) && !options.allow_plaintext_nonloopback)
    throw Error("insecure_listen", "refusing to listen on non-loopback address '" + options.host +
                                       "' over plaintext HTTP (this build has no TLS). Use a loopback address behind an SSH/WireGuard/Tailscale tunnel, "
                                       "or pass --allow-plaintext-nonloopback if the network is already encrypted and trusted (the flag adds no encryption).");
  StoreOptions so;
  so.clock = options.clock;
  so.lease_ms = options.lease_ms;
  so.heartbeat_timeout_ms = options.heartbeat_timeout_ms;
  so.deadline_grace_ms = options.deadline_grace_ms;
  impl_->store = Store::open(options.state_dir, so);
  impl_->artifacts = std::make_unique<ArtifactStore>(options.state_dir / "artifacts", options.max_artifact_bytes);
  impl_->token = read_cluster_token(options.state_dir);
  if (impl_->token.size() < 32) throw Error("conflict", "cluster token in " + options.state_dir.string() + " is too short; refusing to serve");
  impl_->ctx.opts = options;
  impl_->ctx.store = impl_->store.get();
  impl_->ctx.artifacts = impl_->artifacts.get();
}

Coordinator::~Coordinator() { stop(); }

Store& Coordinator::store() { return *impl_->store; }

int Coordinator::start() {
  Impl& m = *impl_;
  const CoordinatorOptions& o = m.ctx.opts;
  httplib::Server& s = m.server;

  s.set_payload_max_length(static_cast<size_t>(o.max_artifact_bytes) + protocol::kMaxJsonBytes);
  s.set_read_timeout(o.read_timeout_s);
  s.set_write_timeout(o.write_timeout_s);
  s.set_keep_alive_max_count(100);
  s.set_keep_alive_timeout(5);
  size_t threads = o.worker_threads, queued = o.max_queued_requests;
  s.new_task_queue = [threads, queued] { return new httplib::ThreadPool(threads, threads * 4, queued); };

  s.set_pre_routing_handler([&m](const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/v1/health") return httplib::Server::HandlerResponse::Unhandled;
    std::string auth = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.rfind(prefix, 0) != 0 || !constant_time_equal(auth.substr(prefix.size()), m.token)) {
      detail::send_error(res, "unauthorized", "missing or invalid bearer token");
      res.set_header("WWW-Authenticate", "Bearer");
      res.set_header("Connection", "close");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  s.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty() || res.get_header_value("Content-Type") != "application/json") {
      std::string code = res.status == 404 ? "not_found" : res.status == 413 ? "too_large" : res.status == 405 ? "method_not_allowed" : "bad_request";
      detail::json b = protocol::error_body(code, "request failed with HTTP status " + std::to_string(res.status));
      res.set_content(b.dump(), "application/json");
    }
  });
  s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
    detail::send_error(res, "internal_error", "internal error");
  });

  s.Get("/v1/health", detail::guard([](const httplib::Request&, httplib::Response& res) { detail::send_json(res, 200, {{"status", "ok"}}); }));
  s.Get("/v1/version", detail::guard([&m](const httplib::Request&, httplib::Response& res) {
    detail::send_json(res, 200, {{"name", "coffee"}, {"version", kVersion}, {"api_version", kApiVersion}, {"cluster_id", m.store->cluster_id()},
                                 {"lease_ms", m.ctx.opts.lease_ms}, {"heartbeat_timeout_ms", m.ctx.opts.heartbeat_timeout_ms},
                                 {"max_artifact_bytes", m.ctx.opts.max_artifact_bytes}, {"max_json_bytes", protocol::kMaxJsonBytes},
                                 {"max_log_bytes", protocol::kMaxLogBytes}, {"transport", "http-plaintext"}});
  }));
  detail::register_bean_routes(s, m.ctx);
  detail::register_job_routes(s, m.ctx);
  detail::register_lease_routes(s, m.ctx);

  int port = o.port;
  if (o.port == 0) {
    port = s.bind_to_any_port(o.host);
    if (port < 0) throw Error("io_error", "cannot bind " + o.host + ":0");
  } else if (!s.bind_to_port(o.host, o.port)) {
    throw Error("io_error", "cannot bind " + o.host + ":" + std::to_string(o.port) + " (address in use?)");
  }
  m.started = true;
  m.serve_thread = std::thread([&m] { m.server.listen_after_bind(); });
  m.reaper_thread = std::thread([&m] {
    // Lease expiry/deadlines must progress even when nobody calls the API.
    int64_t period = std::max<int64_t>(50, std::min<int64_t>(m.ctx.opts.lease_ms / 4, 500));
    while (!m.stopping.load()) {
      try {
        m.store->reap_expired();
        m.artifacts->reap_scopes([&m](const std::string& scope) {
          auto info = m.store->lease_info(scope);
          return info && m.store->lease_status(scope, info->bean_id).ok();
        });
      } catch (const std::exception& e) {
        std::fprintf(stderr, "coffee serve: reaper error: %s\n", e.what());
      }
      for (int64_t waited = 0; waited < period && !m.stopping.load(); waited += 25) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  m.server.wait_until_ready();
  return port;
}

void Coordinator::stop() {
  Impl& m = *impl_;
  if (!m.started) return;
  m.started = false;
  m.stopping = true;
  m.server.stop();
  if (m.serve_thread.joinable()) m.serve_thread.join();
  if (m.reaper_thread.joinable()) m.reaper_thread.join();
}

}  // namespace coffee
