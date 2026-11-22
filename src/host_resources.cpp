#include "coffee/host_resources.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include "coffee/util.hpp"

namespace coffee::host {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr uint64_t kMax64 = std::numeric_limits<uint64_t>::max();
constexpr size_t kMaxCgroupFile = 4096;
constexpr size_t kMaxProcFile = 16u << 20;  // mountinfo can be large on container hosts
constexpr int kMaxAffinityCpus = 1 << 18;

// ---- strict number parsing ------------------------------------------------------------------------------------------------

// Unsigned decimal digits only (no sign, space, prefix or trailing text); nullopt on overflow.
std::optional<uint64_t> parse_u64(std::string_view s) {
  if (s.empty()) return std::nullopt;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (kMax64 - d) / 10) return std::nullopt;
    v = v * 10 + d;
  }
  return v;
}

// A cgroup file body is one line; accept exactly one optional trailing newline.
std::string_view one_line(std::string_view s) {
  if (!s.empty() && s.back() == '\n') s.remove_suffix(1);
  return s;
}

std::optional<uint64_t> sat_sub(std::optional<uint64_t> a, uint64_t b) {
  if (!a) return std::nullopt;
  return *a > b ? *a - b : 0;
}

std::string errno_text(int e) { return std::strerror(e); }

// ---- path helpers ---------------------------------------------------------------------------------------------------------

// Absolute normalized path -> components. "/" -> {}. Rejects relative, empty/"."/".." components, trailing '/', NUL.
std::optional<std::vector<std::string>> split_components(std::string_view p) {
  if (p.empty() || p[0] != '/') return std::nullopt;
  std::vector<std::string> out;
  if (p == "/") return out;
  size_t i = 1;
  while (true) {
    size_t j = p.find('/', i);
    std::string_view c = p.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
    if (c.empty() || c == "." || c == ".." || c.find('\0') != std::string_view::npos) return std::nullopt;
    out.emplace_back(c);
    if (j == std::string_view::npos) break;
    i = j + 1;
  }
  return out;
}

std::string join_components(const std::vector<std::string>& c, size_t n) {
  if (n == 0) return "/";
  std::string s;
  for (size_t i = 0; i < n; i++) s += "/" + c[i];
  return s;
}

// mountinfo escapes space/tab/newline/backslash as \NNN (three octal digits). Any other backslash is malformed.
std::optional<std::string> unescape_mountinfo(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\') {
      out += s[i];
      continue;
    }
    int v = 0;
    for (size_t k = 1; k <= 3; k++) {
      if (i + k >= s.size()) return std::nullopt;
      char c = s[i + k];
      if (c < '0' || c > '7') return std::nullopt;
      v = v * 8 + (c - '0');
    }
    if (v > 0xFF) return std::nullopt;
    out += static_cast<char>(v);
    i += 3;
  }
  return out;
}

std::vector<std::string_view> split_on(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(sep, i);
    if (j == std::string_view::npos) j = s.size();
    out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

// ---- file reading ---------------------------------------------------------------------------------------------------------

bool read_fd(int fd, size_t cap, std::string& out, std::string& err) {
  char buf[4096];
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      err = errno_text(errno);
      return false;
    }
    if (n == 0) return true;
    if (out.size() + static_cast<size_t>(n) > cap) {
      err = "file larger than " + std::to_string(cap) + " bytes";
      return false;
    }
    out.append(buf, static_cast<size_t>(n));
  }
}

std::optional<std::string> read_text_file(const fs::path& p, size_t cap, std::string& err) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    err = "cannot read " + p.string() + ": " + errno_text(errno);
    return std::nullopt;
  }
  std::string out;
  std::string e;
  bool ok = read_fd(fd, cap, out, e);
  ::close(fd);
  if (!ok) {
    err = "cannot read " + p.string() + ": " + e;
    return std::nullopt;
  }
  return out;
}

