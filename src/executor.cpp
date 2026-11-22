#include "coffee/executor.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "coffee/util.hpp"
#include "coffee/workspace.hpp"

extern char** environ;

namespace coffee {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string current_executable() {
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (ec) throw Error("io_error", "cannot resolve /proc/self/exe: " + ec.message());
  return p.string();
}

// ================================================================== Bean side

std::unique_ptr<Execution> Execution::start(const ExecSpec& spec) {
  if (spec.argv.empty()) throw Error("invalid_argument", "empty command");
  std::vector<std::string> args = {"coffee", "__supervise", "--workdir", spec.workdir.string(), "--log-dir", spec.log_dir.string(), "--timeout-ms",
                                   std::to_string(spec.limits.timeout_ms), "--as-bytes", std::to_string(spec.limits.memory_bytes), "--fsize",
                                   std::to_string(spec.limits.max_file_bytes), "--max-log", std::to_string(spec.limits.max_log_bytes), "--nofile",
                                   std::to_string(spec.limits.max_open_files), "--max-workspace", std::to_string(spec.limits.max_workspace_bytes)};
  if (!spec.limits.cpu_ids.empty()) {
    std::string list;
    for (int c : spec.limits.cpu_ids) list += (list.empty() ? "" : ",") + std::to_string(c);
    args.insert(args.end(), {"--cpus", list});
  }
  for (const auto& [k, v] : spec.env) args.insert(args.end(), {"--env", k + "=" + v});
  args.push_back("--");
  args.insert(args.end(), spec.argv.begin(), spec.argv.end());
  std::vector<char*> cargs;
  for (auto& a : args) cargs.push_back(a.data());
  cargs.push_back(nullptr);

  int pipefd[2];
  if (::pipe2(pipefd, O_CLOEXEC) != 0) throw Error("io_error", std::string("pipe2 failed: ") + std::strerror(errno));
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, pipefd[0], 3);  // the supervisor's liveness pipe
  sigset_t block, defaults;
  sigemptyset(&block);
  for (int s : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) sigaddset(&block, s);  // delivered later through the supervisor's signalfd
  sigemptyset(&defaults);
  for (int s = 1; s < 32; s++)
    if (s != SIGKILL && s != SIGSTOP) sigaddset(&defaults, s);
  posix_spawnattr_setsigmask(&attr, &block);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  pid_t pid = -1;
  int rc = ::posix_spawn(&pid, spec.supervisor_exe.c_str(), &fa, &attr, cargs.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  ::close(pipefd[0]);
  if (rc != 0) {
    ::close(pipefd[1]);
    throw Error("io_error", "cannot start supervisor '" + spec.supervisor_exe + "': " + std::strerror(rc));
  }
  std::unique_ptr<Execution> ex(new Execution());
  ex->pid_ = pid;
  ex->liveness_fd_ = pipefd[1];
  ex->status_path_ = spec.log_dir / "status.json";
  return ex;
}

Execution::~Execution() {
  if (pid_ > 0 && !reaped_) {
    cancel();
    wait();
  }
  if (liveness_fd_ >= 0) ::close(liveness_fd_);
}

void Execution::cancel() {
  std::lock_guard<std::mutex> lock(mu_);
  if (pid_ > 0 && !reaped_) ::kill(pid_, SIGTERM);
}

ExecResult Execution::wait() {
  int status = 0;
  for (;;) {
    pid_t r = ::waitpid(pid_, &status, 0);
    if (r == pid_) break;
    if (r < 0 && errno != EINTR) {
      status = -1;
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    reaped_ = true;
  }
  ExecResult res;
  try {
    json j = json::parse(read_file_limited(status_path_, 1 << 16));
    res.exit_code = j.value("exit_code", -1);
    res.term_signal = j.value("term_signal", 0);
    res.timed_out = j.value("timed_out", false);
    res.cancelled = j.value("cancelled", false);
    res.parent_lost = j.value("parent_lost", false);
    res.workspace_exceeded = j.value("workspace_exceeded", false);
    res.workspace_bytes = j.value("workspace_bytes", uint64_t{0});
    res.workspace_problem = j.value("workspace_problem", "");
    res.stdout_truncated = j.value("stdout_truncated", false);
    res.stderr_truncated = j.value("stderr_truncated", false);
    res.spawn_error = j.value("spawn_error", "");
    res.supervisor_error = j.value("supervisor_error", "");
  } catch (const std::exception&) {
    res.supervisor_error = "supervisor ended (wait status " + std::to_string(status) + ") without writing a result";
  }
  if (!res.spawn_error.empty() && res.exit_code == -1) res.exit_code = 127;
  return res;
}

// ================================================================== supervisor

namespace {

using Steady = std::chrono::steady_clock;

struct SupOptions {
  fs::path workdir, log_dir;
  int64_t timeout_ms = 300'000;
  uint64_t as_bytes = 0, fsize = 0, max_log = 1 << 20, max_workspace = 0;
  int nofile = 1024;
  std::vector<int> cpus;
  std::vector<std::string> env_extra;
  std::vector<std::string> argv;
};

bool parse_sup(const std::vector<std::string>& a, SupOptions& o) {
  size_t i = 0;
  auto need = [&](const char* name) -> const std::string& {
    if (i + 1 >= a.size()) throw std::runtime_error(std::string(name) + " needs a value");
    return a[++i];
  };
  try {
    for (; i < a.size(); i++) {
      const std::string& k = a[i];
      if (k == "--") {
        o.argv.assign(a.begin() + static_cast<long>(i) + 1, a.end());
        break;
      } else if (k == "--workdir") o.workdir = need("--workdir");
      else if (k == "--log-dir") o.log_dir = need("--log-dir");
      else if (k == "--timeout-ms") o.timeout_ms = std::stoll(need("--timeout-ms"));
      else if (k == "--as-bytes") o.as_bytes = std::stoull(need("--as-bytes"));
      else if (k == "--fsize") o.fsize = std::stoull(need("--fsize"));
      else if (k == "--max-log") o.max_log = std::stoull(need("--max-log"));
      else if (k == "--max-workspace") o.max_workspace = std::stoull(need("--max-workspace"));
      else if (k == "--nofile") o.nofile = std::stoi(need("--nofile"));
      else if (k == "--env") o.env_extra.push_back(need("--env"));
      else if (k == "--cpus") {
        std::stringstream ss(need("--cpus"));
        std::string item;
        while (std::getline(ss, item, ',')) o.cpus.push_back(std::stoi(item));
      } else return false;
    }
  } catch (const std::exception&) {
    return false;
  }
  return !o.argv.empty() && !o.workdir.empty() && !o.log_dir.empty();
}

// pid -> ppid for every process (reads /proc/<pid>/stat; the comm field may contain spaces and parentheses).
std::map<pid_t, pid_t> parent_map() {
  std::map<pid_t, pid_t> m;
  std::error_code ec;
  for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
    std::ifstream in(it->path() / "stat");
    std::string line;
    if (!std::getline(in, line)) continue;
    size_t rp = line.rfind(')');
    if (rp == std::string::npos) continue;
    std::istringstream rest(line.substr(rp + 1));
    char state;
    long ppid = 0;
    if (rest >> state >> ppid) m[static_cast<pid_t>(std::stol(name))] = static_cast<pid_t>(ppid);
  }
  return m;
}

std::vector<pid_t> descendants(pid_t root) {
  auto m = parent_map();
  std::multimap<pid_t, pid_t> kids;
  for (auto& [p, pp] : m) kids.emplace(pp, p);
  std::vector<pid_t> out, queue = {root};
  while (!queue.empty()) {
    pid_t cur = queue.back();
    queue.pop_back();
    auto [lo, hi] = kids.equal_range(cur);
    for (auto it = lo; it != hi; ++it) {
      out.push_back(it->second);
      queue.push_back(it->second);
    }
  }
  return out;
}

struct Stream {
  int fd = -1;       // read end of the command's pipe
  int log_fd = -1;
  uint64_t written = 0, limit = 0;
  bool truncated = false;
  void pump() {
    char buf[16384];
    for (;;) {
      ssize_t r = ::read(fd, buf, sizeof buf);
      if (r > 0) {
        uint64_t room = limit > written ? limit - written : 0;
        size_t take = static_cast<size_t>(std::min<uint64_t>(room, static_cast<uint64_t>(r)));
        if (take < static_cast<size_t>(r)) truncated = true;
        for (size_t off = 0; off < take;) {
          ssize_t w = ::write(log_fd, buf + off, take - off);
          if (w < 0) {
            if (errno == EINTR) continue;
            break;
          }
          off += static_cast<size_t>(w);
          written += static_cast<uint64_t>(w);
        }
      } else if (r == 0) {
        ::close(fd);
        fd = -1;
        return;
      } else if (errno == EINTR) {
        continue;
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          ::close(fd);
          fd = -1;
        }
        return;
      }
    }
  }
};

struct ChildState {
  pid_t pid = -1;
  bool done = false;
  int status = 0;
};

void reap_all(ChildState& child) {
  int st;
  for (;;) {
    pid_t p = ::waitpid(-1, &st, WNOHANG);
    if (p <= 0) return;
    if (p == child.pid) {
      child.done = true;
      child.status = st;
    }
  }
}

// Kill the command's process group and every descendant (found by parent-pid ancestry) until none remain.
void kill_tree(ChildState& child) {
  for (int round = 0; round < 200; round++) {
    if (child.pid > 0) ::kill(-child.pid, SIGKILL);
    std::vector<pid_t> d = descendants(::getpid());
    for (pid_t p : d) ::kill(p, SIGKILL);
    reap_all(child);
    if (d.empty()) return;
    ::usleep(5000);
  }
}

void set_child_error(int fd, const char* stage) {
  int e = errno;
  char buf[96];
  size_t n = 0;
  for (; stage[n] && n < 60; n++) buf[n] = stage[n];
  buf[n++] = ':';
  // errno as decimal without allocation (async-signal-safe)
  char digits[12];
  int nd = 0;
  do digits[nd++] = static_cast<char>('0' + e % 10);
  while ((e /= 10) > 0);
  while (nd > 0) buf[n++] = digits[--nd];
  ssize_t ignored = ::write(fd, buf, n);
  (void)ignored;
}

}  // namespace

