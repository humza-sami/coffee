// Coffee durable state: state-directory initialization and the transactional SQLite store.
//
// Concurrency: a Store owns one SQLite connection guarded by a mutex; every public method is
// thread-safe and runs as one BEGIN IMMEDIATE transaction that is committed before it returns.
// Several Store objects/processes may share one state directory (SQLite busy handling applies).
//
// Time: all timestamps are milliseconds since the Unix epoch from the injected Clock.
//
// Lifecycle (tasks and jobs): pending -> running -> succeeded | failed | cancelled.
//  * A failed/expired/abandoned attempt returns the task to pending while attempts remain
//    (bounded by max_attempts), otherwise the task fails ("max attempts exhausted").
//  * A task whose dependency failed fails with "dependency_failed: <id>"; one whose dependency was
//    cancelled is cancelled. A job is succeeded when all tasks succeeded, failed when all tasks are
//    terminal and at least one failed, cancelled after cancel(), running once any task has started.
//  * Every claim creates an attempt with a unique unguessable lease id. complete/renew are accepted
//    only for the current, running, unexpired, non-cancelled lease held by the stated Bean.
//  * Opening/reopening a Store (restart or an additional connection) never changes any lease
//    timestamp: valid leases keep their original expiry, expired leases stay expired and are reaped
//    (reservation released, retry state recovered) by the next serialized mutating call. There is no
//    restart grace.
//  * Resource use of a Bean is derived from its running attempts inside the claim transaction, so
//    it can never be double-allocated or negative. Resources are accounting slots, not enforcement.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "coffee/model.hpp"
#include "coffee/util.hpp"

namespace coffee {

// ---------------------------------------------------------------- state directory

struct InitResult {
  std::filesystem::path state_dir;
  std::string cluster_id;
  bool created = false;               // false when the directory was already initialized
  bool token_created = false;
  bool database_created = false;
  std::filesystem::path token_path;   // DIR/token (mode 0600); the token itself is never returned
  std::filesystem::path database_path;
};

// Create DIR (0700), DIR/config.json (cluster id), DIR/token (64 hex chars, 0600), DIR/coffee.db and
// DIR/artifacts/{inputs,outputs,logs,tmp}. Re-running is idempotent: existing cluster id, token and
// database content are never overwritten (a too-permissive token mode is tightened to 0600).
// Throws Error("io_error"/"conflict") when existing files are unreadable or corrupt.
InitResult init_state_dir(const std::filesystem::path& state_dir);

// Read the cluster token of an initialized state directory (throws Error("not_found") if missing).
std::string read_cluster_token(const std::filesystem::path& state_dir);

// ---------------------------------------------------------------- data types

struct StoreOptions {
  std::shared_ptr<const Clock> clock;       // null -> SystemClock
  int busy_timeout_ms = 5000;               // SQLite busy handler
  int64_t lease_ms = 10'000;                // lease duration granted at claim and on every renew
  int64_t heartbeat_timeout_ms = 10'000;    // Bean without heartbeat for this long reports "unreachable"
  int64_t deadline_grace_ms = 30'000;       // slack added to timeout_seconds for staging/transfer
};

struct BeanRegistration {
  std::string id;                           // empty -> generated "bean-<hex>"; reuse to re-register
  std::string name;                         // display name, defaults to the id
  int cpus = 1;                             // CPU slots (accounting, not physical pooling)
  int64_t memory_mb = 0;                    // memory budget advertised by the Bean
  nlohmann::json attributes = nlohmann::json::object();  // free-form (address, labels, ...)
};

struct BeanInfo {
  std::string id, name;
  int cpus = 0;
  int64_t memory_mb = 0;
  int cpus_used = 0;
  int64_t memory_mb_used = 0;
  std::string state;                        // "online" | "unreachable" | "draining"
  bool draining = false;
  int64_t registered_at_ms = 0, last_heartbeat_ms = 0;
  nlohmann::json attributes = nlohmann::json::object();
  nlohmann::json to_json() const;
};

struct SubmitResult {
  std::string job_id;
  bool created = true;                      // false: same id + identical workload already existed
  size_t task_count = 0;
};

struct Claim {
  std::string job_id, task_id;
  int attempt = 0;                          // 1-based attempt number of this task
  std::string lease_id;                     // bearer secret for renew/complete; keep out of logs
  std::string bean_id;
  int64_t lease_expires_at_ms = 0;
  int64_t deadline_at_ms = 0;               // claim time + timeout_seconds + deadline_grace_ms
  int cpus = 0;
  int64_t memory_mb = 0;
  int timeout_seconds = 0;
  nlohmann::json task;                      // normalized task object (command, inputs, outputs, ...)
  // Resolved input manifest, one entry per task input, in declaration order:
  //   {"kind":"file","source","target","sha256","size"}                      local file staged with the job
  //   {"kind":"dependency","from_task","output","target","sha256","size"}    accepted output of a succeeded producer
  // sha256/size are null when no digest is recorded (jobs submitted without `input_files`); the network
  // layer never admits such jobs. Dependency entries come only from the producer's accepted result.
  nlohmann::json inputs = nlohmann::json::array();
};

// Identity of an attempt looked up by lease id (never exposes the lease id itself).
struct LeaseInfo {
  std::string job_id, task_id, bean_id;
  int attempt = 0;
  std::string state;                        // attempt state: running|succeeded|failed|expired|timed_out|cancelled|abandoned
  int64_t lease_expires_at_ms = 0, deadline_at_ms = 0;
  nlohmann::json task;                      // normalized task object (declared outputs, ...)
  nlohmann::json inputs = nlohmann::json::array();  // resolved input manifest, as in Claim::inputs
};

enum class LeaseStatus {
  Ok,
  UnknownLease,       // no such lease id
  WrongBean,          // lease exists but belongs to another Bean
  NotRunning,         // attempt already finished/superseded/abandoned
  Expired,            // lease timed out (the task may have been re-queued or re-claimed)
  DeadlineExceeded,   // attempt exceeded its wall-clock deadline
  Cancelled           // job/task cancelled
};
const char* to_string(LeaseStatus s) noexcept;

struct LeaseResult {
  LeaseStatus status = LeaseStatus::UnknownLease;
  std::string detail;
  int64_t lease_expires_at_ms = 0;          // renew only: the new expiry
  std::string task_state;                   // task state after the call (when the task is known)
  bool ok() const noexcept { return status == LeaseStatus::Ok; }
};

struct CompleteRequest {
  std::string lease_id, bean_id;
  bool success = false;
  int exit_code = 0;
  std::string reason;                       // failure description (recorded in attempt history)
  bool retryable = true;                    // false: a failure fails the task without further attempts
  nlohmann::json outputs = nlohmann::json::array();  // accepted result metadata, e.g. [{path,size,sha256}]
  // Runs inside the commit transaction after the lease was validated and before COMMIT, so accepted
  // artifacts can be published atomically with the state change. If it throws, nothing is committed
  // and the exception propagates. Called only for accepted successful completions. Because it may be
  // slow, the clock, lease expiry and deadline are checked again after it returns; if the attempt
  // expired/timed out meanwhile the completion is rejected (Expired/DeadlineExceeded), no accepted
  // metadata is recorded and anything the hook published stays unreferenced.
  std::function<void()> before_commit;
};

struct CancelResult {
  bool found = false;
  int tasks_cancelled = 0;
  int attempts_cancelled = 0;
  std::string job_state;
};

// ---------------------------------------------------------------- store

class Store {
 public:
  // Opens DIR/coffee.db of an initialized state directory (see init_state_dir), creating/migrating
  // tables. Throws Error("not_found") if the directory is not initialized, Error("db_error") otherwise.
  static std::unique_ptr<Store> open(const std::filesystem::path& state_dir, StoreOptions options = {});
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  const std::string& cluster_id() const;
  const std::filesystem::path& state_dir() const;
  int64_t now_ms() const;