// Opens fs_root, then every component with O_NOFOLLOW|O_DIRECTORY, so nothing outside fs_root/<components> can be reached.
int open_beneath(const fs::path& fs_root, const std::vector<std::string>& comps, std::string& err) {
  int fd = ::open(fs_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    err = "cannot open " + fs_root.string() + ": " + errno_text(errno);
    return -1;
  }
  for (const auto& c : comps) {
    int next = ::openat(fd, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    ::close(fd);
    if (next < 0) {
      err = "cannot open directory component '" + c + "': " + errno_text(e);
      errno = e;
      return -1;
    }
    fd = next;
  }
  return fd;
}

// ---- cgroup2 resolver -----------------------------------------------------------------------------------------------------

struct MountCandidate {
  std::vector<std::string> root;
  std::vector<std::string> mount_point;
};

CgroupHierarchy unresolved(const fs::path& fs_root, const std::string& why) {
  CgroupHierarchy h;
  h.fs_root = fs_root;
  h.error = why;
  return h;
}

bool dir_has_cgroup_events(const fs::path& fs_root, const std::vector<std::string>& comps) {
  std::string err;
  int dfd = open_beneath(fs_root, comps, err);
  if (dfd < 0) return true;  // cannot positively show this is the global root: treat as a namespace root (fail closed)
  int f = ::openat(dfd, "cgroup.events", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  int e = errno;
  if (f >= 0) ::close(f);
  ::close(dfd);
  return !(f < 0 && e == ENOENT);
}

}  // namespace

const char* to_string(LimitStatus s) {
  switch (s) {
    case LimitStatus::kLimited: return "limited";
    case LimitStatus::kUnlimited: return "unlimited";
    case LimitStatus::kUnknown: break;
  }
  return "unknown";
}

// ---- parsers --------------------------------------------------------------------------------------------------------------

MemInfo parse_meminfo(std::string_view text) {
  MemInfo m;
  std::map<std::string, std::vector<std::string_view>> found;
  for (auto line : split_on(text, '\n')) {
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view key = line.substr(0, colon);
    if (key == "MemTotal" || key == "MemAvailable") found[std::string(key)].push_back(line.substr(colon + 1));
  }
  auto value = [&](const char* key) -> std::optional<uint64_t> {
    auto it = found.find(key);
    if (it == found.end()) {
      m.warnings.push_back(std::string(key) + " is missing from /proc/meminfo");
      return std::nullopt;
    }
    if (it->second.size() != 1) {
      m.warnings.push_back(std::string(key) + " appears more than once in /proc/meminfo; value is ambiguous");
      return std::nullopt;
    }
    std::string_view v = it->second[0];
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) i++;
    size_t d = i;
    while (d < v.size() && v[d] >= '0' && v[d] <= '9') d++;
    auto kib = parse_u64(v.substr(i, d - i));
    size_t u = d;
    while (u < v.size() && (v[u] == ' ' || v[u] == '\t')) u++;
    if (!kib || u == d || v.substr(u) != "kB") {
      m.warnings.push_back(std::string(key) + " in /proc/meminfo is malformed");
      return std::nullopt;
    }
    if (*kib > kMax64 / 1024) {
      m.warnings.push_back(std::string(key) + " in /proc/meminfo overflows 64-bit bytes");
      return std::nullopt;
    }
    return *kib * 1024;
  };
  m.total_bytes = value("MemTotal");
  m.available_bytes = value("MemAvailable");
  return m;
}

std::optional<CpuMax> parse_cpu_max(std::string_view text) {
  std::string_view s = one_line(text);
  size_t sp = s.find(' ');
  if (sp == std::string_view::npos) return std::nullopt;
  std::string_view q = s.substr(0, sp), p = s.substr(sp + 1);
  CpuMax out;
  auto period = parse_u64(p);  // rejects a second token, signs, spaces and overflow
  if (!period || *period == 0) return std::nullopt;
  out.period_us = *period;
  if (q != "max") {
    auto quota = parse_u64(q);
    if (!quota) return std::nullopt;
    out.quota_us = *quota;
  }
  return out;
}

