// Coffee command execution: runs a task command under an independently exec'd supervisor process.
//
// Why a supervisor: the Bean is multi-threaded (HTTP, heartbeats, several tasks), and arbitrary child-side
// work between fork() and exec() in such a process is unsafe. The Bean therefore only calls posix_spawn() to
// start `coffee __supervise ...` (a fresh, single-threaded image). The supervisor then:
//  * becomes a child subreaper (orphaned descendants re-parent to it), arms PR_SET_PDEATHSIG(SIGTERM) and
//    watches a liveness pipe whose write end the Bean holds, so a SIGKILLed Bean is detected immediately;
//  * forks the command into its own process group with RLIMIT_AS/FSIZE/CORE/NOFILE and CPU affinity applied,
//    with stdout/stderr captured through bounded copies into log files;
//  * enforces the wall-clock timeout and cancellation (SIGTERM from the Bean);
//  * on every exit path (command exit, timeout, cancel, Bean death) kills the whole command tree: its process
//    group plus every descendant found by parent-pid ancestry (this also catches setsid()/daemonized
//    descendants because they are re-parented to the subreaper).
//
// Limits of the mechanism (documented, not hidden): if the supervisor itself is SIGKILLed, only the direct
// command dies (PDEATHSIG), not its descendants; fork bombs and RLIMIT_AS are not a security sandbox;
// RLIMIT_AS bounds address space per process, not resident memory or the task's total memory; CPU
// affinity pins to the assigned CPU ids and is not a CPU-time quota. No cgroup delegation is used.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "coffee/workspace.hpp"

namespace coffee {

struct ExecLimits {
  int64_t timeout_ms = 300'000;     // wall clock; the whole tree is killed (SIGKILL) when it elapses
  uint64_t memory_bytes = 0;        // RLIMIT_AS of the command (and inherited by descendants); 0 = unlimited
  std::vector<int> cpu_ids;         // sched_setaffinity CPU set; empty = inherit
  uint64_t max_file_bytes = 0;      // RLIMIT_FSIZE: largest file any command process can write; 0 = unlimited
  uint64_t max_log_bytes = 1 << 20; // per stream; the rest is read and discarded and reported as truncated
  int max_open_files = 1024;        // RLIMIT_NOFILE
  // Aggregate bytes allowed below `workdir` (0 = unlimited). Checked by the supervisor about every
  // kWorkspaceScanIntervalMs while the command runs and once more after the command tree is dead, before any success
  // is reported. See workspace.hpp: it counts distinct regular files below `workdir` (directory walk, symlinks never
  // followed) PLUS files held open by the command and its descendants that live below `workdir`, including ones
  // already unlinked, de-duplicated by (device, inode); it fails closed on unreadable directories, depth, entry or
  // process limits. It is userspace sampling, not a filesystem quota: a fast writer can overshoot by (write rate x
  // (interval + scan time + scheduling delay)) before it is killed, a file that is opened, filled and released between
  // two samples is only seen if it is still linked afterwards, and writes outside `workdir` (absolute paths, /tmp,
  // files renamed out while open) are not counted.
  uint64_t max_workspace_bytes = 0;
};

inline constexpr int kWorkspaceScanIntervalMs = 100;

struct ExecSpec {
  std::string supervisor_exe;                 // path of the coffee binary (see current_executable())
  std::vector<std::string> argv;              // no implicit shell
  std::filesystem::path workdir;              // must exist; the command's cwd and HOME
  std::filesystem::path log_dir;              // receives stdout.log, stderr.log and status.json; must exist
  ExecLimits limits;
  std::map<std::string, std::string> env;     // added to the minimal environment (PATH, HOME, LANG)
};

struct ExecResult {
  int exit_code = -1;               // valid when term_signal == 0
  int term_signal = 0;              // signal that terminated the command (0 = exited)
  bool timed_out = false;
  bool cancelled = false;
  bool parent_lost = false;         // the Bean died or closed the liveness pipe
  bool workspace_exceeded = false;  // aggregate workspace limit exceeded, or the workspace could not be scanned
  uint64_t workspace_bytes = 0;     // bytes measured by the last scan (final scan after the command tree is dead)
  std::string workspace_problem;    // reason when workspace_exceeded (limit exceeded / unreadable path / too many entries)
  bool stdout_truncated = false, stderr_truncated = false;
  std::string spawn_error;          // command could not be started (exec/rlimit/affinity failure)
  std::string supervisor_error;     // supervisor itself failed or vanished without a result
  bool ok() const { return supervisor_error.empty() && spawn_error.empty() && !timed_out && !cancelled && !parent_lost && !workspace_exceeded && term_signal == 0 && exit_code == 0; }
};

class Execution {
 public:
  // Starts the supervisor (and thereby the command). Throws Error("io_error") if the supervisor cannot be spawned.
  static std::unique_ptr<Execution> start(const ExecSpec& spec);
  ~Execution();  // cancels and reaps if still running
  Execution(const Execution&) = delete;
  Execution& operator=(const Execution&) = delete;

  pid_t supervisor_pid() const { return pid_; }
  void cancel();        // thread-safe, idempotent: the supervisor kills the tree and reports cancelled
  ExecResult wait();    // blocks until the supervisor exited; may be called once

 private:
  Execution() = default;
  pid_t pid_ = -1;
  int liveness_fd_ = -1;  // write end of the liveness pipe; closing it also signals parent loss
  std::filesystem::path status_path_;
  std::mutex mu_;
  bool reaped_ = false;
};

// Absolute path of the running executable (/proc/self/exe).
std::string current_executable();

// Entry point of the hidden `coffee __supervise` command; `args` excludes argv[0] and the command name.
int supervisor_main(const std::vector<std::string>& args);

}  // namespace coffee
