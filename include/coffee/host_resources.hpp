// Host resource observation for `coffee resources`: logical/physical CPUs, cgroup-v2 CPU and memory limits, /proc/meminfo
// and the filesystem behind a directory. This is an observation and a dry-run check, not an allocation or enforcement.
//
// Every observation-derived number is std::optional: std::nullopt means "unknown" and is never replaced by 0 or by an
// optimistic default. Unknown propagates (fail closed): an unreadable relevant cgroup group makes the limit, headroom,
// effective value and budget derived from it unknown instead of printing a partial minimum.
//
// Live observation (observe_live) is separate from deterministic evaluation (evaluate): tests build HostInputs from
// fixture text and a fixture directory tree, so the real parsers, cgroup2 resolver and file reader are exercised.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace coffee::host {

inline constexpr uint64_t kMiB = 1024ull * 1024ull;
inline constexpr uint64_t kDefaultReserveMiB = 1024;  // default memory and storage reserve

enum class LimitStatus { kLimited, kUnlimited, kUnknown };
const char* to_string(LimitStatus s);  // "limited" / "unlimited" / "unknown"

// ---- strict token parsers (exposed for tests) ---------------------------------------------------------------------

struct MemInfo {
  std::optional<uint64_t> total_bytes, available_bytes;
  std::vector<std::string> warnings;
};
// MemTotal / MemAvailable in exact kB (x1024, overflow checked). Duplicate, malformed or missing values -> nullopt + warning.
MemInfo parse_meminfo(std::string_view text);

struct CpuMax {
  std::optional<uint64_t> quota_us;  // nullopt = "max"
  uint64_t period_us = 0;            // always > 0
};
// "<quota|max> <period>\n": unsigned decimal only, period > 0, no extra tokens. Quota 0 is valid.
std::optional<CpuMax> parse_cpu_max(std::string_view text);

struct MemLimit {
  bool unlimited = false;
  uint64_t bytes = 0;  // valid when !unlimited (0 is a valid finite limit)
};
std::optional<MemLimit> parse_memory_max(std::string_view text);   // "max" or unsigned decimal
std::optional<uint64_t> parse_uint_file(std::string_view text);    // unsigned decimal + optional single newline (memory.current)

// ---- the one cgroup2 resolver used by both CPU and RAM -------------------------------------------------------------

struct CgroupGroup {
  std::string path;          // absolute path as the process sees it: mount point + group below it
  bool visible_root = false; // the mount point itself (the last, outermost visible group)
  bool global_root = false;  // positively the real hierarchy root: mount root "/" and no cgroup.events file there
};

struct CgroupHierarchy {
  bool resolved = false;
  std::string error;                  // why resolution failed (unknown), when !resolved
  std::filesystem::path fs_root{"/"}; // prefix under which mount points are opened (fixtures); "/" live
  std::vector<CgroupGroup> groups;    // leaf first, ending with the visible mount root inclusive
};

// Maps the unified `0::` membership through the longest component-prefix cgroup2 mount root. Fails closed (resolved=false)
// on v1-only, malformed, ambiguous, unmappable or escaping records.
CgroupHierarchy resolve_cgroup2(std::string_view self_cgroup, std::string_view mountinfo, const std::filesystem::path& fs_root);

struct FileRead {
  enum class Status { kOk, kMissing, kError } status = Status::kError;
  std::string text;
  std::string error;
};
// Reads `name` in group `index` of `h`, walking component by component with O_NOFOLLOW below fs_root; never leaves it.
FileRead read_cgroup_file(const CgroupHierarchy& h, size_t index, const std::string& name);

struct CpuQuota {
  LimitStatus status = LimitStatus::kUnknown;
  std::optional<double> quota_cores;  // tightest finite quota; null unless status == kLimited
  std::vector<std::string> warnings;
};
CpuQuota observe_cpu_quota(const CgroupHierarchy& h);

struct MemoryCgroup {
  LimitStatus status = LimitStatus::kUnknown;
  std::optional<uint64_t> limit_bytes, headroom_bytes;  // null unless status == kLimited
  std::vector<std::string> warnings;
};
MemoryCgroup observe_memory_cgroup(const CgroupHierarchy& h);