std::optional<MemLimit> parse_memory_max(std::string_view text) {
  std::string_view s = one_line(text);
  MemLimit l;
  if (s == "max") {
    l.unlimited = true;
    return l;
  }
  auto v = parse_u64(s);
  if (!v) return std::nullopt;
  l.bytes = *v;
  return l;
}

std::optional<uint64_t> parse_uint_file(std::string_view text) { return parse_u64(one_line(text)); }

// ---- resolver -------------------------------------------------------------------------------------------------------------

CgroupHierarchy resolve_cgroup2(std::string_view self_cgroup, std::string_view mountinfo, const fs::path& fs_root) {
  // 1. unified membership: exactly one "0::PATH" line
  std::optional<std::string> membership;
  for (auto line : split_on(self_cgroup, '\n')) {
    if (line.empty()) continue;
    size_t c1 = line.find(':');
    size_t c2 = c1 == std::string_view::npos ? c1 : line.find(':', c1 + 1);
    if (c2 == std::string_view::npos) return unresolved(fs_root, "malformed /proc/self/cgroup record");
    if (line.substr(0, c1) == "0" && c2 == c1 + 1) {
      if (membership) return unresolved(fs_root, "more than one unified (0::) cgroup membership record");
      membership = std::string(line.substr(c2 + 1));
    }
  }
  if (!membership) return unresolved(fs_root, "no unified cgroup v2 membership (0::) in /proc/self/cgroup; cgroup v1 is not supported");
  auto member = split_components(*membership);
  if (!member) return unresolved(fs_root, "cgroup membership path is not an absolute normalized path");

  // 2. cgroup2 mounts from mountinfo
  std::vector<MountCandidate> candidates;
  for (auto line : split_on(mountinfo, '\n')) {
    if (line.empty()) continue;
    std::vector<std::string_view> f;
    for (auto t : split_on(line, ' '))
      if (!t.empty()) f.push_back(t);
    size_t sep = 0;
    for (size_t i = 6; i < f.size(); i++)
      if (f[i] == "-") {
        sep = i;
        break;
      }
    if (sep == 0 || f.size() < sep + 4) return unresolved(fs_root, "malformed /proc/self/mountinfo record");
    if (f[sep + 1] != "cgroup2") continue;
    auto root = unescape_mountinfo(f[3]);
    auto mp = unescape_mountinfo(f[4]);
    if (!root || !mp) return unresolved(fs_root, "malformed escape in a cgroup2 mountinfo record");
    auto root_c = split_components(*root);
    auto mp_c = split_components(*mp);
    if (!root_c || !mp_c) return unresolved(fs_root, "cgroup2 mount root or mount point is not an absolute normalized path");
    candidates.push_back({std::move(*root_c), std::move(*mp_c)});
  }

  // 3. longest component-prefix root that contains the membership
  const MountCandidate* best = nullptr;
  bool ambiguous = false;
  for (const auto& c : candidates) {
    if (c.root.size() > member->size() || !std::equal(c.root.begin(), c.root.end(), member->begin())) continue;
    if (!best || c.root.size() > best->root.size()) {
      best = &c;
      ambiguous = false;
    } else if (c.root.size() == best->root.size() && c.mount_point != best->mount_point) {
      ambiguous = true;
    }
  }
  if (!best) return unresolved(fs_root, "cgroup membership " + *membership + " is not below any visible cgroup2 mount root (namespace or no unified mount)");
  if (ambiguous) return unresolved(fs_root, "several cgroup2 mounts are equally specific for " + *membership + "; mapping is ambiguous");

  // 4. groups, leaf first, ending with the mount point inclusive
  CgroupHierarchy h;
  h.fs_root = fs_root;
  h.resolved = true;
  std::vector<std::string> rel(member->begin() + static_cast<std::ptrdiff_t>(best->root.size()), member->end());
  for (size_t keep = rel.size() + 1; keep-- > 0;) {
    std::vector<std::string> comps = best->mount_point;
    comps.insert(comps.end(), rel.begin(), rel.begin() + static_cast<std::ptrdiff_t>(keep));
    CgroupGroup g;
    g.path = join_components(comps, comps.size());
    g.visible_root = keep == 0;
    g.global_root = keep == 0 && best->root.empty() && !dir_has_cgroup_events(fs_root, comps);
    h.groups.push_back(std::move(g));
  }
  return h;
}

