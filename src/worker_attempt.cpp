#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <set>
#include <condition_variable>
#include <thread>

#include "coffee/artifacts.hpp"
#include "coffee/executor.hpp"
#include "worker_internal.hpp"

namespace coffee::detail {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Steady = std::chrono::steady_clock;

namespace {

struct Failure {
  std::string reason;
  bool retryable = true;
  int exit_code = 1;
};

// Renews the lease on its own thread while the attempt runs; trips `lost` (and cancels the command) when the
// coordinator refuses the lease or the locally tracked expiry elapses without a successful renewal.
class LeaseKeeper {
 public:
  LeaseKeeper(const AttemptContext& ctx, const std::string& lease_id, int64_t lease_ms) : ctx_(ctx), lease_id_(lease_id) {
    expiry_ = Steady::now() + std::chrono::milliseconds(lease_ms);
    thread_ = std::thread([this] { loop(); });
  }
  ~LeaseKeeper() {
    {
      std::lock_guard<std::mutex> l(mu_);
      done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  bool lost() const { return lost_.load(); }
  std::string lost_reason() const { return reason_; }
  void on_lost(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(mu_);
    on_lost_ = std::move(fn);
    if (lost_.load() && on_lost_) on_lost_();
  }

 private:
  void trip(const std::string& why) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> l(mu_);
      reason_ = why;
      lost_ = true;
      fn = on_lost_;
    }
    if (fn) fn();
  }
  void loop() {
    const auto period = std::chrono::milliseconds(std::max<int64_t>(50, ctx_.options->heartbeat_ms));
    std::unique_lock<std::mutex> l(mu_);
    while (!done_) {
      cv_.wait_for(l, period, [this] { return done_; });
      if (done_) return;
      l.unlock();
      try {
        ApiResponse r = ctx_.client->post("/v1/leases/" + lease_id_ + "/renew", {{"bean_id", ctx_.bean_id}});
        if (r.ok()) {
          expiry_ = Steady::now() + std::chrono::milliseconds(std::max<int64_t>(1, r.body.value("lease_ms", int64_t{1000})));
        } else if (r.status == 409 || r.status == 403 || r.status == 404) {
          trip("coordinator refused the lease (" + r.error_code + ")");
          return;
        }
      } catch (const NetworkError&) {
        if (Steady::now() >= expiry_) {
          trip("lease expired locally while the coordinator was unreachable");
          return;
        }
      }
      l.lock();
    }
  }

  const AttemptContext& ctx_;
  std::string lease_id_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::atomic<bool> lost_{false};
  std::string reason_;
  std::function<void()> on_lost_;
  std::thread thread_;
  Steady::time_point expiry_;
};

void write_fd_all(int fd, const char* d, size_t n) {
  for (size_t off = 0; off < n;) {
    ssize_t w = ::write(fd, d + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw Error("io_error", std::string("cannot write input: ") + std::strerror(errno));
    }
    off += static_cast<size_t>(w);
  }
}

// Download, hash and place every input below `ws` (never following symlinks, destinations created exclusively).
std::optional<Failure> stage_inputs(const AttemptContext& ctx, const std::string& lease, const json& inputs, const fs::path& ws) {
  // The declared inputs alone must fit the per-attempt workspace cap; checked before anything is downloaded.
  uint64_t declared = 0;
  for (const auto& in : inputs)
    if (in["size"].is_number_integer()) declared += in["size"].get<uint64_t>();
  if (declared > ctx.options->max_workspace_bytes)
    return Failure{"declared inputs total " + std::to_string(declared) + " bytes, above the per-attempt workspace limit of " +
                       std::to_string(ctx.options->max_workspace_bytes) + " bytes",
                   false, 1};
  for (size_t i = 0; i < inputs.size(); i++) {
    const json& in = inputs[i];
    if (!in["sha256"].is_string() || !in["size"].is_number_integer()) return Failure{"input '" + in["target"].get<std::string>() + "' has no recorded digest", false, 1};
    const std::string sha = in["sha256"].get<std::string>(), target = in["target"].get<std::string>();
    const uint64_t size = in["size"].get<uint64_t>();
    int fd = -1;
    try {
      fd = create_confined_file(ws, target, 0600);
    } catch (const Error& e) {
      return Failure{std::string("unsafe input target: ") + e.what(), false, 1};
    }
    Sha256Stream hash;
    uint64_t got = 0;
    std::string write_error;
    ApiResponse r;
    try {
      r = ctx.client->get_stream("/v1/leases/" + lease + "/inputs/" + std::to_string(i),
                                 [&](const char* d, size_t n) {
                                   try {
                                     write_fd_all(fd, d, n);
                                   } catch (const Error& e) {
                                     write_error = e.what();
                                     return false;
                                   }
                                   hash.update(d, n);
                                   got += n;
                                   return got <= size;
                                 },
                                 {{protocol::kHeaderBean, ctx.bean_id}});
    } catch (const NetworkError& e) {
      ::close(fd);
      return Failure{std::string("input transfer failed: ") + e.what(), true, 1};
    }
    ::close(fd);
    if (!write_error.empty()) return Failure{write_error, true, 1};
    if (r.status != 200) return Failure{"input '" + target + "' download refused: " + r.error_code + " " + r.error_message, true, 1};
    if (got != size || hash.finish() != sha) return Failure{"input '" + target + "' failed verification (digest or size mismatch)", true, 1};
  }
  return std::nullopt;
}

std::string read_log(const fs::path& p, uint64_t cap) {
  try {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec) return {};
    std::string s = read_file_limited(p, static_cast<size_t>(size));
    if (s.size() > cap) s.resize(cap);
    return s;
  } catch (const Error&) {
    return {};
  }
}

}  // namespace

