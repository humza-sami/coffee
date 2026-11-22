#include <catch_amalgamated.hpp>

#include <sched.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "coffee/cli.hpp"
#include "coffee/host_resources.hpp"
#include "coffee/util.hpp"

using namespace coffee;
using namespace coffee::host;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kMax64 = std::numeric_limits<uint64_t>::max();

// A throw-away directory tree used as the fake root of /proc, /sys and the cgroup hierarchy.
struct Tree {
  fs::path root;
  Tree() {
    root = fs::temp_directory_path() / ("coffee-test-host-" + random_hex(6));
    fs::create_directories(root);
  }
  ~Tree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void put(const std::string& rel, const std::string& text) {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel, std::ios::binary) << text;
  }
  void dir(const std::string& rel) { fs::create_directories(root / rel); }
};

const std::string kCg = "sys/fs/cgroup";  // fixture-relative cgroup2 mount point

std::string mount_line(const std::string& root, const std::string& mount_point, const std::string& fstype = "cgroup2", int id = 36) {
  return std::to_string(id) + " 28 0:30 " + root + " " + mount_point + " rw,nosuid,nodev,noexec,relatime shared:8 - " + fstype + " cgroup2 rw,nsdelegate\n";
}
const std::string kGlobalMount = mount_line("/", "/sys/fs/cgroup");

void cgroup_files(Tree& t, const std::string& group, const std::string& cpu_max, const std::string& mem_max = "", const std::string& mem_cur = "") {
  std::string base = kCg + group;
  t.dir(base);
  if (!cpu_max.empty()) t.put(base + "/cpu.max", cpu_max);
  if (!mem_max.empty()) t.put(base + "/memory.max", mem_max);
  if (!mem_cur.empty()) t.put(base + "/memory.current", mem_cur);
}

CgroupHierarchy resolve(const Tree& t, const std::string& membership, const std::string& mounts = kGlobalMount) {
  return resolve_cgroup2("0::" + membership + "\n", mounts, t.root);
}

std::string meminfo(uint64_t total_kib, uint64_t avail_kib) {
  return "MemTotal:       " + std::to_string(total_kib) + " kB\nMemFree:         1 kB\nMemAvailable:   " + std::to_string(avail_kib) + " kB\nBuffers: 5 kB\n";
}

// Complete deterministic inputs: 8 GiB host with 6 GiB available, 4 allowed CPUs with a fixture topology, and `stat`.
HostInputs inputs(const Tree& t, const std::string& membership, const std::string& mounts = kGlobalMount) {
  HostInputs in;
  in.affinity_cpus = std::vector<int>{0, 1, 2, 3};
  in.sys_root = t.root / "sys";
  in.meminfo = meminfo(8 * 1024 * 1024, 6 * 1024 * 1024);
  in.self_cgroup = "0::" + membership + "\n";
  in.mountinfo = mounts;
  in.fs_root = t.root;
  in.storage_path = "/fixture/disk";
  in.fs_stat = FsStat{4096, 100 * kGiB / 4096, 20 * kGiB / 4096, 10 * kGiB / 4096};
  return in;
}

void topology(Tree& t, int cpu, const std::string& pkg, const std::string& core) {
  std::string d = "sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
  t.put(d + "physical_package_id", pkg + "\n");
  t.put(d + "core_id", core + "\n");
}

struct Result {
  int code;
  std::string out, err;
};
Result run(std::vector<std::string> args) {
  std::ostringstream out, err;
  int code = run_cli(args, out, err);
  return {code, out.str(), err.str()};
}

json parse_single(const std::string& out) {
  REQUIRE_FALSE(out.empty());
  REQUIRE(out.back() == '\n');
  json j = json::parse(out);  // throws unless stdout is exactly one JSON document
  return j;
}

uint64_t own_affinity_count() {
  cpu_set_t set;
  CPU_ZERO(&set);
  REQUIRE(sched_getaffinity(0, sizeof(set), &set) == 0);
  return static_cast<uint64_t>(CPU_COUNT(&set));
}

}  // namespace

// ---- 1. CPU affinity and topology ---------------------------------------------------------------------------------------

TEST_CASE("physical cores are distinct (package, core) pairs of the allowed CPUs only", "[host][cpu]") {
  Tree t;
  topology(t, 0, "0", "0");  // not allowed; must not count
  topology(t, 1, "0", "1");  // not allowed
  topology(t, 2, "0", "0");
  topology(t, 4, "1", "0");  // other package, same core id
  auto two_pkg = read_topology(t.root / "sys", {2, 4});
  REQUIRE(two_pkg.physical_cores.has_value());
  CHECK(*two_pkg.physical_cores == 2);

  topology(t, 4, "0", "0");  // same package, same core id: SMT siblings
  auto one = read_topology(t.root / "sys", {2, 4});
  REQUIRE(one.physical_cores.has_value());
  CHECK(*one.physical_cores == 1);

  CHECK(*read_topology(t.root / "sys", {0, 1, 2}).physical_cores == 2);
}

TEST_CASE("incomplete or malformed topology makes physical cores unknown", "[host][cpu]") {
  Tree t;
  topology(t, 2, "0", "0");
  auto missing_cpu = read_topology(t.root / "sys", {2, 4});  // cpu4 has no topology directory
  CHECK_FALSE(missing_cpu.physical_cores.has_value());
  CHECK_FALSE(missing_cpu.warning.empty());

  t.put("sys/devices/system/cpu/cpu4/topology/physical_package_id", "0\n");  // core_id absent
  CHECK_FALSE(read_topology(t.root / "sys", {2, 4}).physical_cores.has_value());

  topology(t, 4, "-1", "3");  // negative package id (virtualized hosts): unknown, never counted as a package
  CHECK_FALSE(read_topology(t.root / "sys", {2, 4}).physical_cores.has_value());
  topology(t, 4, "0", "abc");
  CHECK_FALSE(read_topology(t.root / "sys", {2, 4}).physical_cores.has_value());
  topology(t, 4, "0", "1junk");
  CHECK_FALSE(read_topology(t.root / "sys", {2, 4}).physical_cores.has_value());
  CHECK_FALSE(read_topology(t.root / "sys", {}).physical_cores.has_value());  // no allowed CPU: nothing was observed
}

TEST_CASE("evaluate reports injected affinity as logical CPUs and topology as descriptive physical cores", "[host][cpu]") {
  Tree t;
  topology(t, 2, "0", "0");
  topology(t, 4, "1", "0");
  t.dir(kCg);
  HostInputs in = inputs(t, "/");
  in.affinity_cpus = std::vector<int>{2, 4};
  Report r = evaluate(in, {});
  CHECK(r.cpu.affinity_logical_cpus == 2);
  CHECK(r.cpu.physical_cores == 2);
  CHECK(r.cpu.quota_status == LimitStatus::kUnlimited);  // global root, no cpu.max anywhere
  CHECK_FALSE(r.cpu.quota_cores.has_value());
  CHECK(r.cpu.effective_cores == 2.0);
  CHECK(r.budget.max_cpu_slots == 2);

  in.affinity_cpus = std::nullopt;  // sched_getaffinity failed
  in.affinity_error = "boom";
  Report u = evaluate(in, {});
  CHECK_FALSE(u.cpu.affinity_logical_cpus.has_value());
  CHECK_FALSE(u.cpu.physical_cores.has_value());
  CHECK_FALSE(u.cpu.effective_cores.has_value());
  CHECK_FALSE(u.budget.max_cpu_slots.has_value());
  CHECK_FALSE(u.warnings.empty());
}

// ---- 2. CPU quota across the cgroup hierarchy ---------------------------------------------------------------------------

TEST_CASE("cpu.max parser is strict", "[host][cpu]") {
  auto ok = parse_cpu_max("150000 100000\n");
  REQUIRE(ok.has_value());
  CHECK(ok->quota_us == 150000);
  CHECK(ok->period_us == 100000);
  CHECK_FALSE(parse_cpu_max("max 100000\n")->quota_us.has_value());
  CHECK(parse_cpu_max("0 100000\n")->quota_us == 0);  // a zero limit is valid
  CHECK(parse_cpu_max("50000 100000")->period_us == 100000);  // missing final newline tolerated
  for (const char* bad : {"", "\n", "max", "max 0\n", "100 0\n", "-1 100000\n", "+5 100000\n", "100 -5\n", "100 100000 7\n", "100  100000\n", "1e3 100000\n",
                          "abc 100000\n", "100 abc\n", "18446744073709551616 100000\n", "100 18446744073709551616\n", "100\t100000\n", " 100 100000\n", "max max\n",
                          "100 100000\n\n", "nan 100000\n", "inf 100000\n"})
    CHECK_FALSE(parse_cpu_max(bad).has_value());
  CHECK(parse_cpu_max("18446744073709551615 1\n")->quota_us == kMax64);  // huge but representable
}

TEST_CASE("tightest finite ancestor quota decides, fractional cores are kept", "[host][cpu][cgroup]") {
  Tree t;
  cgroup_files(t, "/a", "150000 100000\n");
  cgroup_files(t, "/a/b", "max 100000\n");
  HostInputs in = inputs(t, "/a/b");
  Report r = evaluate(in, {});
  CHECK(r.cpu.quota_status == LimitStatus::kLimited);
  REQUIRE(r.cpu.quota_cores.has_value());
  CHECK(*r.cpu.quota_cores == Catch::Approx(1.5));
  REQUIRE(r.cpu.effective_cores.has_value());
  CHECK(*r.cpu.effective_cores == Catch::Approx(1.5));  // min(4 CPUs, 1.5)
  CHECK(r.budget.max_cpu_slots == 1);                   // floor only at the end

  cgroup_files(t, "/a/b", "50000 100000\n");  // tighter leaf is respected
  Report leaf = evaluate(in, {});
  CHECK(*leaf.cpu.quota_cores == Catch::Approx(0.5));
  CHECK(leaf.budget.max_cpu_slots == 0);  // a 0.5 CPU quota cannot support one whole slot
  apply_request(leaf, Request{1, std::nullopt, std::nullopt});
  CHECK_FALSE(leaf.ok);
  CHECK_FALSE(leaf.request.valid);
  CHECK(leaf.request.errors.size() == 1);

  cgroup_files(t, "/a/b", "max 100000\n");
  cgroup_files(t, "/a", "800000 100000\n");  // quota 8 > 4 CPUs of affinity: affinity wins
  Report big = evaluate(in, {});
  CHECK(*big.cpu.quota_cores == Catch::Approx(8.0));
  CHECK(*big.cpu.effective_cores == Catch::Approx(4.0));
  CHECK(big.budget.max_cpu_slots == 4);
}