FileRead read_cgroup_file(const CgroupHierarchy& h, size_t index, const std::string& name) {
  FileRead r;
  if (!h.resolved || index >= h.groups.size()) {
    r.error = "no such cgroup group";
    return r;
  }
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
    r.error = "invalid cgroup file name";
    return r;
  }
  auto comps = split_components(h.groups[index].path);
  if (!comps) {
    r.error = "invalid group path";
    return r;
  }
  std::string err;
  int dfd = open_beneath(h.fs_root, *comps, err);
  if (dfd < 0) {
    r.error = err + " (" + h.groups[index].path + ")";
    return r;
  }
  int fd = ::openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  int e = errno;
  ::close(dfd);
  std::string where = h.groups[index].path + "/" + name;
  if (fd < 0) {
    if (e == ENOENT) {
      r.status = FileRead::Status::kMissing;
      r.error = where + " does not exist";
    } else {
      r.error = "cannot open " + where + ": " + errno_text(e) + (e == ELOOP ? " (symbolic links are not followed)" : "");
    }
    return r;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    r.error = where + " is not a regular file";
    return r;
  }
  std::string text, rerr;
  bool ok = read_fd(fd, kMaxCgroupFile, text, rerr);
  ::close(fd);
  if (!ok) {
    r.error = "cannot read " + where + ": " + rerr;
    return r;
  }
  r.status = FileRead::Status::kOk;
  r.text = std::move(text);
  return r;
}

CpuQuota observe_cpu_quota(const CgroupHierarchy& h) {
  CpuQuota q;
  if (!h.resolved) {
    q.warnings.push_back("cgroup CPU quota is unknown: " + h.error);
    return q;
  }
  bool unknown = false;
  std::optional<double> tightest;
  for (size_t i = 0; i < h.groups.size(); i++) {
    FileRead r = read_cgroup_file(h, i, "cpu.max");
    if (r.status == FileRead::Status::kMissing && h.groups[i].global_root) continue;  // the real root has no cpu.max: no cap there
    if (r.status != FileRead::Status::kOk) {
      unknown = true;
      q.warnings.push_back("cgroup CPU quota is unknown: " + r.error + (r.status == FileRead::Status::kMissing ? " (cpu controller not visible for this group)" : ""));
      continue;
    }
    auto cm = parse_cpu_max(r.text);
    if (!cm) {
      unknown = true;
      q.warnings.push_back("cgroup CPU quota is unknown: malformed cpu.max in " + h.groups[i].path);
      continue;
    }
    if (!cm->quota_us) continue;  // "max"
    double cores = static_cast<double>(*cm->quota_us) / static_cast<double>(cm->period_us);
    if (!std::isfinite(cores)) {
      unknown = true;
      q.warnings.push_back("cgroup CPU quota is unknown: non-finite quota in " + h.groups[i].path);
      continue;
    }
    if (!tightest || cores < *tightest) tightest = cores;
  }
  if (unknown) return q;  // never print a partial minimum
  if (tightest) {
    q.status = LimitStatus::kLimited;
    q.quota_cores = tightest;
  } else {
    q.status = LimitStatus::kUnlimited;
  }
  return q;
}

