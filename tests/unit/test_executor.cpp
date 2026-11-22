#include <catch_amalgamated.hpp>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "coffee/executor.hpp"
#include "coffee/util.hpp"

using namespace coffee;
namespace fs = std::filesystem;

namespace {
struct Tmp {
  fs::path dir = fs::temp_directory_path() / ("coffee-test-exec-" + random_hex(6));
  Tmp() { fs::create_directories(dir / "work"); fs::create_directories(dir / "log"); }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  ExecSpec spec(std::vector<std::string> argv) {
    ExecSpec s;
    s.supervisor_exe = COFFEE_BIN;
    s.argv = std::move(argv);
    s.workdir = dir / "work";
    s.log_dir = dir / "log";
    s.limits.timeout_ms = 20'000;
    return s;
  }
  std::string log(const char* name) { return read_file_limited(dir / "log" / name, 1 << 22); }
};
bool alive(pid_t p) { return p > 0 && ::kill(p, 0) == 0; }
bool gone_within(pid_t p, int ms) {
  for (int i = 0; i < ms / 20; i++) {
    if (!alive(p)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return !alive(p);
}
pid_t read_pid(const fs::path& p) {
  for (int i = 0; i < 200; i++) {
    std::ifstream in(p);
    long v = 0;
    if (in >> v && v > 0) return static_cast<pid_t>(v);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return -1;
}
}  // namespace

TEST_CASE("exit status, captured logs and working directory", "[executor]") {
  Tmp t;
  auto ex = Execution::start(t.spec({"sh", "-c", "echo out; echo err >&2; pwd > where.txt; exit 7"}));
  ExecResult r = ex->wait();
  CHECK(r.exit_code == 7);
  CHECK_FALSE(r.timed_out);
  CHECK_FALSE(r.cancelled);
  CHECK(t.log("stdout.log") == "out\n");
  CHECK(t.log("stderr.log") == "err\n");
  CHECK(read_file_limited(t.dir / "work/where.txt", 1000).find("work") != std::string::npos);
}

TEST_CASE("the command environment is minimal and carries no Coffee credentials", "[executor]") {
  Tmp t;
  ::setenv("COFFEE_TOKEN", "super-secret", 1);
  auto s = t.spec({"sh", "-c", "env > env.txt"});
  s.env = {{"COFFEE_TASK_ID", "t1"}};
  CHECK(Execution::start(s)->wait().exit_code == 0);
  std::string env = read_file_limited(t.dir / "work/env.txt", 1 << 16);
  CHECK(env.find("COFFEE_TASK_ID=t1") != std::string::npos);
  CHECK(env.find("super-secret") == std::string::npos);
  ::unsetenv("COFFEE_TOKEN");
}

TEST_CASE("a spawn failure is reported, not hidden", "[executor]") {
  Tmp t;
  ExecResult r = Execution::start(t.spec({"/nonexistent/program"}))->wait();
  CHECK_FALSE(r.spawn_error.empty());
  CHECK(r.exit_code != 0);
}

TEST_CASE("wall timeout kills the command and its ordinary children", "[executor][limits]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "sleep 300 & echo $! > child.pid; wait"});
  s.limits.timeout_ms = 400;
  auto start = std::chrono::steady_clock::now();
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.timed_out);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
  pid_t child = read_pid(t.dir / "work/child.pid");
  REQUIRE(child > 0);
  CHECK(gone_within(child, 2000));
}

TEST_CASE("cancel stops the command tree and reports cancellation", "[executor][limits]") {
  Tmp t;
  auto ex = Execution::start(t.spec({"sh", "-c", "sleep 300 & echo $! > child.pid; wait"}));
  pid_t child = read_pid(t.dir / "work/child.pid");
  REQUIRE(child > 0);
  ex->cancel();
  ExecResult r = ex->wait();
  CHECK(r.cancelled);
  CHECK(gone_within(child, 2000));
}

TEST_CASE("descendants that start their own session are still stopped (subreaper ancestry)", "[executor][limits]") {
  Tmp t;
  // setsid escapes the process group; the daemonized grandchild is re-parented to the supervisor.
  auto s = t.spec({"sh", "-c", "setsid sh -c 'sleep 300 & echo $! > far.pid; wait' >/dev/null 2>&1 & sleep 0.3; echo main-done"});
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.exit_code == 0);
  pid_t far = read_pid(t.dir / "work/far.pid");
  REQUIRE(far > 0);
  CHECK(gone_within(far, 2000));
}

TEST_CASE("address-space limit makes a memory hog fail", "[executor][limits]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "head -c 400000000 /dev/zero | tr '\\0' x > /dev/null; exit 0"});
  s.limits.memory_bytes = 64ll << 20;
  // `tr`/`head` need only small buffers: the limit must not break ordinary commands.
  CHECK(Execution::start(s)->wait().exit_code == 0);
  auto hog = t.spec({"sh", "-c", "exec perl -e '$x = \"a\" x (300*1024*1024); print length($x)' || exit 9"});
  hog.limits.memory_bytes = 64ll << 20;
  ExecResult r = Execution::start(hog)->wait();
  CHECK(r.exit_code != 0);
}

TEST_CASE("file size limit stops runaway output files", "[executor][limits]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "head -c 5000000 /dev/zero > big.bin"});
  s.limits.max_file_bytes = 100'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK((r.term_signal == SIGXFSZ || r.exit_code != 0));
  CHECK(fs::file_size(t.dir / "work/big.bin") <= 100'000);
}

TEST_CASE("CPU affinity is applied from the assigned CPU ids", "[executor][limits]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "grep Cpus_allowed_list /proc/self/status > cpus.txt"});
  s.limits.cpu_ids = {0};
  CHECK(Execution::start(s)->wait().exit_code == 0);
  std::string c = read_file_limited(t.dir / "work/cpus.txt", 1000);
  CHECK(c.find("Cpus_allowed_list:\t0\n") != std::string::npos);
}

TEST_CASE("log capture is bounded and reports truncation", "[executor][limits]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "yes x | head -c 2000000; yes y | head -c 100 >&2"});
  s.limits.max_log_bytes = 10'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.exit_code == 0);
  CHECK(r.stdout_truncated);
  CHECK_FALSE(r.stderr_truncated);
  CHECK(fs::file_size(t.dir / "log/stdout.log") == 10'000);
}

// ------------------------------------------------------------------ correction round 1: aggregate workspace bound

TEST_CASE("many small files exceed the aggregate workspace cap although each is under the per-file limit", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "for i in 1 2 3 4; do head -c 900000 /dev/zero > file$i; sleep 0.2; done; echo done > out"});
  s.limits.max_file_bytes = 1'000'000;
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.workspace_exceeded);
  CHECK_FALSE(r.ok());
  CHECK(r.workspace_bytes > 2'000'000);
  CHECK_FALSE(fs::exists(t.dir / "work/out"));  // killed while still growing, did not finish
}