TEST_CASE("zero CPU quota is a valid limit, not unknown", "[host][cpu][cgroup]") {
  Tree t;
  cgroup_files(t, "/a", "0 100000\n");
  Report r = evaluate(inputs(t, "/a"), {});
  CHECK(r.cpu.quota_status == LimitStatus::kLimited);
  REQUIRE(r.cpu.quota_cores.has_value());
  CHECK(*r.cpu.quota_cores == 0.0);
  REQUIRE(r.cpu.effective_cores.has_value());
  CHECK(*r.cpu.effective_cores == 0.0);
  REQUIRE(r.budget.max_cpu_slots.has_value());
  CHECK(*r.budget.max_cpu_slots == 0);
}

TEST_CASE("all-unlimited visible hierarchy is unlimited; the global root alone needs no cpu.max", "[host][cpu][cgroup]") {
  Tree t;
  cgroup_files(t, "/a", "max 100000\n");
  Report r = evaluate(inputs(t, "/a"), {});
  CHECK(r.cpu.quota_status == LimitStatus::kUnlimited);
  CHECK_FALSE(r.cpu.quota_cores.has_value());
  CHECK(r.cpu.effective_cores == 4.0);
  CHECK(r.budget.max_cpu_slots == 4);

  Tree root_only;
  root_only.dir(kCg);  // process lives in the global root cgroup and the root has no cpu.max
  Report g = evaluate(inputs(root_only, "/"), {});
  CHECK(g.cpu.quota_status == LimitStatus::kUnlimited);
  CHECK(g.cpu.effective_cores == 4.0);
}

TEST_CASE("unreadable or malformed relevant CPU data propagates unknown to every derived value", "[host][cpu][cgroup]") {
  auto expect_unknown = [](const Report& r) {
    CHECK(r.cpu.quota_status == LimitStatus::kUnknown);
    CHECK_FALSE(r.cpu.quota_cores.has_value());
    CHECK_FALSE(r.cpu.effective_cores.has_value());
    CHECK_FALSE(r.budget.max_cpu_slots.has_value());
    CHECK_FALSE(r.warnings.empty());
  };
  SECTION("zero period") {
    Tree t;
    cgroup_files(t, "/a", "100000 0\n");
    expect_unknown(evaluate(inputs(t, "/a"), {}));
  }
  SECTION("explicit max still needs a valid period") {
    Tree t;
    cgroup_files(t, "/a", "max 0\n");
    expect_unknown(evaluate(inputs(t, "/a"), {}));
  }
  SECTION("overflowing quota") {
    Tree t;
    cgroup_files(t, "/a", "18446744073709551616 100000\n");
    expect_unknown(evaluate(inputs(t, "/a"), {}));
  }
  SECTION("finite known ancestor does not hide an unreadable one") {
    Tree t;
    cgroup_files(t, "/a", "");           // cpu.max missing on a non-root ancestor: not unlimited
    cgroup_files(t, "/a/b", "50000 100000\n");
    expect_unknown(evaluate(inputs(t, "/a/b"), {}));
  }
  SECTION("unreadable (a directory in place of the file) leaf") {
    Tree t;
    cgroup_files(t, "/a", "150000 100000\n");
    t.dir(kCg + "/a/b/cpu.max");
    expect_unknown(evaluate(inputs(t, "/a/b"), {}));
  }
  SECTION("missing file in the leaf group") {
    Tree t;
    cgroup_files(t, "/a", "max 100000\n");
    t.dir(kCg + "/a/b");
    expect_unknown(evaluate(inputs(t, "/a/b"), {}));
  }
  SECTION("cgroup v1 only") {
    Tree t;
    t.dir(kCg);
    HostInputs in = inputs(t, "/");
    in.self_cgroup = "1:cpu,cpuacct:/foo\n";
    in.mountinfo = mount_line("/", "/sys/fs/cgroup", "cgroup");
    expect_unknown(evaluate(in, {}));
  }
  SECTION("quota known but affinity unknown keeps quota yet no effective value") {
    Tree t;
    cgroup_files(t, "/a", "150000 100000\n");
    HostInputs in = inputs(t, "/a");
    in.affinity_cpus = std::nullopt;
    Report r = evaluate(in, {});
    CHECK(r.cpu.quota_status == LimitStatus::kLimited);
    CHECK(*r.cpu.quota_cores == Catch::Approx(1.5));
    CHECK_FALSE(r.cpu.effective_cores.has_value());
    CHECK_FALSE(r.budget.max_cpu_slots.has_value());
  }
}

// ---- 3. RAM ---------------------------------------------------------------------------------------------------------------

TEST_CASE("meminfo kB values convert exactly and fail closed", "[host][memory]") {
  auto ok = parse_meminfo(meminfo(8 * 1024 * 1024, 6 * 1024 * 1024));
  CHECK(ok.total_bytes == 8 * kGiB);
  CHECK(ok.available_bytes == 6 * kGiB);
  CHECK(ok.warnings.empty());
  CHECK(parse_meminfo("MemTotal: 1 kB\nMemAvailable: 0 kB\n").available_bytes == 0);  // observed zero stays zero

  auto dup = parse_meminfo("MemTotal: 1024 kB\nMemTotal: 2048 kB\nMemAvailable: 512 kB\n");
  CHECK_FALSE(dup.total_bytes.has_value());
  CHECK(dup.available_bytes == 512 * 1024);
  CHECK_FALSE(dup.warnings.empty());

  auto missing = parse_meminfo("MemTotal: 1024 kB\n");
  CHECK(missing.total_bytes == 1024 * 1024);
  CHECK_FALSE(missing.available_bytes.has_value());
  CHECK_FALSE(missing.warnings.empty());

  for (const char* bad : {"MemTotal: abc kB\nMemAvailable: 1 kB\n", "MemTotal: 5\nMemAvailable: 1 kB\n", "MemTotal: 5 MB\nMemAvailable: 1 kB\n",
                          "MemTotal: -5 kB\nMemAvailable: 1 kB\n", "MemTotal: 5 kB junk\nMemAvailable: 1 kB\n", "MemTotal: 1e3 kB\nMemAvailable: 1 kB\n"}) {
    INFO(bad);
    auto m = parse_meminfo(bad);
    CHECK_FALSE(m.total_bytes.has_value());
    CHECK_FALSE(m.warnings.empty());
  }
  // 2^54 kB * 1024 = 2^64 overflows; 2^54-1 kB does not.
  auto over = parse_meminfo("MemTotal: 18014398509481984 kB\nMemAvailable: 18014398509481983 kB\n");
  CHECK_FALSE(over.total_bytes.has_value());
  CHECK(over.available_bytes == 18014398509481983ull * 1024);
  // a key that only has MemAvailable as a suffix is not MemAvailable
  CHECK_FALSE(parse_meminfo("MemTotal: 1 kB\nXMemAvailable: 1 kB\n").available_bytes.has_value());
}

TEST_CASE("inconsistent host figures are capped at the total with a warning", "[host][memory]") {
  Tree t;
  t.dir(kCg);
  HostInputs in = inputs(t, "/");
  in.meminfo = meminfo(1024 * 1024, 2 * 1024 * 1024);  // available 2 GiB > total 1 GiB
  Report r = evaluate(in, {});
  CHECK(r.memory.host_total_bytes == kGiB);
  CHECK(r.memory.host_available_bytes == kGiB);
  CHECK_FALSE(r.warnings.empty());
}

TEST_CASE("memory headroom is the tightest max - current of each visible group, not a difference of limits", "[host][memory][cgroup]") {
  Tree t;
  cgroup_files(t, "/p", "max 100000\n", "4294967296\n", "3221225472\n");        // parent max 4 GiB, current 3 GiB (includes the leaf)
  cgroup_files(t, "/p/leaf", "max 100000\n", "2147483648\n", "536870912\n");     // leaf max 2 GiB, current 512 MiB
  Reserves res{256 * kMiB, 1024 * kMiB};
  Report r = evaluate(inputs(t, "/p/leaf"), res);
  CHECK(r.memory.host_total_bytes == 8 * kGiB);
  CHECK(r.memory.host_available_bytes == 6 * kGiB);
  CHECK(r.memory.cgroup_status == LimitStatus::kLimited);
  CHECK(r.memory.cgroup_limit_bytes == 2 * kGiB);
  CHECK(r.memory.cgroup_headroom_bytes == 1 * kGiB);  // parent: 4-3 = 1; leaf: 2-0.5 = 1.5
  CHECK(r.memory.effective_available_bytes == 1 * kGiB);
  CHECK(r.budget.reserve_memory_bytes == 256 * kMiB);
  CHECK(r.budget.max_memory_bytes == 768 * kMiB);
}

