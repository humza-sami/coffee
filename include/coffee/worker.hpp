// Coffee Bean (worker): registers with the coordinator, heartbeats, claims tasks that fit its advertised budget,
// stages verified inputs into per-attempt workspaces, runs commands under the supervisor (see executor.hpp),
// publishes verified outputs under the lease fence and reports the result.
//
// Failure behaviour: a Bean that cannot renew its lease before the locally tracked expiry cancels the command
// (the coordinator independently fences any late completion). Coordinator outages are retried; a Bean never
// completes an attempt whose lease the coordinator refused.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "coffee/client.hpp"

namespace coffee {

struct WorkerOptions {
  ClientOptions client;                     // coordinator URL + token
  std::filesystem::path work_dir;           // attempt workspaces, bean-id file (0700)
  std::string bean_id;                      // empty: read/create work_dir/bean-id
  std::string name;
  int cpus = 0;                             // CPU slots advertised; 0 = number of CPUs this process may use
  int64_t memory_mb = 0;                    // advertised memory budget; 0 = half of MemTotal
  int64_t heartbeat_ms = 2000;              // heartbeat / lease renewal period
  int64_t poll_ms = 500;                    // claim polling period when idle
  uint64_t max_artifact_bytes = 256ull << 20;  // per-file output limit (RLIMIT_FSIZE and upload limit)
  uint64_t max_log_bytes = 1 << 20;
  // Aggregate bytes one attempt may hold in its workspace (staged inputs + everything the command and its children
  // write below it). Finite by default; 0 is rejected by the CLI. Per attempt: a Bean running N attempts at once can
  // use up to N x this value, and the coordinator does not account disk across Beans. See executor.hpp for how it is
  // enforced (sampling + final post-exit check) and its limits.
  uint64_t max_workspace_bytes = 1ull << 30;
  std::string supervisor_exe;               // default: the running executable
  bool keep_attempt_dirs = false;           // keep finished workspaces (debugging)
};

class Worker {
 public:
  explicit Worker(WorkerOptions options);
  ~Worker();
  // Registers (retrying while the coordinator is unreachable) and serves until request_stop(). Returns 0 on a
  // clean stop, 1 on an unrecoverable error (e.g. permanently rejected registration).
  int run();
  void request_stop();  // async-signal-safe
  const std::string& bean_id() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace coffee