  // Persist a normalized (or normalizable) workload. `job_id` may be empty (generated "job-<hex>").
  // `input_files` maps each local `source` path to {"sha256","size"} of the staged content.
  // Resubmitting the same id with an identical workload and files is a no-op (created=false); a
  // different workload or file set under the same id throws Error("conflict"). Invalid documents throw
  // WorkloadError; malformed digests throw Error("invalid_argument").
  SubmitResult submit(const nlohmann::json& workload, const std::string& job_id = {}, const nlohmann::json& input_files = nlohmann::json::object());

  // Register (or re-register) a Bean. Re-registering an existing id replaces its capacity, marks the
  // previous process's running attempts "abandoned" (re-queued if attempts remain) and heartbeats.
  BeanInfo register_bean(const BeanRegistration& registration);
  // Liveness heartbeat. Returns false when the Bean is unknown (caller must register again).
  bool heartbeat(const std::string& bean_id);
  // Drain: no new claims; running attempts continue. Returns false for an unknown Bean.
  bool set_draining(const std::string& bean_id, bool draining);
  std::vector<BeanInfo> list_beans();

  // Atomically pick the first deterministic fit and reserve its resources.
  // Order: job submission order, then task declaration order. Eligible: pending, all dependencies
  // succeeded, job not cancelled, Bean not draining, free cpus/memory >= request. Counts as a
  // heartbeat. Returns std::nullopt when nothing is eligible. Throws Error("not_found") for an unknown Bean.
  std::optional<Claim> claim(const std::string& bean_id);

  // Extend a valid lease to now + lease_ms (also counts as a heartbeat of its Bean).
  LeaseResult renew(const std::string& lease_id, const std::string& bean_id);

  // Finish an attempt. Success is accepted only for a valid lease (see header comment); on success the
  // outputs metadata becomes the task's accepted result and is never replaced. A failure re-queues or
  // fails the task per max_attempts/retryable. Rejected calls change no accepted state.
  LeaseResult complete(const CompleteRequest& request);

  // Read-only lookup of an attempt by lease id (no state change, no reaping).
  std::optional<LeaseInfo> lease_info(const std::string& lease_id);

  // Read-only classification of a lease for that Bean at the current time (no state change, no renewal):
  // Ok only for a running, unexpired, in-deadline lease. Used to refuse uploads/downloads of stale attempts.
  LeaseResult lease_status(const std::string& lease_id, const std::string& bean_id);

  // Read-only check (no state change): true when the lease is currently valid for that Bean.
  bool lease_valid(const std::string& lease_id, const std::string& bean_id);

  // Cancel a whole job (task_id empty) or one task (dependents are cancelled too). Running attempts
  // are marked cancelled and their leases stop being valid immediately.
  CancelResult cancel(const std::string& job_id, const std::string& task_id = {}, const std::string& reason = "cancelled by owner");

  // Expire leases past lease_expires_at and attempts past their deadline; re-queue or fail their
  // tasks. Called automatically by every mutating method. Returns the number of attempts ended.
  int reap_expired();

  // Reporting (JSON; lease ids are never included).
  std::optional<nlohmann::json> job_status(const std::string& job_id);   // job + tasks + attempt history
  nlohmann::json list_jobs();                                            // array of job summaries
  // Why a task is (not) running: dependency blockers, per-Bean fit reasons, unsatisfiable flag.
  std::optional<nlohmann::json> explain_task(const std::string& job_id, const std::string& task_id);

 private:
  struct Impl;
  explicit Store(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace coffee