TEST_CASE("memory: host availability can be the tightest bound; over-limit groups saturate at zero", "[host][memory][cgroup]") {
  SECTION("host available below every headroom") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "17179869184\n", "0\n");
    HostInputs in = inputs(t, "/p");
    Report r = evaluate(in, {});
    CHECK(r.memory.cgroup_headroom_bytes == 16 * kGiB);
    CHECK(r.memory.effective_available_bytes == 6 * kGiB);
  }
  SECTION("current above max") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "1073741824\n", "2147483648\n");
    Report r = evaluate(inputs(t, "/p"), {});
    CHECK(r.memory.cgroup_limit_bytes == kGiB);
    CHECK(r.memory.cgroup_headroom_bytes == 0);
    CHECK(r.memory.effective_available_bytes == 0);
    CHECK(r.budget.max_memory_bytes == 0);
  }
  SECTION("finite zero limit is valid") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "0\n", "0\n");
    Report r = evaluate(inputs(t, "/p"), {});
    CHECK(r.memory.cgroup_status == LimitStatus::kLimited);
    CHECK(r.memory.cgroup_limit_bytes == 0);
    CHECK(r.memory.effective_available_bytes == 0);
    apply_request(r, Request{std::nullopt, 1, std::nullopt});
    CHECK_FALSE(r.ok);  // a request against a zero maximum fails
  }
  SECTION("reserve larger than availability saturates the budget at zero") {
    Tree t;
    t.dir(kCg);
    Report r = evaluate(inputs(t, "/"), Reserves{7 * kGiB, 0});
    CHECK(r.budget.max_memory_bytes == 0);
    CHECK(r.budget.max_storage_bytes == 10 * kGiB);
  }
}

TEST_CASE("unlimited cgroups leave numeric cgroup fields null and use host availability", "[host][memory][cgroup]") {
  Tree t;
  cgroup_files(t, "/p", "max 100000\n", "max\n");
  cgroup_files(t, "/p/leaf", "max 100000\n", "max\n");
  Report r = evaluate(inputs(t, "/p/leaf"), {});
  CHECK(r.memory.cgroup_status == LimitStatus::kUnlimited);
  CHECK_FALSE(r.memory.cgroup_limit_bytes.has_value());
  CHECK_FALSE(r.memory.cgroup_headroom_bytes.has_value());
  CHECK(r.memory.effective_available_bytes == 6 * kGiB);
  CHECK(r.budget.max_memory_bytes == 6 * kGiB - 1024 * kMiB);
}

TEST_CASE("memory limit parsers reject signs, junk and overflow", "[host][memory]") {
  CHECK(parse_memory_max("max\n")->unlimited);
  CHECK(parse_memory_max("0\n")->bytes == 0);
  CHECK(parse_memory_max("4096")->bytes == 4096);
  CHECK(parse_memory_max("18446744073709551615\n")->bytes == kMax64);
  for (const char* bad : {"", "\n", "-1\n", "+1\n", "12junk\n", "1 2\n", "18446744073709551616\n", "MAX\n", "max max\n", "0x10\n", " 5\n", "5\n\n"}) {
    INFO(bad);
    CHECK_FALSE(parse_memory_max(bad).has_value());
  }
  CHECK(parse_uint_file("123\n") == 123);
  CHECK(parse_uint_file("0") == 0);
  for (const char* bad : {"", "\n", "-1\n", "+1\n", "5x\n", "max\n", "18446744073709551616\n", "1\n2\n"}) {
    INFO(bad);
    CHECK_FALSE(parse_uint_file(bad).has_value());
  }
}

TEST_CASE("malformed memory observations make cgroup, effective and budget unknown, never a partial minimum", "[host][memory][cgroup]") {
  auto expect_unknown = [](const Report& r) {
    CHECK(r.memory.cgroup_status == LimitStatus::kUnknown);
    CHECK_FALSE(r.memory.cgroup_limit_bytes.has_value());
    CHECK_FALSE(r.memory.cgroup_headroom_bytes.has_value());
    CHECK_FALSE(r.memory.effective_available_bytes.has_value());
    CHECK_FALSE(r.budget.max_memory_bytes.has_value());
    CHECK(r.memory.host_available_bytes == 6 * kGiB);  // the host observation itself stays known
    CHECK_FALSE(r.warnings.empty());
  };
  SECTION("finite max without memory.current") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "1073741824\n");
    expect_unknown(evaluate(inputs(t, "/p"), {}));
  }
  SECTION("malformed memory.current") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "1073741824\n", "-5\n");
    expect_unknown(evaluate(inputs(t, "/p"), {}));
  }
  SECTION("known tight leaf with unreadable parent") {
    Tree t;
    t.dir(kCg + "/p/memory.max");  // directory instead of file: read error
    cgroup_files(t, "/p/leaf", "max 100000\n", "1073741824\n", "0\n");
    expect_unknown(evaluate(inputs(t, "/p/leaf"), {}));
  }
  SECTION("missing memory.max on a non-root group") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n");
    expect_unknown(evaluate(inputs(t, "/p"), {}));
  }
  SECTION("overflowing memory.max") {
    Tree t;
    cgroup_files(t, "/p", "max 100000\n", "18446744073709551616\n", "0\n");
    expect_unknown(evaluate(inputs(t, "/p"), {}));
  }
  SECTION("unresolved hierarchy is not an optimistic host-only budget") {
    Tree t;
    HostInputs in = inputs(t, "/");
    in.self_cgroup = "1:memory:/x\n";
    expect_unknown(evaluate(in, {}));
  }
  SECTION("unreadable /proc/self/cgroup") {
    Tree t;
    HostInputs in = inputs(t, "/");
    in.self_cgroup = std::nullopt;
    in.cgroup_error = "cannot read";
    expect_unknown(evaluate(in, {}));
  }
}

TEST_CASE("unknown host memory propagates to effective and budget", "[host][memory]") {
  Tree t;
  t.dir(kCg);
  HostInputs in = inputs(t, "/");
  in.meminfo = std::nullopt;
  in.meminfo_error = "unreadable";
  Report r = evaluate(in, {});
  CHECK(r.memory.cgroup_status == LimitStatus::kUnlimited);
  CHECK_FALSE(r.memory.host_total_bytes.has_value());
  CHECK_FALSE(r.memory.host_available_bytes.has_value());
  CHECK_FALSE(r.memory.effective_available_bytes.has_value());
  CHECK_FALSE(r.budget.max_memory_bytes.has_value());
  CHECK_FALSE(r.warnings.empty());

  Tree t2;
  cgroup_files(t2, "/p", "max 100000\n", "1073741824\n", "0\n");
  HostInputs in2 = inputs(t2, "/p");
  in2.meminfo = std::string("MemTotal: abc kB\nMemAvailable: abc kB\n");
  Report r2 = evaluate(in2, {});
  CHECK(r2.memory.cgroup_headroom_bytes == kGiB);          // cgroup value is still reported
  CHECK_FALSE(r2.memory.effective_available_bytes.has_value());  // but the effective minimum is not (host unknown)
}

// ---- 4. cgroup2 resolver --------------------------------------------------------------------------------------------------

TEST_CASE("resolver walks the leaf through the visible mount root inclusive", "[host][cgroup][resolver]") {
  Tree t;
  t.dir(kCg);  // the visible root must exist to be positively identified as the global root
  auto h = resolve(t, "/a/b/c");
  REQUIRE(h.resolved);
  REQUIRE(h.groups.size() == 4);
  CHECK(h.groups[0].path == "/sys/fs/cgroup/a/b/c");
  CHECK(h.groups[1].path == "/sys/fs/cgroup/a/b");
  CHECK(h.groups[2].path == "/sys/fs/cgroup/a");
  CHECK(h.groups[3].path == "/sys/fs/cgroup");
  CHECK(h.groups[3].visible_root);
  CHECK_FALSE(h.groups[0].visible_root);
  CHECK(h.groups[3].global_root);
  CHECK_FALSE(h.groups[2].global_root);

  auto root = resolve(t, "/");
  REQUIRE(root.resolved);
  REQUIRE(root.groups.size() == 1);
  CHECK(root.groups[0].global_root);
}

TEST_CASE("resolver component-boundary matching: /foo never matches /foobar", "[host][cgroup][resolver]") {
  Tree t;
  auto bad = resolve(t, "/foobar/x", mount_line("/foo", "/sys/fs/cgroup"));
  CHECK_FALSE(bad.resolved);
  CHECK_FALSE(bad.error.empty());
  auto exact = resolve(t, "/foo", mount_line("/foo", "/sys/fs/cgroup"));
  REQUIRE(exact.resolved);
  CHECK(exact.groups.size() == 1);
  auto below = resolve(t, "/foo/bar/baz", mount_line("/foo", "/mnt/cg"));
  REQUIRE(below.resolved);
  REQUIRE(below.groups.size() == 3);
  CHECK(below.groups[0].path == "/mnt/cg/bar/baz");
  CHECK(below.groups[2].path == "/mnt/cg");
  CHECK_FALSE(below.groups[2].global_root);  // namespace mount root other than "/"
}

TEST_CASE("resolver selects the longest mount root and rejects ambiguity", "[host][cgroup][resolver]") {
  Tree t;
  std::string two = mount_line("/", "/sys/fs/cgroup", "cgroup2", 36) + mount_line("/foo", "/mnt/x", "cgroup2", 40);
  auto h = resolve(t, "/foo/a", two);
  REQUIRE(h.resolved);
  CHECK(h.groups[0].path == "/mnt/x/a");
  auto other = resolve(t, "/bar/a", two);
  REQUIRE(other.resolved);
  CHECK(other.groups[0].path == "/sys/fs/cgroup/bar/a");

  std::string tie = mount_line("/foo", "/mnt/x", "cgroup2", 40) + mount_line("/foo", "/mnt/y", "cgroup2", 41);
  auto amb = resolve(t, "/foo/a", tie);
  CHECK_FALSE(amb.resolved);
  CHECK(amb.error.find("ambiguous") != std::string::npos);

  // a tie that cannot apply (different roots, only one matching) is fine; identical duplicate records are not ambiguous
  std::string dup = mount_line("/", "/sys/fs/cgroup", "cgroup2", 36) + mount_line("/", "/sys/fs/cgroup", "cgroup2", 37);
  CHECK(resolve(t, "/a", dup).resolved);

  // non-cgroup2 mounts are ignored even when more specific
  std::string v1 = mount_line("/", "/sys/fs/cgroup") + mount_line("/foo", "/mnt/v1", "cgroup");
  auto only2 = resolve(t, "/foo/a", v1);
  REQUIRE(only2.resolved);
  CHECK(only2.groups[0].path == "/sys/fs/cgroup/foo/a");
}