MemoryCgroup observe_memory_cgroup(const CgroupHierarchy& h) {
  MemoryCgroup m;
  if (!h.resolved) {
    m.warnings.push_back("cgroup memory limit is unknown: " + h.error);
    return m;
  }
  bool unknown = false;
  std::optional<uint64_t> limit, headroom;
  for (size_t i = 0; i < h.groups.size(); i++) {
    FileRead r = read_cgroup_file(h, i, "memory.max");
    if (r.status == FileRead::Status::kMissing && h.groups[i].global_root) continue;
    if (r.status != FileRead::Status::kOk) {
      unknown = true;
      m.warnings.push_back("cgroup memory limit is unknown: " + r.error + (r.status == FileRead::Status::kMissing ? " (memory controller not visible for this group)" : ""));
      continue;
    }
    auto ml = parse_memory_max(r.text);
    if (!ml) {
      unknown = true;
      m.warnings.push_back("cgroup memory limit is unknown: malformed memory.max in " + h.groups[i].path);
      continue;
    }
    if (ml->unlimited) continue;
    FileRead c = read_cgroup_file(h, i, "memory.current");
    std::optional<uint64_t> cur;
    if (c.status == FileRead::Status::kOk) cur = parse_uint_file(c.text);
    if (!cur) {
      unknown = true;
      m.warnings.push_back("cgroup memory headroom is unknown: " + (c.status == FileRead::Status::kOk ? "malformed memory.current in " + h.groups[i].path : c.error));
      continue;
    }
    uint64_t room = ml->bytes > *cur ? ml->bytes - *cur : 0;
    if (!limit || ml->bytes < *limit) limit = ml->bytes;
    if (!headroom || room < *headroom) headroom = room;
  }
  if (unknown) return m;
  if (limit) {
    m.status = LimitStatus::kLimited;
    m.limit_bytes = limit;
    m.headroom_bytes = headroom;
  } else {
    m.status = LimitStatus::kUnlimited;
  }
  return m;
}

// ---- topology and storage -------------------------------------------------------------------------------------------------

namespace {
// Plain non-negative decimal id from a sysfs topology file; "-1" (not reported) and anything else is unknown.
std::optional<uint64_t> read_topology_id(const fs::path& p) {
  std::string err;
  auto text = read_text_file(p, 64, err);
  if (!text) return std::nullopt;
  return parse_u64(one_line(*text));
}
}  // namespace

Topology read_topology(const fs::path& sys_root, const std::vector<int>& cpus) {
  Topology t;
  if (cpus.empty()) {
    t.warning = "physical core count is unknown: no allowed CPU to inspect";
    return t;
  }
  std::set<std::pair<uint64_t, uint64_t>> cores;
  for (int cpu : cpus) {
    fs::path dir = sys_root / "devices/system/cpu" / ("cpu" + std::to_string(cpu)) / "topology";
    auto pkg = read_topology_id(dir / "physical_package_id");
    auto core = read_topology_id(dir / "core_id");
    if (!pkg || !core) {
      t.warning = "physical core count is unknown: incomplete topology for cpu" + std::to_string(cpu);
      return t;
    }
    cores.emplace(*pkg, *core);
  }
  t.physical_cores = cores.size();
  return t;
}

StorageNumbers evaluate_storage(const FsStat& st) {
  StorageNumbers n;
  if (st.frsize == 0) {
    n.error = "filesystem reports a zero fragment size";
    return n;
  }
  if (st.bavail > st.blocks || st.bfree > st.blocks) {
    n.error = "filesystem reports more free blocks than total blocks";
    return n;
  }
  uint64_t total = 0, avail = 0;
  if (__builtin_mul_overflow(st.blocks, st.frsize, &total) || __builtin_mul_overflow(st.bavail, st.frsize, &avail)) {
    n.error = "filesystem size overflows 64-bit bytes";
    return n;
  }
  n.total_bytes = total;
  n.available_bytes = avail;
  return n;
}

// ---- live observation -----------------------------------------------------------------------------------------------------

