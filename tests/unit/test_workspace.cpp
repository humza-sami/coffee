// Workspace measurement including open-but-unlinked files (correction round 2). The measuring process is the test
// process itself: its own fds play the role of "the command and its descendants".
#include <catch_amalgamated.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "coffee/util.hpp"
#include "coffee/workspace.hpp"

using namespace coffee;
namespace fs = std::filesystem;

namespace {
struct Tmp {
  fs::path dir = fs::temp_directory_path() / ("coffee-test-ws-" + random_hex(6));
  fs::path ws = dir / "ws", outside = dir / "outside";
  Tmp() { fs::create_directories(ws); fs::create_directories(outside); }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};
int open_unlinked(const fs::path& p, size_t bytes) {
  int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  REQUIRE(fd >= 0);
  std::string chunk(bytes, 'x');
  REQUIRE(::write(fd, chunk.data(), chunk.size()) == static_cast<ssize_t>(bytes));
  REQUIRE(::unlink(p.c_str()) == 0);
  return fd;
}
}  // namespace

TEST_CASE("open files unlinked from the workspace still count", "[workspace][unlinked]") {
  Tmp t;
  std::vector<int> fds;
  for (int i = 0; i < 4; i++) fds.push_back(open_unlinked(t.ws / ("u" + std::to_string(i)), 900'000));
  REQUIRE(fs::is_empty(t.ws));  // the directory view sees nothing
  WorkspaceUsage u = measure_workspace(t.ws, 2'000'000, {::getpid()});
  CHECK_FALSE(u.problem.empty());
  CHECK(u.open_deleted_bytes >= 2'700'000);
  WorkspaceUsage no_pid = measure_workspace(t.ws, 2'000'000, {});
  CHECK(no_pid.problem.empty());  // without the process view the old blind spot reappears
  for (int fd : fds) ::close(fd);
  CHECK(measure_workspace(t.ws, 2'000'000, {::getpid()}).problem.empty());  // closing releases the allocation
}

TEST_CASE("one inode is counted once across several fds, a path view and hard links", "[workspace][unlinked]") {
  Tmp t;
  int fd = open_unlinked(t.ws / "tmp", 600'000);
  int dup_fd = ::dup(fd);
  int again = ::open(("/proc/self/fd/" + std::to_string(fd)).c_str(), O_RDONLY);  // second open file description
  REQUIRE(dup_fd >= 0);
  REQUIRE(again >= 0);
  WorkspaceUsage u = measure_workspace(t.ws, 1'000'000, {::getpid(), ::getpid()});
  CHECK(u.problem.empty());
  CHECK(u.bytes >= 600'000);
  CHECK(u.bytes < 1'000'000);  // not 3 x 600 000
  // linked file: visible by path AND held open: still once; a hard link to it: still once
  std::ofstream(t.ws / "linked", std::ios::binary) << std::string(600'000, 'y');
  int lfd = ::open((t.ws / "linked").c_str(), O_RDONLY);
  fs::create_hard_link(t.ws / "linked", t.ws / "linked2");
  u = measure_workspace(t.ws, 1'500'000, {::getpid()});
  CHECK(u.problem.empty());
  CHECK(u.bytes < 1'500'000);
  for (int f : {fd, dup_fd, again, lfd}) ::close(f);
}

TEST_CASE("files outside the workspace and non-file descriptors are not attributed", "[workspace][unlinked]") {
  Tmp t;
  int ext = open_unlinked(t.outside / "elsewhere", 3'000'000);
  int sibling = open_unlinked(fs::path(t.ws.string() + "-sibling"), 3'000'000);  // shares the textual prefix only
  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);
  WorkspaceUsage u = measure_workspace(t.ws, 1'000'000, {::getpid()});
  CHECK(u.problem.empty());
  CHECK(u.open_deleted_bytes == 0);
  for (int f : {ext, sibling, pipefd[0], pipefd[1]}) ::close(f);
}

TEST_CASE("normal temporary-file churn and processes that vanish do not fail", "[workspace][unlinked]") {
  Tmp t;
  for (int i = 0; i < 20; i++) {
    int fd = open_unlinked(t.ws / "scratch", 10'000);  // common create-unlink-use pattern, small
    CHECK(measure_workspace(t.ws, 1'000'000, {::getpid()}).problem.empty());
    ::close(fd);
  }
  pid_t child = ::fork();
  if (child == 0) ::_exit(0);
  int st;
  ::waitpid(child, &st, 0);  // reaped: the pid is gone
  CHECK(measure_workspace(t.ws, 1'000'000, {child, ::getpid(), 999999999}).problem.empty());
}

TEST_CASE("the process/fd traversal is bounded and reports a readable problem", "[workspace][unlinked]") {
  CHECK(kMaxWorkspaceFdEntries >= 10'000);
  CHECK(kMaxWorkspaceProcesses >= 256);
  Tmp t;
  std::vector<pid_t> many(kMaxWorkspaceProcesses + 1, ::getpid());
  WorkspaceUsage u = measure_workspace(t.ws, 1'000'000, many);
  CHECK_FALSE(u.problem.empty());
  CHECK(u.problem.find("processes") != std::string::npos);
}

// Regression (found by the benchmark): while a process is inside execve(), /proc/<pid>/fd is briefly unreadable
// (the new image is not yet dumpable). A sample taken in that window used to fail the whole attempt closed, killing
// healthy commands at random (one task in about a hundred on a loaded single CPU). A process that keeps exec'ing under
// the same pid (shell `exec` chains, make recursion, wrappers) must never be reported as unreadable merely because
// a sample overlapped an execve.
TEST_CASE("a process repeatedly inside execve is not mistaken for an unreadable one", "[workspace][exec]") {
  Tmp t;
  // Each hop replaces the process image (same pid): sh -c SCRIPT SCRIPT N  ->  exec sh -c SCRIPT SCRIPT N-1 ...
  const char* script = "n=$1; if [ \"$n\" -gt 0 ]; then exec /bin/sh -c \"$0\" \"$0\" $((n-1)); fi";
  std::vector<pid_t> procs;
  for (int i = 0; i < 3; i++) {
    pid_t p = ::fork();
    if (p == 0) {
      char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(script), const_cast<char*>(script), const_cast<char*>("1500"), nullptr};
      ::execv("/bin/sh", argv);
      ::_exit(127);
    }
    REQUIRE(p > 0);
    procs.push_back(p);
  }
  int spurious = 0, samples = 0, alive = static_cast<int>(procs.size());
  std::string first_problem;
  while (alive > 0 && samples < 2'000'000) {
    for (pid_t p : procs) {
      auto u = measure_workspace(t.ws, 1'000'000, {p});
      samples++;
      if (!u.problem.empty()) {
        spurious++;
        if (first_problem.empty()) first_problem = u.problem;
      }
    }
    alive = 0;
    for (pid_t p : procs) {
      int st;
      if (::waitpid(p, &st, WNOHANG) == 0) alive++;
    }
  }
  for (pid_t p : procs) ::kill(p, SIGKILL);
  for (pid_t p : procs) ::waitpid(p, nullptr, 0);
  INFO(spurious << " of " << samples << " samples reported a problem; first: " << first_problem);
  CHECK(samples > 1000);
  CHECK(spurious == 0);
}