TEST_CASE("resolver decodes escaped mount points and rejects malformed paths", "[host][cgroup][resolver]") {
  Tree t;
  auto sp = resolve(t, "/a", mount_line("/", "/sys/fs/cg\\040roup\\011x\\134y"));
  REQUIRE(sp.resolved);
  CHECK(sp.groups[0].path == "/sys/fs/cg roup\tx\\y/a");
  auto root_sp = resolve(t, "/ns", mount_line("/ns", "/mnt/with\\040space"));
  REQUIRE(root_sp.resolved);
  CHECK(root_sp.groups[0].path == "/mnt/with space");

  for (const char* bad_membership : {"/a/../b", "/a/./b", "//a", "/a//b", "/a/", "a/b", "", "/..", "/a/b/.."}) {
    INFO(bad_membership);
    CHECK_FALSE(resolve(t, bad_membership).resolved);
  }
  auto nul = resolve_cgroup2(std::string("0::/a\0b\n", 8), kGlobalMount, t.root);
  CHECK_FALSE(nul.resolved);
  // traversal and bad syntax hidden behind octal escapes in the mount record
  CHECK_FALSE(resolve(t, "/a", mount_line("/", "/sys/fs/cgroup/\\056\\056")).resolved);
  CHECK_FALSE(resolve(t, "/a", mount_line("/", "/sys/fs/cgroup/a\\000b")).resolved);
  CHECK_FALSE(resolve(t, "/a", mount_line("/", "relative/path")).resolved);
  CHECK_FALSE(resolve(t, "/a", mount_line("/..", "/sys/fs/cgroup")).resolved);
  CHECK_FALSE(resolve(t, "/a", mount_line("/", "/sys/fs/cgroup/")).resolved);
  CHECK_FALSE(resolve(t, "/a", mount_line("/", "/sys/fs/cg\\04")).resolved);  // truncated escape
}

TEST_CASE("resolver fails closed on missing, v1-only, duplicate and malformed records", "[host][cgroup][resolver]") {
  Tree t;
  CHECK_FALSE(resolve_cgroup2("1:cpu:/a\n", kGlobalMount, t.root).resolved);                    // v1 only
  CHECK_FALSE(resolve_cgroup2("", kGlobalMount, t.root).resolved);
  CHECK_FALSE(resolve_cgroup2("0::/a\n0::/b\n", kGlobalMount, t.root).resolved);                 // two unified lines
  CHECK_FALSE(resolve_cgroup2("garbage\n", kGlobalMount, t.root).resolved);
  CHECK_FALSE(resolve_cgroup2("0:/a\n", kGlobalMount, t.root).resolved);
  CHECK_FALSE(resolve_cgroup2("0::/a\n", "", t.root).resolved);                                  // no mount at all
  CHECK_FALSE(resolve_cgroup2("0::/a\n", mount_line("/", "/sys/fs/cgroup", "cgroup"), t.root).resolved);  // v1 mount only
  CHECK_FALSE(resolve_cgroup2("0::/a\n", "this is not a mountinfo line\n", t.root).resolved);
  CHECK_FALSE(resolve_cgroup2("0::/a\n", "36 28 0:30 / /sys/fs/cgroup rw cgroup2 cgroup2 rw\n", t.root).resolved);  // no '-' separator
  CHECK_FALSE(resolve_cgroup2("0::/a\n", "36 28 0:30 / /sys/fs/cgroup rw - cgroup2\n", t.root).resolved);           // truncated after separator
  CHECK_FALSE(resolve_cgroup2("0::/a\n", kGlobalMount + "short line\n", t.root).resolved);                          // one bad record spoils it
  // hybrid layout: v1 controllers plus a unified entry is fine
  auto hybrid = resolve_cgroup2("12:cpu:/x\n0::/a\n", kGlobalMount, t.root);
  REQUIRE(hybrid.resolved);
  CHECK(hybrid.groups[0].path == "/sys/fs/cgroup/a");
  // a colon inside the group path is part of the path
  auto colon = resolve_cgroup2("0::/a:b\n", kGlobalMount, t.root);
  REQUIRE(colon.resolved);
  CHECK(colon.groups[0].path == "/sys/fs/cgroup/a:b");
  // optional fields before the separator (several tags) are handled
  auto tags = resolve_cgroup2("0::/a\n", "36 28 0:30 / /sys/fs/cgroup rw shared:8 master:2 - cgroup2 cgroup2 rw\n", t.root);
  CHECK(tags.resolved);
  auto no_tags = resolve_cgroup2("0::/a\n", "36 28 0:30 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n", t.root);
  CHECK(no_tags.resolved);
}

TEST_CASE("namespace mount root: absent root files are unknown, not unlimited", "[host][cgroup][resolver]") {
  Tree t;
  t.dir(kCg + "/leaf");
  t.put(kCg + "/leaf/cpu.max", "max 100000\n");
  t.put(kCg + "/leaf/memory.max", "max\n");
  // mount root "/ns" at /sys/fs/cgroup; the visible root directory has no limit files
  HostInputs in = inputs(t, "/ns/leaf", mount_line("/ns", "/sys/fs/cgroup"));
  Report r = evaluate(in, {});
  CHECK(r.cpu.quota_status == LimitStatus::kUnknown);
  CHECK_FALSE(r.cpu.effective_cores.has_value());
  CHECK(r.memory.cgroup_status == LimitStatus::kUnknown);
  CHECK_FALSE(r.memory.effective_available_bytes.has_value());

  // once the namespace root exposes its files and they are unlimited, the result is unlimited
  t.put(kCg + "/cpu.max", "max 100000\n");
  t.put(kCg + "/memory.max", "max\n");
  Report ok = evaluate(in, {});
  CHECK(ok.cpu.quota_status == LimitStatus::kUnlimited);
  CHECK(ok.memory.cgroup_status == LimitStatus::kUnlimited);
}

TEST_CASE("a root with cgroup.events is a namespace root, so missing files there are unknown", "[host][cgroup][resolver]") {
  Tree t;
  t.dir(kCg);
  t.put(kCg + "/cgroup.events", "populated 1\nfrozen 0\n");  // never present in the real root cgroup
  auto h = resolve(t, "/");
  REQUIRE(h.resolved);
  CHECK_FALSE(h.groups[0].global_root);
  Report r = evaluate(inputs(t, "/"), {});
  CHECK(r.cpu.quota_status == LimitStatus::kUnknown);
  CHECK(r.memory.cgroup_status == LimitStatus::kUnknown);
}

TEST_CASE("cgroup files are read without leaving the selected mount", "[host][cgroup][resolver]") {
  Tree t;
  Tree outside;
  outside.put("cpu.max", "10000 100000\n");
  outside.put("memory.max", "1\n");
  outside.put("memory.current", "0\n");
  cgroup_files(t, "/a", "max 100000\n", "max\n");

  SECTION("regular file read") {
    auto h = resolve(t, "/a");
    auto r = read_cgroup_file(h, 0, "cpu.max");
    CHECK(r.status == FileRead::Status::kOk);
    CHECK(r.text == "max 100000\n");
    CHECK(read_cgroup_file(h, 0, "does-not-exist").status == FileRead::Status::kMissing);
    t.dir(kCg + "/a/dir.max");
    CHECK(read_cgroup_file(h, 0, "dir.max").status == FileRead::Status::kError);
    CHECK(read_cgroup_file(h, 7, "cpu.max").status == FileRead::Status::kError);  // no such group index
  }
  SECTION("file names may not contain separators or traverse") {
    auto h = resolve(t, "/a");
    CHECK(read_cgroup_file(h, 0, "../a/cpu.max").status == FileRead::Status::kError);
    CHECK(read_cgroup_file(h, 0, "/etc/passwd").status == FileRead::Status::kError);
    CHECK(read_cgroup_file(h, 0, "").status == FileRead::Status::kError);
    CHECK(read_cgroup_file(h, 0, "..").status == FileRead::Status::kError);
  }
  SECTION("a limit file symlinked outside is not followed") {
    fs::remove(t.root / kCg / "a/cpu.max");
    fs::create_symlink(outside.root / "cpu.max", t.root / kCg / "a/cpu.max");
    auto h = resolve(t, "/a");
    CHECK(read_cgroup_file(h, 0, "cpu.max").status == FileRead::Status::kError);
    Report r = evaluate(inputs(t, "/a"), {});
    CHECK(r.cpu.quota_status == LimitStatus::kUnknown);  // never the 0.1 CPU limit of the outside file
  }
  SECTION("a group directory symlinked outside is not followed") {
    fs::remove_all(t.root / kCg / "a");
    fs::create_directory_symlink(outside.root, t.root / kCg / "a");
    Report r = evaluate(inputs(t, "/a"), {});
    CHECK(r.cpu.quota_status == LimitStatus::kUnknown);
    CHECK(r.memory.cgroup_status == LimitStatus::kUnknown);
    CHECK_FALSE(r.memory.effective_available_bytes.has_value());
  }
  SECTION("a symlinked prefix of the mount point inside the fixture root is not followed") {
    Tree jail;
    fs::create_directories(outside.root / "fs/cgroup/a");
    std::ofstream(outside.root / "fs/cgroup/a/cpu.max") << "10000 100000\n";
    fs::create_directory_symlink(outside.root, jail.root / "sys");
    Report r = evaluate(inputs(jail, "/a"), {});
    CHECK(r.cpu.quota_status == LimitStatus::kUnknown);
  }
}

