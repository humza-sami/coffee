#include "coffee/workspace.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <utility>

namespace coffee {

namespace fs = std::filesystem;

namespace {

using Inode = std::pair<uint64_t, uint64_t>;  // (st_dev, st_ino)

constexpr int kExecWindowRetries = 50;  // x 1 ms: a genuinely unreadable process still fails closed after about 50 ms

uint64_t sat_add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }
uint64_t file_cost(const struct stat& st) { return std::max<uint64_t>(static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_blocks) * 512); }

struct Scan {
  uint64_t limit;
  WorkspaceUsage usage;
  std::set<Inode> seen;

  // Returns true when the file was new (and has been added).
  bool add(const struct stat& st, bool open_deleted) {
    if (!seen.insert({static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)}).second) return false;
    uint64_t c = file_cost(st);
    usage.bytes = sat_add(usage.bytes, c);
    if (open_deleted) usage.open_deleted_bytes = sat_add(usage.open_deleted_bytes, c);
    if (limit && usage.bytes > limit && usage.problem.empty()) usage.problem = "workspace holds more than the limit";
    return true;
  }
  bool failed() const { return !usage.problem.empty(); }
};

void walk_dir(int dirfd, int depth, Scan& s) {
  DIR* d = ::fdopendir(dirfd);  // takes ownership of dirfd
  if (!d) {
    ::close(dirfd);
    s.usage.problem = "cannot read a directory in the workspace";
    return;
  }
  while (!s.failed()) {
    errno = 0;
    dirent* e = ::readdir(d);
    if (!e) {
      if (errno != 0) s.usage.problem = "cannot read a directory in the workspace";
      break;
    }
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    if (++s.usage.entries > kMaxWorkspaceEntries) {
      s.usage.problem = "more than " + std::to_string(kMaxWorkspaceEntries) + " files and directories in the workspace";
      break;
    }
    struct stat st{};
    if (::fstatat(::dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;  // removed while scanning
      s.usage.problem = "cannot stat a workspace entry";
      break;
    }
    if (S_ISREG(st.st_mode)) {
      s.add(st, false);
    } else if (S_ISDIR(st.st_mode)) {
      if (depth >= 128) {
        s.usage.problem = "workspace directories nested deeper than 128 levels";
        break;
      }
      int sub = ::openat(::dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (sub < 0) {
        if (errno == ENOENT) continue;
        s.usage.problem = "unreadable directory in the workspace";  // e.g. chmod 000: fail closed
        break;
      }
      walk_dir(sub, depth + 1, s);
    }  // symlinks and special files are neither followed nor counted
  }
  ::closedir(d);
}

bool process_is_zombie_or_gone(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(in, line)) return true;
  size_t rp = line.rfind(')');
  return rp == std::string::npos || rp + 2 >= line.size() || line[rp + 2] == 'Z' || line[rp + 2] == 'X';
}

// Open files of one process below `prefix` ("<root>/"). Races (process exit, fd close) are expected and ignored.
void scan_process_fds(pid_t pid, const std::string& prefix, Scan& s, uint64_t& fd_entries) {
  const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
  DIR* d = ::opendir(dir.c_str());
  // While a process is inside execve() its new image is not yet dumpable and /proc/<pid>/fd answers EACCES for a moment.
  // That is a transient state of a healthy command, not an unreadable one: retry briefly (bounded) before failing closed.
  for (int retry = 0; !d && (errno == EACCES || errno == EPERM) && retry < kExecWindowRetries; retry++) {
    if (process_is_zombie_or_gone(pid)) return;
    const struct timespec pause = {0, 1'000'000};  // 1 ms
    ::nanosleep(&pause, nullptr);
    d = ::opendir(dir.c_str());
  }
  if (!d) {
    if (errno == ENOENT || errno == ESRCH || process_is_zombie_or_gone(pid)) return;
    s.usage.problem = "cannot inspect the open files of process " + std::to_string(pid) + " (" + std::strerror(errno) + ")";
    return;
  }
  while (!s.failed()) {
    errno = 0;
    dirent* e = ::readdir(d);
    if (!e) break;  // an error mid-listing means the process is exiting
    if (e->d_name[0] == '.') continue;
    if (++fd_entries > kMaxWorkspaceFdEntries) {
      s.usage.problem = "more than " + std::to_string(kMaxWorkspaceFdEntries) + " open descriptors to examine";
      break;
    }
    const std::string link = dir + "/" + e->d_name;
    char target[4096];
    ssize_t n = ::readlink(link.c_str(), target, sizeof target - 1);
    if (n <= 0) continue;  // closed meanwhile
    target[n] = 0;
    std::string t(target, static_cast<size_t>(n));
    const std::string suffix = " (deleted)";
    bool deleted = t.size() > suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0;
    if (deleted) t.resize(t.size() - suffix.size());
    if (t.compare(0, prefix.size(), prefix) != 0) continue;  // not a workspace path (also excludes pipes, sockets, anon inodes)
    struct stat st{};
    if (::stat(link.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;  // follows the magic link to the open inode
    s.add(st, deleted && st.st_nlink == 0);
  }
  ::closedir(d);
}

}  // namespace

WorkspaceUsage measure_workspace(const fs::path& root, uint64_t limit, const std::vector<pid_t>& process_ids) {
  Scan s;
  s.limit = limit;
  int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    s.usage.problem = "cannot open the workspace";
    return s.usage;
  }
  walk_dir(fd, 0, s);
  if (s.failed()) return s.usage;

  if (process_ids.size() > kMaxWorkspaceProcesses) {
    s.usage.problem = "more than " + std::to_string(kMaxWorkspaceProcesses) + " processes below the command";
    return s.usage;
  }
  std::vector<pid_t> pids = process_ids;
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  std::error_code ec;
  fs::path canon = fs::canonical(root, ec);  // /proc reports canonical paths
  std::string prefix = (ec ? root : canon).string();
  if (prefix.empty() || prefix.back() != '/') prefix += '/';
  uint64_t fd_entries = 0;
  for (pid_t pid : pids) {
    if (pid <= 0) continue;
    scan_process_fds(pid, prefix, s, fd_entries);
    if (s.failed()) break;
  }
  return s.usage;
}

}  // namespace coffee