HostInputs observe_live(const fs::path& dir) {
  HostInputs in;
  if (dir.empty()) throw Error("io_error", "cannot access '': empty path");
  const std::string shown = dir.string();
  char* rp = ::realpath(dir.c_str(), nullptr);
  if (!rp) throw Error("io_error", "cannot access '" + shown + "': " + errno_text(errno));
  fs::path resolved = rp;
  std::free(rp);
  int fd = ::open(resolved.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    int e = errno;
    throw Error("io_error", "cannot access '" + shown + "': " + (e == ENOTDIR ? "not a directory" : errno_text(e)));
  }
  if (::faccessat(AT_FDCWD, resolved.c_str(), R_OK | X_OK, AT_EACCESS) != 0) {
    int e = errno;
    ::close(fd);
    throw Error("io_error", "cannot access '" + shown + "': directory is not readable and searchable (" + errno_text(e) + ")");
  }
  struct statvfs sv;
  if (::fstatvfs(fd, &sv) != 0) {
    int e = errno;
    ::close(fd);
    throw Error("io_error", "cannot stat the filesystem of '" + shown + "': " + errno_text(e));
  }
  ::close(fd);
  in.storage_path = resolved;
  in.fs_stat = FsStat{static_cast<uint64_t>(sv.f_frsize), static_cast<uint64_t>(sv.f_blocks), static_cast<uint64_t>(sv.f_bfree), static_cast<uint64_t>(sv.f_bavail)};

  // CPU affinity, with a set that grows until it fits (never the machine-wide thread count)
  for (int n = 1024; n <= kMaxAffinityCpus; n *= 2) {
    cpu_set_t* set = CPU_ALLOC(static_cast<size_t>(n));
    if (!set) {
      in.affinity_error = "out of memory reading CPU affinity";
      break;
    }
    size_t bytes = CPU_ALLOC_SIZE(static_cast<size_t>(n));
    CPU_ZERO_S(bytes, set);
    if (::sched_getaffinity(0, bytes, set) == 0) {
      std::vector<int> cpus;
      for (int c = 0; c < n; c++)
        if (CPU_ISSET_S(c, bytes, set)) cpus.push_back(c);
      CPU_FREE(set);
      in.affinity_cpus = std::move(cpus);
      in.affinity_error.clear();
      break;
    }
    int e = errno;
    CPU_FREE(set);
    if (e != EINVAL) {
      in.affinity_error = "sched_getaffinity failed: " + errno_text(e);
      break;
    }
    in.affinity_error = "more than " + std::to_string(kMaxAffinityCpus) + " CPUs are possible; affinity set is too large to enumerate";
  }

  std::string err;
  in.meminfo = read_text_file("/proc/meminfo", 1 << 20, err);
  if (!in.meminfo) in.meminfo_error = err;
  std::string cerr1, cerr2;
  in.self_cgroup = read_text_file("/proc/self/cgroup", 1 << 20, cerr1);
  in.mountinfo = read_text_file("/proc/self/mountinfo", kMaxProcFile, cerr2);
  in.cgroup_error = cerr1.empty() ? cerr2 : cerr1;
  return in;
}

// ---- evaluation -----------------------------------------------------------------------------------------------------------