// ---- 5. Disk ---------------------------------------------------------------------------------------------------------------

TEST_CASE("storage uses f_bavail, not free blocks reserved for root", "[host][storage]") {
  FsStat st{4096, 100 * kGiB / 4096, 20 * kGiB / 4096, 10 * kGiB / 4096};
  auto n = evaluate_storage(st);
  CHECK(n.error.empty());
  CHECK(n.total_bytes == 100 * kGiB);
  CHECK(n.available_bytes == 10 * kGiB);

  Tree t;
  t.dir(kCg);
  HostInputs in = inputs(t, "/");
  in.fs_stat = st;
  Report r = evaluate(in, Reserves{1024 * kMiB, 2 * kGiB});
  CHECK(r.storage.path == "/fixture/disk");
  CHECK(r.storage.total_bytes == 100 * kGiB);
  CHECK(r.storage.available_bytes == 10 * kGiB);
  CHECK(r.budget.max_storage_bytes == 8 * kGiB);

  in.fs_stat = FsStat{512, 10, 10, 0};  // nothing available: valid zero, not unknown
  Report zero = evaluate(in, {});
  CHECK(zero.storage.available_bytes == 0);
  CHECK(zero.budget.max_storage_bytes == 0);
}

TEST_CASE("implausible or overflowing statvfs values fail closed", "[host][storage]") {
  CHECK_FALSE(evaluate_storage(FsStat{0, 10, 5, 5}).error.empty());                 // zero fragment size
  CHECK_FALSE(evaluate_storage(FsStat{4096, 10, 5, 11}).error.empty());             // bavail > blocks
  CHECK_FALSE(evaluate_storage(FsStat{4096, 10, 11, 5}).error.empty());             // bfree > blocks
  CHECK_FALSE(evaluate_storage(FsStat{4096, kMax64 / 4096 + 1, 0, 0}).error.empty());  // blocks*frsize overflows
  CHECK_FALSE(evaluate_storage(FsStat{kMax64, 2, 0, 0}).error.empty());
  auto edge = evaluate_storage(FsStat{4096, kMax64 / 4096, 0, 1});  // largest representable
  CHECK(edge.error.empty());
  CHECK(edge.total_bytes == (kMax64 / 4096) * 4096);
  for (auto bad : {FsStat{0, 10, 5, 5}, FsStat{4096, 10, 5, 11}}) {
    auto n = evaluate_storage(bad);
    CHECK_FALSE(n.total_bytes.has_value());
    CHECK_FALSE(n.available_bytes.has_value());
  }

  Tree t;
  t.dir(kCg);
  HostInputs in = inputs(t, "/");
  in.fs_stat = FsStat{4096, 10, 5, 11};
  Report r = evaluate(in, {});
  CHECK_FALSE(r.storage.total_bytes.has_value());
  CHECK_FALSE(r.budget.max_storage_bytes.has_value());
  CHECK_FALSE(r.warnings.empty());
  in.fs_stat = std::nullopt;
  in.storage_error = "statvfs failed";
  Report f = evaluate(in, {});
  CHECK_FALSE(f.storage.available_bytes.has_value());
  CHECK_FALSE(f.budget.max_storage_bytes.has_value());
}

TEST_CASE("observe_live reports the resolved directory and statvfs numbers without writing", "[host][storage]") {
  Tree t;
  t.dir("real/sub");
  t.put("real/file.txt", "payload");
  fs::create_directory_symlink(t.root / "real", t.root / "link");
  auto snapshot = [&] {
    std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> m;
    for (auto& e : fs::recursive_directory_iterator(t.root, fs::directory_options::none))
      m[e.path().string()] = {e.is_regular_file() ? e.file_size() : 0, fs::last_write_time(e.path())};
    return m;
  };
  auto before = snapshot();
  HostInputs in = observe_live(t.root / "link");
  CHECK(snapshot() == before);
  CHECK(in.storage_path == fs::canonical(t.root / "real"));
  REQUIRE(in.fs_stat.has_value());
  struct statvfs sv;
  REQUIRE(statvfs(in.storage_path.c_str(), &sv) == 0);
  CHECK(in.fs_stat->frsize == sv.f_frsize);
  CHECK(in.fs_stat->blocks == sv.f_blocks);
  REQUIRE(in.affinity_cpus.has_value());
  CHECK(in.affinity_cpus->size() == own_affinity_count());
  REQUIRE(in.meminfo.has_value());
  CHECK(in.meminfo->find("MemTotal:") != std::string::npos);
  REQUIRE(in.self_cgroup.has_value());
  REQUIRE(in.mountinfo.has_value());
  CHECK(in.fs_root == fs::path("/"));
  CHECK(in.sys_root == fs::path("/sys"));
}

TEST_CASE("observe_live rejects missing, non-directory and inaccessible paths with io_error", "[host][storage]") {
  Tree t;
  t.put("file.txt", "x");
  t.dir("d");
  auto expect_io = [](const fs::path& p) {
    try {
      observe_live(p);
      FAIL("expected an io_error for " << p);
    } catch (const Error& e) {
      CHECK(e.code() == "io_error");
      CHECK(std::string(e.what()).size() > 0);
    }
  };
  expect_io(t.root / "missing");
  expect_io(t.root / "file.txt");
  expect_io(t.root / "file.txt" / "child");
  expect_io("");
  fs::create_symlink(t.root / "nowhere", t.root / "dangling");
  expect_io(t.root / "dangling");
  fs::create_symlink(t.root / "file.txt", t.root / "to-file");
  expect_io(t.root / "to-file");
  CHECK(fs::exists(t.root / "d"));
  CHECK_FALSE(fs::exists(t.root / "missing"));  // nothing was created
  if (geteuid() == 0) {
    WARN("running as root: permission-denied directory cases skipped (root bypasses mode bits)");
    return;
  }
  t.dir("noperm/inner");
  fs::permissions(t.root / "noperm", fs::perms::none);
  expect_io(t.root / "noperm");
  expect_io(t.root / "noperm" / "inner");
  fs::permissions(t.root / "noperm", fs::perms::owner_all);
  t.dir("noread");
  fs::permissions(t.root / "noread", fs::perms::owner_exec);  // searchable but not readable
  expect_io(t.root / "noread");
  fs::permissions(t.root / "noread", fs::perms::owner_all);
  t.dir("nosearch");
  fs::permissions(t.root / "nosearch", fs::perms::owner_read);  // readable but not searchable
  expect_io(t.root / "nosearch");
  fs::permissions(t.root / "nosearch", fs::perms::owner_all);
}

// ---- 6. Dry-run requests --------------------------------------------------------------------------------------------------

namespace {
Report budget_report() {
  Tree t;
  cgroup_files(t, "/p", "max 100000\n", "4294967296\n", "3221225472\n");  // headroom 1 GiB
  Report r = evaluate(inputs(t, "/p"), Reserves{256 * kMiB, 2 * kGiB});     // memory budget 768 MiB, storage budget 8 GiB, cpu 4
  return r;
}
}  // namespace

TEST_CASE("a request exactly at the budget passes; one more fails per quantity", "[host][request]") {
  Report base = budget_report();
  REQUIRE(base.budget.max_cpu_slots == 4);
  REQUIRE(base.budget.max_memory_bytes == 768 * kMiB);
  REQUIRE(base.budget.max_storage_bytes == 8 * kGiB);

  Report exact = base;
  apply_request(exact, Request{4, 768 * kMiB, 8 * kGiB});
  CHECK(exact.ok);
  CHECK(exact.request.valid);
  CHECK(exact.request.errors.empty());
  CHECK(exact.request.cpus == 4);
  CHECK(exact.request.memory_bytes == 768 * kMiB);

  Report over = base;
  apply_request(over, Request{5, 768 * kMiB + 1, 8 * kGiB + 1});
  CHECK_FALSE(over.ok);
  CHECK_FALSE(over.request.valid);
  CHECK(over.request.errors.size() == 3);  // every failing quantity gets its own reason
  std::string all;
  for (auto& e : over.request.errors) all += e + "|";
  CHECK(all.find("cpu") != std::string::npos);
  CHECK(all.find("memory") != std::string::npos);
  CHECK(all.find("storage") != std::string::npos);

  Report only_mem = base;
  apply_request(only_mem, Request{std::nullopt, 768 * kMiB + 1, std::nullopt});
  CHECK_FALSE(only_mem.ok);
  CHECK(only_mem.request.errors.size() == 1);
  CHECK(only_mem.request.errors[0].find("memory") != std::string::npos);
}

TEST_CASE("a request against an unknown maximum fails; no request with unknowns is still ok", "[host][request]") {
  Tree t;
  cgroup_files(t, "/p", "");  // unknown CPU and memory
  Report unknown = evaluate(inputs(t, "/p"), {});
  REQUIRE_FALSE(unknown.budget.max_cpu_slots.has_value());
  REQUIRE_FALSE(unknown.budget.max_memory_bytes.has_value());

  Report none = unknown;
  apply_request(none, Request{});
  CHECK(none.ok);
  CHECK(none.request.valid);
  CHECK_FALSE(none.warnings.empty());  // unknowns are reported, not hidden

  for (int which = 0; which < 2; which++) {
    Report r = unknown;
    Request q;
    if (which == 0) q.cpus = 1;
    else q.memory_bytes = 1;
    apply_request(r, q);
    INFO(which);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.request.valid);
    REQUIRE(r.request.errors.size() == 1);
    CHECK(r.request.errors[0].find("unknown") != std::string::npos);
  }
}