int supervisor_main(const std::vector<std::string>& args) {
  SupOptions o;
  if (!parse_sup(args, o)) return 64;
  ::prctl(PR_SET_CHILD_SUBREAPER, 1);
  ::prctl(PR_SET_PDEATHSIG, SIGTERM);
  ::setsid();
  ::close_range(4, ~0U, 0);  // nothing but stdio and the liveness pipe (fd 3) may leak in from the Bean
  sigset_t mask;
  sigemptyset(&mask);
  for (int s : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) sigaddset(&mask, s);
  ::sigprocmask(SIG_BLOCK, &mask, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
  int sfd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

  json status = {{"exit_code", -1}, {"term_signal", 0}, {"timed_out", false}, {"cancelled", false}, {"parent_lost", false}, {"workspace_exceeded", false}, {"workspace_bytes", 0}, {"workspace_problem", ""},
                 {"stdout_truncated", false}, {"stderr_truncated", false}, {"spawn_error", ""}, {"supervisor_error", ""}};
  auto write_status = [&] {
    try {
      write_file_atomic(o.log_dir / "status.json", status.dump(), 0600);
    } catch (...) {
    }
  };
  if (sfd < 0) {
    status["supervisor_error"] = std::string("signalfd failed: ") + std::strerror(errno);
    write_status();
    return 70;
  }

  auto open_log = [&](const char* name) {
    return ::open((o.log_dir / name).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  };
  Stream out, err;
  out.log_fd = open_log("stdout.log");
  err.log_fd = open_log("stderr.log");
  out.limit = err.limit = o.max_log;
  int outp[2], errp[2], exec_err[2];
  if (out.log_fd < 0 || err.log_fd < 0 || ::pipe2(outp, O_CLOEXEC) != 0 || ::pipe2(errp, O_CLOEXEC) != 0 || ::pipe2(exec_err, O_CLOEXEC) != 0) {
    status["supervisor_error"] = std::string("cannot create logs/pipes: ") + std::strerror(errno);
    write_status();
    return 70;
  }
  // Only the supervisor's read ends are non-blocking; the command writes to ordinary blocking pipes.
  ::fcntl(outp[0], F_SETFL, ::fcntl(outp[0], F_GETFL) | O_NONBLOCK);
  ::fcntl(errp[0], F_SETFL, ::fcntl(errp[0], F_GETFL) | O_NONBLOCK);

  // Everything the child needs is prepared before fork(): the child does only async-signal-safe calls.
  std::vector<std::string> env_store = {"PATH=" + std::string(std::getenv("PATH") ? std::getenv("PATH") : "/usr/local/bin:/usr/bin:/bin"),
                                        "HOME=" + o.workdir.string(), "LANG=C.UTF-8"};
  for (const auto& e : o.env_extra) {
    std::string key = e.substr(0, e.find('='));
    env_store.erase(std::remove_if(env_store.begin(), env_store.end(), [&](const std::string& s) { return s.compare(0, key.size() + 1, key + "=") == 0; }), env_store.end());
    env_store.push_back(e);
  }
  std::vector<char*> envp, argv;
  for (auto& e : env_store) envp.push_back(e.data());
  envp.push_back(nullptr);
  for (auto& a : o.argv) argv.push_back(a.data());
  argv.push_back(nullptr);
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : o.cpus)
    if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
  const std::string workdir = o.workdir.string();
  const pid_t sup = ::getpid();
  const pid_t bean = ::getppid();  // the spawning Bean; a different parent pid later means it died

  ChildState child;
  child.pid = ::fork();
  if (child.pid < 0) {
    status["supervisor_error"] = std::string("fork failed: ") + std::strerror(errno);
    write_status();
    return 70;
  }
  if (child.pid == 0) {
    // ---- command side (single-threaded image; async-signal-safe calls only until exec)
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != sup) ::_exit(127);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    ::signal(SIGPIPE, SIG_DFL);
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull < 0 || ::dup2(devnull, 0) < 0 || ::dup2(outp[1], 1) < 0 || ::dup2(errp[1], 2) < 0) {
      set_child_error(exec_err[1], "stdio");
      ::_exit(127);
    }
    ::close(3);  // liveness pipe
    if (::chdir(workdir.c_str()) != 0) {
      set_child_error(exec_err[1], "chdir");
      ::_exit(127);
    }
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &rl);
    if (o.nofile > 0) {
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(o.nofile);
      if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        set_child_error(exec_err[1], "setrlimit(NOFILE)");
        ::_exit(127);
      }
    }
    if (o.as_bytes > 0) {
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(o.as_bytes);
      if (::setrlimit(RLIMIT_AS, &rl) != 0) {
        set_child_error(exec_err[1], "setrlimit(AS)");
        ::_exit(127);
      }
    }
    if (o.fsize > 0) {
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(o.fsize);
      if (::setrlimit(RLIMIT_FSIZE, &rl) != 0) {
        set_child_error(exec_err[1], "setrlimit(FSIZE)");
        ::_exit(127);
      }
    }
    if (!o.cpus.empty() && ::sched_setaffinity(0, sizeof set, &set) != 0) {
      set_child_error(exec_err[1], "sched_setaffinity");
      ::_exit(127);
    }
    ::execvpe(argv[0], argv.data(), envp.data());
    set_child_error(exec_err[1], "exec");
    ::_exit(127);
  }
  // ---- supervisor side
  ::setpgid(child.pid, child.pid);  // also done by the child; whichever runs first wins
  ::close(outp[1]);
  ::close(errp[1]);
  ::close(exec_err[1]);
  out.fd = outp[0];
  err.fd = errp[0];

  enum class Why { None, Timeout, Cancel, ParentLost, Workspace } why = Why::None;
  const auto deadline = Steady::now() + std::chrono::milliseconds(o.timeout_ms);
  bool liveness_open = true;
  bool killed = false;
  Steady::time_point drain_deadline = Steady::time_point::max();
  std::string exec_error;
  WorkspaceUsage ws;
  auto last_scan = Steady::now();

  auto trigger = [&](Why w) {
    if (why == Why::None) why = w;
    if (!killed) {
      kill_tree(child);
      killed = true;
      drain_deadline = Steady::now() + std::chrono::seconds(2);
    }
  };
  auto read_exec_error = [&] {
    char buf[128];
    ssize_t n = ::read(exec_err[0], buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      std::string s(buf);
      size_t c = s.rfind(':');
      int e = c == std::string::npos ? 0 : std::atoi(s.c_str() + c + 1);
      exec_error = s.substr(0, c) + ": " + std::strerror(e);
    }
    ::close(exec_err[0]);
    exec_err[0] = -1;
  };

  for (;;) {
    reap_all(child);
    if (child.done && !killed) {  // the command ended: nothing it left behind may outlive the task
      kill_tree(child);
      killed = true;
      drain_deadline = Steady::now() + std::chrono::seconds(2);
    }
    if (out.fd >= 0) out.pump();
    if (err.fd >= 0) err.pump();
    if (child.done && killed && (out.fd < 0 && err.fd < 0)) break;
    if (killed && Steady::now() >= drain_deadline) break;

    std::vector<pollfd> fds = {{sfd, POLLIN, 0}};
    if (liveness_open) fds.push_back({3, POLLIN, 0});
    if (out.fd >= 0) fds.push_back({out.fd, POLLIN, 0});
    if (err.fd >= 0) fds.push_back({err.fd, POLLIN, 0});
    if (exec_err[0] >= 0) fds.push_back({exec_err[0], POLLIN, 0});
    int wait_ms = 200;
    if (!killed) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Steady::now()).count();
      if (left <= 0) {
        trigger(Why::Timeout);
        continue;
      }
      wait_ms = static_cast<int>(std::min<int64_t>(left, o.max_workspace ? kWorkspaceScanIntervalMs : 200));
      if (o.max_workspace && Steady::now() - last_scan >= std::chrono::milliseconds(kWorkspaceScanIntervalMs)) {
        ws = measure_workspace(o.workdir, o.max_workspace, descendants(::getpid()));
        last_scan = Steady::now();
        if (!ws.problem.empty()) {
          trigger(Why::Workspace);
          continue;
        }
      }
    }
    ::poll(fds.data(), fds.size(), wait_ms);
    for (const auto& p : fds) {
      if (!p.revents) continue;
      if (p.fd == sfd) {
        signalfd_siginfo si;
        while (::read(sfd, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
          if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT || si.ssi_signo == SIGHUP) {
            // PDEATHSIG delivers SIGTERM when the Bean dies; an explicit cancel is a SIGTERM while the Bean is still our parent.
            if (why == Why::None) why = ::getppid() != bean ? Why::ParentLost : Why::Cancel;
          }
        }
        if (why != Why::None) trigger(why);
      } else if (p.fd == 3 && liveness_open) {
        char b[16];
        ssize_t r = ::read(3, b, sizeof b);
        if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) {  // write end closed: the Bean is gone
          liveness_open = false;
          trigger(Why::ParentLost);
        }
      } else if (exec_err[0] >= 0 && p.fd == exec_err[0]) {
        read_exec_error();
      }
    }
  }
  if (exec_err[0] >= 0) read_exec_error();
  if (!killed) kill_tree(child);
  if (!child.done) reap_all(child);
  // Final measurement after every descendant is dead: a fast writer that finished between samples cannot slip
  // through, and no success is reported for an unmeasurable workspace.
  if (o.max_workspace) {
    WorkspaceUsage fin = measure_workspace(o.workdir, o.max_workspace, descendants(::getpid()));
    if (fin.problem.empty() && fin.bytes > o.max_workspace) fin.problem = "workspace holds more than the limit";
    if (why != Why::Workspace || !fin.problem.empty()) ws = fin;  // keep the sample that triggered the kill (the tree is dead now)
    if (!ws.problem.empty() && why == Why::None) why = Why::Workspace;
  }

  status["timed_out"] = why == Why::Timeout;
  status["cancelled"] = why == Why::Cancel;
  status["parent_lost"] = why == Why::ParentLost;
  status["workspace_exceeded"] = why == Why::Workspace || (o.max_workspace && !ws.problem.empty());
  status["workspace_bytes"] = ws.bytes;
  status["workspace_problem"] = ws.problem;
  status["stdout_truncated"] = out.truncated;
  status["stderr_truncated"] = err.truncated;
  status["spawn_error"] = exec_error;
  if (child.done) {
    if (WIFEXITED(child.status)) status["exit_code"] = WEXITSTATUS(child.status);
    else if (WIFSIGNALED(child.status)) status["term_signal"] = WTERMSIG(child.status);
  } else {
    status["supervisor_error"] = "command was not reaped";
  }
  ::close(out.log_fd);
  ::close(err.log_fd);
  write_status();
  return 0;
}

}  // namespace coffee