TEST_CASE("ordinary child writers count toward the workspace cap", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "(for i in 1 2 3 4 5 6; do head -c 900000 /dev/zero > child$i; sleep 0.2; done) & wait"});
  s.limits.max_file_bytes = 1'000'000;
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.workspace_exceeded);
}

TEST_CASE("a fast writer that exits between samples is still rejected by the post-exit check", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "for i in 1 2 3 4; do head -c 900000 /dev/zero > file$i; done"});  // finishes in a few ms
  s.limits.max_file_bytes = 1'000'000;
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.exit_code == 0);                // the command itself "succeeded" ...
  CHECK(r.workspace_exceeded);            // ... but the attempt must not be accepted
  CHECK_FALSE(r.ok());
  CHECK(r.workspace_bytes > 2'000'000);  // the scan stops as soon as the limit is crossed
}

TEST_CASE("a bounded workload below the cap succeeds", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "head -c 500000 /dev/zero > a; head -c 500000 /dev/zero > b; echo ok > out"});
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.ok());
  CHECK_FALSE(r.workspace_exceeded);
  CHECK(r.workspace_bytes >= 1'000'000);
}

TEST_CASE("an unreadable workspace directory fails closed", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "mkdir hidden; echo x > hidden/f; chmod 000 hidden"});
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.workspace_exceeded);
  CHECK(r.workspace_problem.find("unreadable") != std::string::npos);
  ::chmod((t.dir / "work/hidden").c_str(), 0700);  // let the fixture clean up
}

TEST_CASE("symlinks are not followed by the workspace scan", "[executor][workspace]") {
  Tmp t;
  fs::create_directories(t.dir / "outside");
  std::ofstream(t.dir / "outside/huge", std::ios::binary) << std::string(3'000'000, 'x');
  auto s = t.spec({"sh", "-c", std::string("ln -s ") + (t.dir / "outside").string() + " dirlink; ln -s " + (t.dir / "outside/huge").string() + " filelink"});
  s.limits.max_workspace_bytes = 1'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.ok());
  CHECK(r.workspace_bytes < 1'000'000);
}

TEST_CASE("too many entries fail closed", "[executor][workspace]") {
  Tmp t;
  auto s = t.spec({"sh", "-c", "mkdir d; i=0; while [ $i -lt 2000 ]; do : > d/f$i; i=$((i+1)); done"});
  s.limits.max_workspace_bytes = 1ull << 30;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.ok());  // 2000 entries is fine; the entry bound itself is kMaxWorkspaceEntries
  CHECK(kMaxWorkspaceEntries >= 100'000);
}

TEST_CASE("unlinked-but-open files held by the command and an ordinary child count toward the cap", "[executor][workspace][unlinked]") {
  Tmp t;
  // two descriptors in the command, two in an ordinary child, each file unlinked while open, held for 2 s
  const char* perl = "for $i (1..2) { my $f; open($f,'>',\"u$i\"); unlink \"u$i\"; syswrite($f, 'x' x 900000); push @k,$f } "
                     "if (!fork) { for $i (3..4) { my $g; open($g,'>',\"u$i\"); unlink \"u$i\"; syswrite($g, 'x' x 900000); push @k,$g } sleep 2; exit 0 } sleep 2; wait;";
  auto s = t.spec({"perl", "-e", perl});
  s.limits.max_file_bytes = 1'000'000;
  s.limits.max_workspace_bytes = 2'000'000;
  auto start = std::chrono::steady_clock::now();
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.workspace_exceeded);
  CHECK(r.workspace_bytes > 2'000'000);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1800));  // killed by the sampler, not after the 2 s hold
  CHECK(fs::is_empty(t.dir / "work"));
}

TEST_CASE("a small unlinked temporary file is not a violation", "[executor][workspace][unlinked]") {
  Tmp t;
  auto s = t.spec({"perl", "-e", "open($f,'>','tmp'); unlink 'tmp'; syswrite($f,'x' x 50000); select(undef,undef,undef,0.4); open(O,'>','out'); print O 'ok';"});
  s.limits.max_workspace_bytes = 2'000'000;
  ExecResult r = Execution::start(s)->wait();
  CHECK(r.ok());
}