TEST_CASE("insufficient reserve saturates the budget at zero and rejects any request", "[host][request]") {
  Tree t;
  t.dir(kCg);
  Report r = evaluate(inputs(t, "/"), Reserves{100 * kGiB, 100 * kGiB});
  CHECK(r.budget.max_memory_bytes == 0);
  CHECK(r.budget.max_storage_bytes == 0);
  apply_request(r, Request{std::nullopt, 1, 1});
  CHECK_FALSE(r.ok);
  CHECK(r.request.errors.size() == 2);
}

// ---- JSON schema ----------------------------------------------------------------------------------------------------------

TEST_CASE("JSON schema v1 has the stable shape, nulls for unknowns and full uint64 range", "[host][json]") {
  Tree t;
  t.dir(kCg);
  Report r = evaluate(inputs(t, "/"), {});
  apply_request(r, Request{});
  json j = to_json(r);
  CHECK(j["schema_version"] == 1);
  CHECK(j["ok"] == true);
  std::set<std::string> top;
  for (auto& [k, v] : j.items()) top.insert(k);
  CHECK(top == std::set<std::string>{"schema_version", "ok", "cpu", "memory", "storage", "budget", "request", "warnings"});
  std::set<std::string> cpu;
  for (auto& [k, v] : j["cpu"].items()) cpu.insert(k);
  CHECK(cpu == std::set<std::string>{"affinity_logical_cpus", "physical_cores", "quota_cores", "quota_status", "effective_cores"});
  std::set<std::string> mem;
  for (auto& [k, v] : j["memory"].items()) mem.insert(k);
  CHECK(mem == std::set<std::string>{"host_total_bytes", "host_available_bytes", "cgroup_limit_bytes", "cgroup_headroom_bytes", "cgroup_status",
                                     "effective_available_bytes"});
  std::set<std::string> bud;
  for (auto& [k, v] : j["budget"].items()) bud.insert(k);
  CHECK(bud == std::set<std::string>{"reserve_memory_bytes", "reserve_storage_bytes", "max_cpu_slots", "max_memory_bytes", "max_storage_bytes"});
  std::set<std::string> req;
  for (auto& [k, v] : j["request"].items()) req.insert(k);
  CHECK(req == std::set<std::string>{"cpus", "memory_bytes", "storage_bytes", "valid", "errors"});
  CHECK(j["cpu"]["affinity_logical_cpus"] == 4);
  CHECK(j["cpu"]["physical_cores"].is_null());  // no topology in the fixture
  CHECK(j["cpu"]["quota_cores"].is_null());
  CHECK(j["cpu"]["quota_status"] == "unlimited");
  CHECK(j["cpu"]["effective_cores"].is_number_float());
  CHECK(j["cpu"]["effective_cores"].get<double>() == 4.0);
  CHECK(j["memory"]["cgroup_limit_bytes"].is_null());
  CHECK(j["memory"]["cgroup_status"] == "unlimited");
  CHECK(j["storage"]["path"] == "/fixture/disk");
  CHECK(j["budget"]["reserve_memory_bytes"] == 1073741824ull);
  CHECK(j["budget"]["max_cpu_slots"] == 4);
  CHECK(j["request"]["valid"] == true);
  CHECK(j["request"]["cpus"].is_null());
  CHECK(j["request"]["errors"].is_array());
  CHECK(j["warnings"].is_array());

  // largest values survive a dump/parse round trip as exact unsigned integers
  Report big;
  big.memory.host_total_bytes = kMax64;
  big.memory.host_available_bytes = kMax64;
  big.storage.total_bytes = kMax64;
  json bj = json::parse(to_json(big).dump());
  CHECK(bj["memory"]["host_total_bytes"].get<uint64_t>() == kMax64);
  CHECK(bj["storage"]["total_bytes"].get<uint64_t>() == kMax64);
}

TEST_CASE("non-finite doubles are serialized as null with a warning", "[host][json]") {
  Report r;
  r.cpu.effective_cores = std::numeric_limits<double>::infinity();
  r.cpu.quota_cores = std::nan("");
  json j = to_json(r);
  CHECK(j["cpu"]["effective_cores"].is_null());
  CHECK(j["cpu"]["quota_cores"].is_null());
  CHECK(j["warnings"].size() >= 1);
  CHECK(to_string(LimitStatus::kLimited) == std::string("limited"));
  CHECK(to_string(LimitStatus::kUnlimited) == std::string("unlimited"));
  CHECK(to_string(LimitStatus::kUnknown) == std::string("unknown"));
}

// ---- 7. CLI ---------------------------------------------------------------------------------------------------------------

namespace {
std::string tmpdir_for_cli(Tree& t) {
  t.dir("work");
  return (t.root / "work").string();
}
}  // namespace

TEST_CASE("coffee resources --json emits one schema-1 object matching independent observations", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  auto r = run({"resources", "--path", dir, "--json"});
  REQUIRE(r.code == 0);
  CHECK(r.err.empty());
  json j = parse_single(r.out);
  CHECK(j["schema_version"] == 1);
  CHECK(j["ok"] == true);
  CHECK(j["storage"]["path"] == fs::canonical(dir).string());
  struct statvfs sv;
  REQUIRE(statvfs(dir.c_str(), &sv) == 0);
  CHECK(j["storage"]["total_bytes"].get<uint64_t>() == static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize);
  uint64_t avail = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
  uint64_t reported = j["storage"]["available_bytes"].get<uint64_t>();
  CHECK((reported > avail ? reported - avail : avail - reported) < 64 * kMiB);  // dynamic counter, allow drift
  CHECK(j["cpu"]["affinity_logical_cpus"].get<uint64_t>() == own_affinity_count());
  CHECK(j["budget"]["reserve_memory_bytes"] == 1073741824ull);
  CHECK(j["budget"]["reserve_storage_bytes"] == 1073741824ull);
  CHECK(j["request"]["valid"] == true);
  CHECK(j["request"]["cpus"].is_null());
  CHECK(j["request"]["memory_bytes"].is_null());
  CHECK(j["request"]["storage_bytes"].is_null());
  CHECK(r.out.find(fs::canonical(dir).string()) != std::string::npos);
  char host[256] = {0};
  gethostname(host, sizeof(host) - 1);
  if (host[0] != 0) {  // no hostname in the output (the caller-supplied path is echoed on purpose, so remove it first)
    std::string without_path = r.out;
    for (size_t at; (at = without_path.find(fs::canonical(dir).string())) != std::string::npos;) without_path.erase(at, fs::canonical(dir).string().size());
    CHECK(without_path.find(host) == std::string::npos);
  }
  // the schema is an object tree whose non-null numbers are unsigned or non-negative finite
  for (auto section : {"cpu", "memory", "storage", "budget"})
    for (auto& [k, v] : j[section].items())
      if (v.is_number_float()) {
        CHECK(std::isfinite(v.get<double>()));
        CHECK(v.get<double>() >= 0);
      }
}

TEST_CASE("coffee resources defaults the path to '.', accepts relative and symlinked directories", "[host][cli]") {
  Tree t;
  t.dir("real/inner");
  fs::create_directory_symlink(t.root / "real", t.root / "link");
  struct Cwd {
    fs::path old = fs::current_path();
    ~Cwd() { fs::current_path(old); }
  } restore;
  fs::current_path(t.root / "real");
  auto dflt = run({"resources", "--json"});
  REQUIRE(dflt.code == 0);
  CHECK(parse_single(dflt.out)["storage"]["path"] == fs::canonical(t.root / "real").string());
  auto rel = run({"resources", "--path", "inner", "--json"});
  REQUIRE(rel.code == 0);
  CHECK(parse_single(rel.out)["storage"]["path"] == fs::canonical(t.root / "real/inner").string());
  auto sym = run({"resources", "--path=" + (t.root / "link").string(), "--json"});
  REQUIRE(sym.code == 0);
  CHECK(parse_single(sym.out)["storage"]["path"] == fs::canonical(t.root / "real").string());
}

TEST_CASE("coffee resources human output labels scope, unknowns and the dry-run nature", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  auto r = run({"resources", "--path", dir});
  REQUIRE(r.code == 0);
  CHECK(r.out.find("observation") != std::string::npos);
  CHECK(r.out.find("no resources were allocated") != std::string::npos);
  CHECK(r.out.find(fs::canonical(dir).string()) != std::string::npos);
  for (auto label : {"CPU", "Memory", "Storage", "Budget"}) CHECK(r.out.find(label) != std::string::npos);
  CHECK(r.out.find("{") == std::string::npos);  // not JSON
  CHECK(r.err.empty());

  auto rej = run({"resources", "--path", dir, "--memory-mb", "17592186044415"});
  CHECK(rej.code == 1);
  CHECK(rej.out.find("rejected") != std::string::npos);
  CHECK(rej.out.find("memory") != std::string::npos);
  CHECK_FALSE(rej.err.empty());
}