Report evaluate(const HostInputs& in, const Reserves& reserves) {
  Report r;
  r.budget.reserve_memory_bytes = reserves.memory_bytes;
  r.budget.reserve_storage_bytes = reserves.storage_bytes;

  CgroupHierarchy h;
  if (in.self_cgroup && in.mountinfo) h = resolve_cgroup2(*in.self_cgroup, *in.mountinfo, in.fs_root);
  else {
    h.fs_root = in.fs_root;
    h.error = in.cgroup_error.empty() ? "/proc/self/cgroup or /proc/self/mountinfo is unavailable" : in.cgroup_error;
  }

  // CPU
  if (in.affinity_cpus && !in.affinity_cpus->empty()) {
    r.cpu.affinity_logical_cpus = in.affinity_cpus->size();
    Topology t = read_topology(in.sys_root, *in.affinity_cpus);
    r.cpu.physical_cores = t.physical_cores;
    if (!t.physical_cores) r.warnings.push_back(t.warning);
  } else {
    r.warnings.push_back("logical CPU count is unknown: " + (in.affinity_error.empty() ? std::string("empty or unavailable CPU affinity set") : in.affinity_error));
  }
  CpuQuota q = observe_cpu_quota(h);
  r.cpu.quota_status = q.status;
  r.cpu.quota_cores = q.quota_cores;
  r.warnings.insert(r.warnings.end(), q.warnings.begin(), q.warnings.end());
  if (r.cpu.affinity_logical_cpus && q.status != LimitStatus::kUnknown) {
    double cpus = static_cast<double>(*r.cpu.affinity_logical_cpus);
    r.cpu.effective_cores = q.quota_cores ? std::min(cpus, *q.quota_cores) : cpus;
    r.budget.max_cpu_slots = static_cast<uint64_t>(std::floor(*r.cpu.effective_cores));
  }

  // RAM
  if (in.meminfo) {
    MemInfo mi = parse_meminfo(*in.meminfo);
    r.memory.host_total_bytes = mi.total_bytes;
    r.memory.host_available_bytes = mi.available_bytes;
    r.warnings.insert(r.warnings.end(), mi.warnings.begin(), mi.warnings.end());
    if (mi.total_bytes && mi.available_bytes && *mi.available_bytes > *mi.total_bytes) {
      r.memory.host_available_bytes = mi.total_bytes;
      r.warnings.push_back("MemAvailable exceeds MemTotal in /proc/meminfo; capped at MemTotal");
    }
  } else {
    r.warnings.push_back("host memory is unknown: " + (in.meminfo_error.empty() ? std::string("/proc/meminfo unavailable") : in.meminfo_error));
  }
  MemoryCgroup mc = observe_memory_cgroup(h);
  r.memory.cgroup_status = mc.status;
  r.memory.cgroup_limit_bytes = mc.limit_bytes;
  r.memory.cgroup_headroom_bytes = mc.headroom_bytes;
  r.warnings.insert(r.warnings.end(), mc.warnings.begin(), mc.warnings.end());
  if (mc.status != LimitStatus::kUnknown && r.memory.host_available_bytes) {
    uint64_t eff = *r.memory.host_available_bytes;
    if (mc.headroom_bytes) eff = std::min(eff, *mc.headroom_bytes);
    r.memory.effective_available_bytes = eff;
  }
  r.budget.max_memory_bytes = sat_sub(r.memory.effective_available_bytes, reserves.memory_bytes);

  // Storage
  r.storage.path = in.storage_path.string();
  if (in.fs_stat) {
    StorageNumbers sn = evaluate_storage(*in.fs_stat);
    r.storage.total_bytes = sn.total_bytes;
    r.storage.available_bytes = sn.available_bytes;
    if (!sn.error.empty()) r.warnings.push_back("storage is unknown: " + sn.error);
  } else {
    r.warnings.push_back("storage is unknown: " + (in.storage_error.empty() ? std::string("filesystem statistics unavailable") : in.storage_error));
  }
  r.budget.max_storage_bytes = sat_sub(r.storage.available_bytes, reserves.storage_bytes);
  return r;
}

void apply_request(Report& report, const Request& req) {
  report.request.cpus = req.cpus;
  report.request.memory_bytes = req.memory_bytes;
  report.request.storage_bytes = req.storage_bytes;
  report.request.errors.clear();
  auto check = [&](const char* what, const char* unit, std::optional<uint64_t> want, std::optional<uint64_t> max) {
    if (!want) return;
    if (!max)
      report.request.errors.push_back(std::string("requested ") + what + " cannot be verified: the maximum is unknown on this host");
    else if (*want > *max)
      report.request.errors.push_back(std::string("requested ") + what + " " + std::to_string(*want) + " " + unit + " exceeds the maximum of " + std::to_string(*max) + " " + unit);
  };
  check("cpu slots", "slots", req.cpus, report.budget.max_cpu_slots);
  check("memory", "bytes", req.memory_bytes, report.budget.max_memory_bytes);
  check("storage", "bytes", req.storage_bytes, report.budget.max_storage_bytes);
  report.request.valid = report.request.errors.empty();
  report.ok = report.request.valid;
}