// ---- topology and storage ------------------------------------------------------------------------------------------

struct Topology {
  std::optional<uint64_t> physical_cores;  // distinct (physical_package_id, core_id) of the allowed CPUs; null if incomplete
  std::string warning;
};
Topology read_topology(const std::filesystem::path& sys_root, const std::vector<int>& cpus);

struct FsStat {  // statvfs equivalent
  uint64_t frsize = 0, blocks = 0, bfree = 0, bavail = 0;
};
struct StorageNumbers {
  std::optional<uint64_t> total_bytes, available_bytes;
  std::string error;  // non-empty: implausible or overflowing statvfs values (no budget is invented)
};
StorageNumbers evaluate_storage(const FsStat& st);

// ---- inputs, report, evaluation ------------------------------------------------------------------------------------

struct HostInputs {
  std::optional<std::vector<int>> affinity_cpus;  // nullopt: sched_getaffinity failed or too many CPUs
  std::string affinity_error;
  std::filesystem::path sys_root{"/sys"};         // devices/system/cpu/cpuN/topology/{physical_package_id,core_id}
  std::optional<std::string> meminfo;             // /proc/meminfo text
  std::string meminfo_error;
  std::optional<std::string> self_cgroup, mountinfo;  // /proc/self/cgroup, /proc/self/mountinfo text
  std::string cgroup_error;
  std::filesystem::path fs_root{"/"};             // prefix for mountinfo mount points
  std::filesystem::path storage_path;             // resolved directory that fs_stat describes
  std::optional<FsStat> fs_stat;
  std::string storage_error;                      // non-empty with !fs_stat: operational failure
};

struct Reserves {
  uint64_t memory_bytes = kDefaultReserveMiB * kMiB;
  uint64_t storage_bytes = kDefaultReserveMiB * kMiB;
};

struct Request {
  std::optional<uint64_t> cpus, memory_bytes, storage_bytes;
};

struct Report {
  struct Cpu {
    std::optional<uint64_t> affinity_logical_cpus, physical_cores;
    std::optional<double> quota_cores, effective_cores;
    LimitStatus quota_status = LimitStatus::kUnknown;
  } cpu;
  struct Memory {
    std::optional<uint64_t> host_total_bytes, host_available_bytes, cgroup_limit_bytes, cgroup_headroom_bytes, effective_available_bytes;
    LimitStatus cgroup_status = LimitStatus::kUnknown;
  } memory;
  struct Storage {
    std::string path;
    std::optional<uint64_t> total_bytes, available_bytes;
  } storage;
  struct Budget {
    uint64_t reserve_memory_bytes = 0, reserve_storage_bytes = 0;
    std::optional<uint64_t> max_cpu_slots, max_memory_bytes, max_storage_bytes;
  } budget;
  struct RequestResult {
    std::optional<uint64_t> cpus, memory_bytes, storage_bytes;
    bool valid = true;
    std::vector<std::string> errors;
  } request;
  std::vector<std::string> warnings;
  bool ok = true;
};

// Live observation of /proc, /sys and the directory `dir`. Throws coffee::Error("io_error") if `dir` does not exist, is not a
// directory, is not readable and searchable by this process, or cannot be stat'ed. Never creates or writes anything.
HostInputs observe_live(const std::filesystem::path& dir);

// Deterministic evaluation of already-collected inputs.
Report evaluate(const HostInputs& in, const Reserves& reserves);

// Dry-run check of requested quantities against report.budget. Fails (ok=false, request.valid=false, one error per failing
// quantity) when a requested maximum is unknown or insufficient.
void apply_request(Report& report, const Request& req);

// Presentation helper: `s` with every invalid UTF-8 sequence (stray/truncated/overlong/surrogate/out-of-range bytes) replaced by
// U+FFFD; valid UTF-8 is returned unchanged. `*replaced` (optional) reports whether anything was replaced. Only for display:
// the filesystem is always accessed with the original bytes, and the result cannot be mapped back to the original name.
std::string to_valid_utf8(const std::string& s, bool* replaced = nullptr);

// Schema version 1 JSON (unknown -> null; non-finite doubles -> null with a warning).
nlohmann::json to_json(const Report& report);

}  // namespace coffee::host