TEST_CASE("coffee resources reserves: zero is accepted, defaults are 1024 MiB", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  auto z = run({"resources", "--path", dir, "--json", "--reserve-memory-mb", "0", "--reserve-storage-mb=0"});
  REQUIRE(z.code == 0);
  json j = parse_single(z.out);
  CHECK(j["budget"]["reserve_memory_bytes"] == 0);
  CHECK(j["budget"]["reserve_storage_bytes"] == 0);
  if (j["memory"]["effective_available_bytes"].is_number())
    CHECK(j["budget"]["max_memory_bytes"] == j["memory"]["effective_available_bytes"]);
  if (j["storage"]["available_bytes"].is_number()) CHECK(j["budget"]["max_storage_bytes"] == j["storage"]["available_bytes"]);
  auto m = run({"resources", "--path", dir, "--json", "--reserve-memory-mb", "5", "--reserve-storage-mb", "7"});
  json k = parse_single(m.out);
  CHECK(k["budget"]["reserve_memory_bytes"] == 5 * kMiB);
  CHECK(k["budget"]["reserve_storage_bytes"] == 7 * kMiB);
  if (k["storage"]["available_bytes"].is_number()) {
    uint64_t av = k["storage"]["available_bytes"].get<uint64_t>();
    CHECK(k["budget"]["max_storage_bytes"].get<uint64_t>() == (av > 7 * kMiB ? av - 7 * kMiB : 0));
  }
}

TEST_CASE("coffee resources dry-run request: accepted within budget, rejected beyond with full schema", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  // Requests derived from the command's own reported budget: exactly the budget passes, budget+1 fails.
  auto base = parse_single(run({"resources", "--path", dir, "--json", "--reserve-storage-mb", "0"}).out);
  if (base["budget"]["max_storage_bytes"].is_number()) {
    uint64_t mib = base["budget"]["max_storage_bytes"].get<uint64_t>() / kMiB;
    if (mib >= 1 && mib < (1ull << 40)) {
      auto ok = run({"resources", "--path", dir, "--json", "--reserve-storage-mb", "0", "--storage-mb", std::to_string(mib)});
      CHECK(ok.code == 0);
      CHECK(parse_single(ok.out)["request"]["storage_bytes"].get<uint64_t>() == mib * kMiB);
      CHECK(parse_single(ok.out)["request"]["valid"] == true);
    }
  }
  auto bad = run({"resources", "--path", dir, "--json", "--cpus", "4294967296", "--memory-mb", "17592186044415", "--storage-mb", "17592186044415"});
  CHECK(bad.code == 1);
  CHECK(bad.err.empty());
  json j = parse_single(bad.out);
  CHECK(j["ok"] == false);
  CHECK(j["request"]["valid"] == false);
  CHECK(j["request"]["errors"].size() == 3);
  CHECK(j["request"]["cpus"] == 4294967296ull);
  CHECK(j["request"]["memory_bytes"] == 17592186044415ull * kMiB);
  CHECK(j["cpu"].is_object());  // full schema, not the error object
  CHECK_FALSE(j.contains("error"));
  // a one-CPU request agrees with the reported CPU budget
  auto base_cpu = parse_single(run({"resources", "--path", dir, "--json"}).out);
  auto one = run({"resources", "--path", dir, "--json", "--cpus", "1"});
  bool fits = base_cpu["budget"]["max_cpu_slots"].is_number() && base_cpu["budget"]["max_cpu_slots"].get<uint64_t>() >= 1;
  CHECK(one.code == (fits ? 0 : 1));
  CHECK(parse_single(one.out)["ok"] == fits);
}

TEST_CASE("coffee resources help overrides everything and exits 0", "[host][cli]") {
  for (auto args : std::vector<std::vector<std::string>>{{"resources", "--help"},
                                                         {"resources", "-h"},
                                                         {"resources", "--bogus", "--help"},
                                                         {"resources", "--json", "--help"},
                                                         {"resources", "--cpus", "abc", "-h"},
                                                         {"resources", "extra", "--help"}}) {
    auto h = run(args);
    INFO(args.back());
    CHECK(h.code == 0);
    CHECK(h.out.find("Usage: coffee resources") != std::string::npos);
    for (auto opt : {"--path", "--json", "--cpus", "--memory-mb", "--storage-mb", "--reserve-memory-mb", "--reserve-storage-mb"})
      CHECK(h.out.find(opt) != std::string::npos);
    CHECK(h.err.empty());
  }
  CHECK(run({"--help"}).out.find("resources") != std::string::npos);
}

TEST_CASE("coffee resources usage errors exit 2 (human: stderr only)", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  struct Case {
    std::vector<std::string> args;
    std::string needle;
  };
  std::vector<Case> cases = {
      {{"resources", "--bogus"}, "--bogus"},
      {{"resources", "-x"}, "-x"},
      {{"resources", "extra"}, "extra"},
      {{"resources", "--path", dir, "extra"}, "extra"},
      {{"resources", "--path"}, "--path"},
      {{"resources", "--cpus"}, "--cpus"},
      {{"resources", "--path", ""}, "--path"},
      {{"resources", "--path", dir, "--cpus", "0"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "-1"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "1x"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", ""}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", " 1"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "+1"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "1.5"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "0x10"}, "--cpus"},
      {{"resources", "--path", dir, "--cpus", "99999999999999999999"}, "--cpus"},
      {{"resources", "--path", dir, "--memory-mb", "0"}, "--memory-mb"},
      {{"resources", "--path", dir, "--memory-mb", "-5"}, "--memory-mb"},
      {{"resources", "--path", dir, "--memory-mb", "17592186044416"}, "--memory-mb"},  // x 1048576 would overflow uint64
      {{"resources", "--path", dir, "--storage-mb", "17592186044416"}, "--storage-mb"},
      {{"resources", "--path", dir, "--storage-mb", "18446744073709551616"}, "--storage-mb"},
      {{"resources", "--path", dir, "--reserve-memory-mb", "-1"}, "--reserve-memory-mb"},
      {{"resources", "--path", dir, "--reserve-storage-mb", "17592186044416"}, "--reserve-storage-mb"},
      {{"resources", "--path", dir, "--reserve-storage-mb", "abc"}, "--reserve-storage-mb"},
      {{"resources", "--path", dir, "--json=1"}, "--json"},
  };
  for (auto& c : cases) {
    INFO(c.needle << " :: " << (c.args.size() > 2 ? c.args[c.args.size() - 2] + " " + c.args.back() : c.args.back()));
    auto r = run(c.args);
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.find(c.needle) != std::string::npos);
  }
  // the largest value that does not overflow after x1048576 is accepted as a number (and then judged against the budget)
  auto edge = run({"resources", "--path", dir, "--json", "--memory-mb", "17592186044415"});
  CHECK((edge.code == 1));
  CHECK(parse_single(edge.out)["request"]["memory_bytes"] == 17592186044415ull * kMiB);
}

TEST_CASE("coffee resources --json errors are a single stdout object with the exact status", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  t.put("afile", "x");
  struct Case {
    std::vector<std::string> args;
    int code;
    std::string error_code;
  };
  std::vector<Case> cases = {
      {{"resources", "--json", "--bogus"}, 2, "invalid_argument"},
      {{"resources", "--bogus", "--json"}, 2, "invalid_argument"},
      {{"resources", "--json", "extra"}, 2, "invalid_argument"},
      {{"resources", "--json", "--path"}, 2, "invalid_argument"},
      {{"resources", "--json", "--path", ""}, 2, "invalid_argument"},
      {{"resources", "--json", "--path", dir, "--cpus", "-3"}, 2, "invalid_argument"},
      {{"resources", "--json", "--path", dir, "--memory-mb", "17592186044416"}, 2, "invalid_argument"},
      {{"resources", "--json", "--path", (t.root / "missing").string()}, 1, "io_error"},
      {{"resources", "--json", "--path", (t.root / "afile").string()}, 1, "io_error"},
      {{"resources", "--path", (t.root / "afile" / "x").string(), "--json"}, 1, "io_error"},
  };
  for (auto& c : cases) {
    auto r = run(c.args);
    INFO(c.args.back());
    CHECK(r.code == c.code);
    CHECK(r.err.empty());
    json j = parse_single(r.out);
    CHECK(j.size() == 3);
    CHECK(j["schema_version"] == 1);
    CHECK(j["ok"] == false);
    REQUIRE(j["error"].is_object());
    CHECK(j["error"].size() == 2);
    CHECK(j["error"]["code"] == c.error_code);
    CHECK(j["error"]["message"].is_string());
    CHECK_FALSE(j["error"]["message"].get<std::string>().empty());
  }
}