void run_attempt(const AttemptContext& ctx, const json& claim) {
  const WorkerOptions& opts = *ctx.options;
  const std::string lease = claim["lease_id"].get<std::string>();
  const std::string job = claim["job_id"].get<std::string>(), task_id = claim["task_id"].get<std::string>();
  const int attempt = claim["attempt"].get<int>();
  const json& task = claim["task"];
  fs::path attempt_dir = opts.work_dir / "attempts" / (job + "." + task_id + "." + std::to_string(attempt) + "." + random_hex(4));
  fs::path ws = attempt_dir / "work", logs = attempt_dir / "log";
  auto cleanup = [&] {
    if (!opts.keep_attempt_dirs) {
      std::error_code ec;
      fs::remove_all(attempt_dir, ec);
    }
  };
  try {
    std::error_code ec;
    fs::create_directories(ws, ec);
    fs::create_directories(logs, ec);
    ::chmod(attempt_dir.c_str(), 0700);
    ::chmod(ws.c_str(), 0700);

    LeaseKeeper keeper(ctx, lease, claim.value("lease_ms", int64_t{10000}));
    std::unique_ptr<Execution> exec;
    std::mutex exec_mu;
    auto cancel_exec = [&] {
      std::lock_guard<std::mutex> l(exec_mu);
      if (exec) exec->cancel();
    };
    keeper.on_lost(cancel_exec);

    std::optional<Failure> failure;
    ExecResult result;
    bool ran = false;

    failure = stage_inputs(ctx, lease, claim["inputs"], ws);
    if (!failure && !keeper.lost()) {
      ExecSpec spec;
      spec.supervisor_exe = opts.supervisor_exe.empty() ? current_executable() : opts.supervisor_exe;
      for (const auto& a : task["command"]) spec.argv.push_back(a.get<std::string>());
      spec.workdir = ws;
      spec.log_dir = logs;
      spec.limits.timeout_ms = static_cast<int64_t>(claim["timeout_seconds"].get<int>()) * 1000;
      spec.limits.memory_bytes = static_cast<uint64_t>(claim["memory_mb"].get<int64_t>()) << 20;
      spec.limits.cpu_ids = ctx.cpu_ids;
      spec.limits.max_file_bytes = opts.max_artifact_bytes;
      spec.limits.max_log_bytes = opts.max_log_bytes;
      spec.limits.max_workspace_bytes = opts.max_workspace_bytes;
      spec.env = {{"COFFEE_JOB_ID", job}, {"COFFEE_TASK_ID", task_id}, {"COFFEE_ATTEMPT", std::to_string(attempt)}, {"COFFEE_WORKDIR", ws.string()}};
      {
        std::lock_guard<std::mutex> l(exec_mu);
        exec = Execution::start(spec);
        if (keeper.lost() || (ctx.stopping && ctx.stopping->load())) exec->cancel();
      }
      // A shutting-down Bean cancels the command and reports the attempt as retryable.
      std::atomic<bool> finished{false};
      std::thread stop_watch([&] {
        while (!finished.load()) {
          if (ctx.stopping && ctx.stopping->load()) {
            cancel_exec();
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      });
      result = exec->wait();
      finished = true;
      stop_watch.join();
      ran = true;
    }

    if (keeper.lost()) {
      ctx.log("attempt " + job + "/" + task_id + "#" + std::to_string(attempt) + " abandoned: " + keeper.lost_reason());
      cleanup();
      return;
    }
    std::string stop_reason;
    if (ran && !failure) {
      if (!result.supervisor_error.empty()) failure = Failure{"supervisor failure: " + result.supervisor_error, true, 1};
      else if (result.workspace_exceeded)
        failure = Failure{"workspace limit exceeded: " + result.workspace_problem + " (measured " + std::to_string(result.workspace_bytes) + " bytes, limit " +
                              std::to_string(opts.max_workspace_bytes) + " bytes per attempt); process tree killed",
                          false, 1};
      else if (!result.spawn_error.empty()) failure = Failure{"cannot start command: " + result.spawn_error, false, 127};
      else if (result.timed_out) failure = Failure{"timed out after " + std::to_string(claim["timeout_seconds"].get<int>()) + "s (process tree killed)", true, 124};
      else if (result.cancelled) failure = Failure{"worker stopped while the command was running", true, 143};
      else if (result.parent_lost) failure = Failure{"command stopped because the supervisor lost its Bean", true, 143};
      else if (result.term_signal != 0) failure = Failure{"command killed by signal " + std::to_string(result.term_signal), true, 128 + result.term_signal};
      else if (result.exit_code != 0) failure = Failure{"command exited with status " + std::to_string(result.exit_code), true, result.exit_code};
    }

    // Verify and upload declared outputs (only for a command that succeeded).
    json outputs = json::array();
    if (ran && !failure) {
      std::set<std::string> uploaded;
      for (const auto& o : task["outputs"]) {
        const std::string path = o.get<std::string>();
        try {
          FileDigest d = digest_confined_regular(ws, path, opts.max_artifact_bytes);
          outputs.push_back({{"path", path}, {"size", d.size}, {"sha256", d.sha256}});
        } catch (const Error& e) {
          if (e.code() == "not_found") failure = Failure{"declared output '" + path + "' was not produced", false, 1};
          else if (e.code() == "unsafe_path") failure = Failure{"unsafe output '" + path + "' refused: " + e.what(), false, 1};
          else if (e.code() == "too_large") failure = Failure{"output '" + path + "' exceeds the artifact limit: " + e.what(), false, 1};
          else failure = Failure{"cannot read output '" + path + "': " + e.what(), true, 1};
          break;
        }
      }
      if (!failure) {
        for (const auto& o : outputs) {
          if (!uploaded.insert(o["sha256"].get<std::string>()).second) continue;
          int fd = -1;
          try {
            fd = open_confined_regular(ws, o["path"].get<std::string>());
            ApiResponse r = ctx.client->put_fd("/v1/leases/" + lease + "/uploads/" + o["sha256"].get<std::string>(), fd, o["size"].get<uint64_t>(),
                                               {{protocol::kHeaderBean, ctx.bean_id}});
            ::close(fd);
            fd = -1;
            if (r.status == 409 || r.status == 403 || r.status == 404) {
              ctx.log("upload refused for " + job + "/" + task_id + "#" + std::to_string(attempt) + ": " + r.error_code + "; abandoning the attempt");
              cleanup();
              return;
            }
            if (!r.ok()) {
              failure = Failure{"output upload failed: " + r.error_code + " " + r.error_message, true, 1};
              break;
            }
          } catch (const NetworkError& e) {
            if (fd >= 0) ::close(fd);
            failure = Failure{std::string("output upload failed: ") + e.what(), true, 1};
            break;
          } catch (const Error& e) {
            if (fd >= 0) ::close(fd);
            failure = Failure{std::string("output changed or unreadable: ") + e.what(), false, 1};
            break;
          }
        }
      }
    }

    // Logs first (best effort), then the fenced completion.
    for (const char* stream : {"stdout", "stderr"}) {
      std::string text = read_log(logs / (std::string(stream) + ".log"), std::min<uint64_t>(opts.max_log_bytes, protocol::kMaxLogBytes));
      if (text.empty()) continue;
      try {
        ctx.client->put_text("/v1/leases/" + lease + "/logs/" + stream, text, {{protocol::kHeaderBean, ctx.bean_id}});
      } catch (const NetworkError&) {
      }
    }
    json body = {{"bean_id", ctx.bean_id}, {"success", !failure}, {"exit_code", failure ? failure->exit_code : 0}};
    if (failure) {
      body["reason"] = failure->reason;
      body["retryable"] = failure->retryable;
    } else {
      body["outputs"] = outputs;
    }
    for (int tries = 0; tries < 20; tries++) {
      try {
        ApiResponse r = ctx.client->post("/v1/leases/" + lease + "/complete", body);
        if (!r.ok()) ctx.log("completion of " + job + "/" + task_id + "#" + std::to_string(attempt) + " refused: " + r.error_code + " " + r.error_message);
        break;
      } catch (const NetworkError& e) {
        if (keeper.lost()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<int64_t>(100, opts.heartbeat_ms / 2)));
      }
    }
  } catch (const std::exception& e) {
    ctx.log(std::string("attempt failed internally: ") + e.what());
    try {
      ctx.client->post("/v1/leases/" + lease + "/complete",
                       {{"bean_id", ctx.bean_id}, {"success", false}, {"exit_code", 1}, {"reason", std::string("worker error: ") + e.what()}, {"retryable", true}});
    } catch (const std::exception&) {
    }
  }
  cleanup();
}

}  // namespace coffee::detail