std::string to_valid_utf8(const std::string& s, bool* replaced) {
  // The JSON library's replace handler substitutes U+FFFD for each invalid sequence; parsing its output back gives the display string.
  std::string out = json::parse(json(s).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
  if (replaced) *replaced = out != s;
  return out;
}

// ---- JSON -----------------------------------------------------------------------------------------------------------------

namespace {
json opt(const std::optional<uint64_t>& v) { return v ? json(*v) : json(nullptr); }
}  // namespace

json to_json(const Report& r) {
  // Presentation only: invalid UTF-8 (legal in Linux file names) is shown as U+FFFD so the object always serializes.
  std::vector<std::string> warnings;
  for (const auto& w : r.warnings) warnings.push_back(to_valid_utf8(w));
  bool path_replaced = false;
  std::string shown_path = to_valid_utf8(r.storage.path, &path_replaced);
  if (path_replaced)
    warnings.push_back("storage.path contains bytes that are not valid UTF-8 and is shown with U+FFFD replacements; the observation used the original path, "
                       "but the displayed path cannot be used to recover the original file name");
  std::vector<std::string> request_errors;
  for (const auto& e : r.request.errors) request_errors.push_back(to_valid_utf8(e));
  auto finite = [&](const char* what, const std::optional<double>& v) -> json {
    if (!v) return nullptr;
    if (!std::isfinite(*v) || *v < 0) {
      warnings.push_back(std::string(what) + " was not a finite non-negative number and is reported as unknown");
      return nullptr;
    }
    return *v;
  };
  json cpu = {{"affinity_logical_cpus", opt(r.cpu.affinity_logical_cpus)},
              {"physical_cores", opt(r.cpu.physical_cores)},
              {"quota_cores", finite("quota_cores", r.cpu.quota_cores)},
              {"quota_status", to_string(r.cpu.quota_status)},
              {"effective_cores", finite("effective_cores", r.cpu.effective_cores)}};
  json mem = {{"host_total_bytes", opt(r.memory.host_total_bytes)},
              {"host_available_bytes", opt(r.memory.host_available_bytes)},
              {"cgroup_limit_bytes", opt(r.memory.cgroup_limit_bytes)},
              {"cgroup_headroom_bytes", opt(r.memory.cgroup_headroom_bytes)},
              {"cgroup_status", to_string(r.memory.cgroup_status)},
              {"effective_available_bytes", opt(r.memory.effective_available_bytes)}};
  json storage = {{"path", shown_path}, {"total_bytes", opt(r.storage.total_bytes)}, {"available_bytes", opt(r.storage.available_bytes)}};
  json budget = {{"reserve_memory_bytes", r.budget.reserve_memory_bytes},
                 {"reserve_storage_bytes", r.budget.reserve_storage_bytes},
                 {"max_cpu_slots", opt(r.budget.max_cpu_slots)},
                 {"max_memory_bytes", opt(r.budget.max_memory_bytes)},
                 {"max_storage_bytes", opt(r.budget.max_storage_bytes)}};
  json request = {{"cpus", opt(r.request.cpus)},
                  {"memory_bytes", opt(r.request.memory_bytes)},
                  {"storage_bytes", opt(r.request.storage_bytes)},
                  {"valid", r.request.valid},
                  {"errors", request_errors}};
  return {{"schema_version", 1}, {"ok", r.ok}, {"cpu", cpu}, {"memory", mem}, {"storage", storage}, {"budget", budget}, {"request", request}, {"warnings", warnings}};
}

}  // namespace coffee::host