TEST_CASE("coffee resources human-mode io errors exit 1 with a diagnostic on stderr only", "[host][cli]") {
  Tree t;
  t.put("afile", "x");
  for (auto p : {t.root / "missing", t.root / "afile"}) {
    auto r = run({"resources", "--path", p.string()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.find(p.string()) != std::string::npos);
  }
  CHECK_FALSE(fs::exists(t.root / "missing"));  // never created
}

TEST_CASE("coffee resources performs no writes below the inspected directory", "[host][cli]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  t.put("work/keep.txt", "keep");
  t.dir("work/sub");
  auto snapshot = [&] {
    std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> m;
    for (auto& e : fs::recursive_directory_iterator(t.root))
      m[e.path().string()] = {e.is_regular_file() ? e.file_size() : 0, fs::last_write_time(e.path())};
    m[dir] = {0, fs::last_write_time(dir)};
    return m;
  };
  auto before = snapshot();
  CHECK(run({"resources", "--path", dir}).code == 0);
  CHECK(run({"resources", "--path", dir, "--json", "--storage-mb", "1"}).code <= 1);
  CHECK(run({"resources", "--path", dir, "--json", "--memory-mb", "17592186044415"}).code == 1);
  CHECK(snapshot() == before);
}

TEST_CASE("existing commands are unaffected by the resources command", "[host][cli]") {
  CHECK(run({"--version"}).code == 0);
  auto u = run({"frobnicate"});
  CHECK(u.code == 2);
  CHECK(u.err.find("unknown command") != std::string::npos);
  CHECK(run({"prepare"}).code == 3);
  CHECK(run({"resource"}).code == 2);  // near-miss names are not aliases
  auto v = run({"validate", (fs::path(COFFEE_TEST_SOURCE_DIR) / "examples/coffee.yaml").string(), "--json"});
  CHECK(v.code == 0);
  CHECK(json::parse(v.out)["valid"] == true);
}

// ---- fix round 1: non-UTF-8 Linux arguments and paths -----------------------------------------------------------------------
// Linux argv and file names are bytes. The filesystem always sees the original bytes; only the JSON/diagnostic *presentation*
// replaces invalid UTF-8 with U+FFFD, and a replaced storage.path adds a warning.

namespace {
const std::string kFffd = "\xEF\xBF\xBD";

// One invalid sequence per entry: stray 0xFF, truncated 3-byte sequence at end / in the middle, overlong, surrogate, > U+10FFFF, lone continuation.
std::vector<std::string> bad_utf8_names() {
  return {"bad\xff", "trunc\xE2\x82", "mid\xE2\x82-x", "over\xC0\xAF", "surr\xED\xA0\x80", "big\xF4\x90\x80\x80", "cont\x80"};
}

// A well-formed single JSON object on stdout (strict parse; this throws on invalid UTF-8 or trailing text).
json strict_object(const Result& r) {
  json j = json::parse(r.out);
  REQUIRE(j.is_object());
  REQUIRE(r.out.back() == '\n');
  REQUIRE(r.out.find('\n') == r.out.size() - 1);
  return j;
}

bool mentions_serializer(const std::string& s) { return s.find("json.exception") != std::string::npos || s.find("type_error") != std::string::npos || s.find("invalid UTF-8") != std::string::npos; }
}  // namespace

TEST_CASE("an existing directory with a non-UTF-8 name is observed through its original bytes", "[host][cli][utf8]") {
  for (const std::string& name : bad_utf8_names()) {
    Tree t;
    std::string dir = (t.root / name).string();
    REQUIRE(::mkdir(dir.c_str(), 0755) == 0);
    t.put(name + "/sentinel.txt", "keep");
    // the replaced spelling must NOT exist, so success proves the original bytes were used for the filesystem
    REQUIRE_FALSE(fs::exists(t.root / "bad\xEF\xBF\xBD"));
    std::vector<std::string> args = {"resources", "--json", "--path", dir};
    auto before_args = args;
    struct statvfs sv;
    REQUIRE(statvfs(dir.c_str(), &sv) == 0);

    auto r = run(args);
    INFO("name bytes: " << json(name).dump(-1, ' ', false, json::error_handler_t::replace));
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    json j = strict_object(r);
    CHECK(j["ok"] == true);
    CHECK(j["schema_version"] == 1);
    // display is lossy: invalid sequences became U+FFFD, the valid prefix is untouched
    std::string shown = j["storage"]["path"].get<std::string>();
    CHECK(shown.find(kFffd) != std::string::npos);
    CHECK(shown.rfind((t.root / "").string(), 0) == 0);
    CHECK(shown != dir);
    // the observation is of the real directory
    CHECK(j["storage"]["total_bytes"].get<uint64_t>() == static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize);
    // an explicit warning says storage.path was altered and cannot be round-tripped
    bool warned = false;
    for (auto& w : j["warnings"])
      if (w.get<std::string>().find("storage.path") != std::string::npos && w.get<std::string>().find("UTF-8") != std::string::npos) warned = true;
    CHECK(warned);
    // nothing was modified: arguments, directory contents
    CHECK(args == before_args);
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);
    CHECK(fs::exists(fs::path(dir) / "sentinel.txt"));

    // low level: the resolved path keeps the original bytes
    HostInputs in = observe_live(dir);
    CHECK(in.storage_path.string() == dir);
    REQUIRE(in.fs_stat.has_value());
    CHECK(in.fs_stat->blocks == sv.f_blocks);

    // human mode works too and writes nothing to stderr
    auto h = run({"resources", "--path", dir});
    CHECK(h.code == 0);
    CHECK(h.err.empty());
    CHECK(h.out.find("Storage (filesystem of ") != std::string::npos);
  }
}

TEST_CASE("valid UTF-8 paths stay verbatim and add no replacement warning", "[host][cli][utf8]") {
  Tree t;
  std::string name = "caf\xC3\xA9-\xE2\x82\xAC-\xF0\x9F\x8D\xB5";  // é, euro sign, tea emoji
  std::string dir = (t.root / name).string();
  fs::create_directories(dir);
  auto r = run({"resources", "--json", "--path", dir});
  REQUIRE(r.code == 0);
  json j = strict_object(r);
  CHECK(j["storage"]["path"].get<std::string>() == fs::canonical(dir).string());
  CHECK(j["storage"]["path"].get<std::string>().find(kFffd) == std::string::npos);
  for (auto& w : j["warnings"]) CHECK(w.get<std::string>().find("storage.path") == std::string::npos);
}

TEST_CASE("a missing or non-directory byte path keeps the underlying io_error, not a serialization error", "[host][cli][utf8]") {
  Tree t;
  t.put("plain-file", "x");
  for (const std::string& name : bad_utf8_names()) {
    for (const std::string& path : {(t.root / ("missing-" + name)).string(), (t.root / "plain-file" / name).string()}) {
      auto r = run({"resources", "--json", "--path", path});
      INFO(json(path).dump(-1, ' ', false, json::error_handler_t::replace));
      CHECK(r.code == 1);
      CHECK(r.err.empty());
      json j = strict_object(r);
      CHECK(j["ok"] == false);
      CHECK(j["error"]["code"] == "io_error");
      std::string msg = j["error"]["message"].get<std::string>();
      CHECK_FALSE(mentions_serializer(msg));
      CHECK(msg.find("cannot access") != std::string::npos);
      CHECK((msg.find("No such file") != std::string::npos || msg.find("Not a directory") != std::string::npos));
      CHECK(msg.find(kFffd) != std::string::npos);
    }
    auto h = run({"resources", "--path", (t.root / ("missing-" + name)).string()});  // human mode: raw diagnostic, status 1, no abort
    CHECK(h.code == 1);
    CHECK(h.out.empty());
    CHECK(h.err.find("No such file") != std::string::npos);
  }
  CHECK_FALSE(fs::exists(t.root / "missing-bad\xff"));  // never created
}

TEST_CASE("non-UTF-8 option names, values and positionals give exit 2 invalid_argument JSON", "[host][cli][utf8]") {
  Tree t;
  std::string dir = tmpdir_for_cli(t);
  struct Case {
    std::vector<std::string> tail;
    std::string needle;  // a valid-UTF-8 fragment of the underlying message that must survive
  };
  for (const std::string& bad : bad_utf8_names()) {
    std::vector<Case> cases = {
        {{"--bad" + bad}, "unknown option"},
        {{"-" + bad}, "unknown option"},
        {{"--cpus", bad}, "--cpus"},
        {{"--memory-mb", bad}, "--memory-mb"},
        {{"--storage-mb", bad}, "--storage-mb"},
        {{"--reserve-memory-mb", bad}, "--reserve-memory-mb"},
        {{"--reserve-storage-mb=" + bad}, "--reserve-storage-mb"},
        {{"--path", dir, bad}, "unexpected argument"},
        {{bad}, "unexpected argument"},
        {{"--json=" + bad}, "--json"},
    };
    for (auto& c : cases) {
      for (bool json_first : {true, false}) {
        std::vector<std::string> args = {"resources"};
        if (json_first) args.push_back("--json");
        for (auto& a : c.tail) args.push_back(a);
        if (!json_first) args.push_back("--json");
        INFO("arg: " << json(c.tail.back()).dump(-1, ' ', false, json::error_handler_t::replace) << " json_first=" << json_first);
        auto r = run(args);  // must return, never terminate
        CHECK(r.code == 2);
        CHECK(r.err.empty());
        json j = strict_object(r);
        CHECK(j.size() == 3);
        CHECK(j["schema_version"] == 1);
        CHECK(j["ok"] == false);
        CHECK(j["error"]["code"] == "invalid_argument");
        std::string msg = j["error"]["message"].get<std::string>();
        CHECK_FALSE(mentions_serializer(msg));
        CHECK(msg.find(c.needle) != std::string::npos);
      }
      // human mode: same status, diagnostic on stderr only
      std::vector<std::string> hargs = {"resources"};
      for (auto& a : c.tail) hargs.push_back(a);
      auto h = run(hargs);
      CHECK(h.code == 2);
      CHECK(h.out.empty());
      CHECK(h.err.find(c.needle) != std::string::npos);
    }
  }
}

TEST_CASE("valid-UTF-8 usage errors are reported byte for byte (no replacement)", "[host][cli][utf8]") {
  auto r = run({"resources", "--json", "--caf\xC3\xA9"});
  CHECK(r.code == 2);
  CHECK(strict_object(r)["error"]["message"].get<std::string>().find("--caf\xC3\xA9") != std::string::npos);
}

TEST_CASE("to_valid_utf8 replaces only the invalid sequences", "[host][utf8]") {
  bool replaced = true;
  CHECK(to_valid_utf8("plain /tmp/x", &replaced) == "plain /tmp/x");
  CHECK_FALSE(replaced);
  CHECK(to_valid_utf8("caf\xC3\xA9 \xE2\x82\xAC", &replaced) == "caf\xC3\xA9 \xE2\x82\xAC");
  CHECK_FALSE(replaced);
  CHECK(to_valid_utf8("a\xff" "b", &replaced) == "a" + kFffd + "b");
  CHECK(replaced);
  CHECK(to_valid_utf8("\xE2\x82", &replaced) == kFffd);  // truncated sequence at the end
  CHECK(replaced);
  CHECK(to_valid_utf8("", &replaced).empty());
  CHECK_FALSE(replaced);
  // the result is always strictly serializable
  for (const std::string& n : bad_utf8_names()) CHECK_NOTHROW(json(to_valid_utf8(n)).dump());
}
