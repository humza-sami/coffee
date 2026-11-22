// Workspace measurement for the per-attempt aggregate cap (see ExecLimits::max_workspace_bytes).
//
// Two views are combined and de-duplicated by (device, inode), so a file reachable by several names, held by several
// descriptors or processes, or seen in both views is counted once:
//  * path view: every regular file below the workspace root, found by an openat/O_NOFOLLOW directory walk
//    (symlinks are never followed);
//  * descriptor view: every regular file that one of the given processes holds open and whose /proc/<pid>/fd link
//    resolves to a path below the workspace root. This is what catches files that were unlinked while still open
//    (the kernel reports them as "<workspace path> (deleted)"), including O_TMPFILE files, and files held by ordinary
//    descendants. Descriptors that point outside the workspace, sockets, pipes and anonymous inodes are not counted,
//    and neither are files renamed out of the workspace while open (an external write).
// Size of a file = max(st_size, allocated bytes). Not a filesystem quota: it is a point-in-time sample; memory-only
// objects (memfd, anonymous mmap) are bounded by RLIMIT_AS instead, and an unlinked file closed or freed between two
// samples is not observed.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace coffee {

inline constexpr uint64_t kMaxWorkspaceEntries = 200'000;   // directory entries walked per scan
inline constexpr uint64_t kMaxWorkspaceFdEntries = 200'000; // /proc/<pid>/fd entries examined per scan
inline constexpr size_t kMaxWorkspaceProcesses = 4096;      // processes examined per scan

struct WorkspaceUsage {
  uint64_t bytes = 0;               // distinct files (path view + descriptor view)
  uint64_t open_deleted_bytes = 0;  // part of `bytes` that is only reachable through open descriptors
  uint64_t entries = 0;
  std::string problem;              // non-empty: limit exceeded or the workspace could not be measured (fail closed)
};

// `limit` 0 = report only. Stops early (with a problem) as soon as `limit` is exceeded. Processes that exit or
// descriptors that close while being examined are skipped silently; a process inside execve() (transiently
// unreadable) is retried for up to about 50 ms; a live process whose descriptors stay unreadable
// (permission) or too many processes/entries produce a readable problem.
WorkspaceUsage measure_workspace(const std::filesystem::path& root, uint64_t limit, const std::vector<pid_t>& process_ids);

}  // namespace coffee
