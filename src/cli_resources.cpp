// `coffee resources`: read-only host resource observation and dry-run contribution check (see docs/host-resources.md).
#include <cstdio>
#include <iostream>

#include "cli_internal.hpp"
#include "coffee/host_resources.hpp"
#include "coffee/util.hpp"

namespace coffee::cli {

namespace {

using json = nlohmann::json;
using namespace coffee::host;

const char* kResourcesHelp = R"(Usage: coffee resources [--path DIR] [--json] [--cpus N] [--memory-mb N] [--storage-mb N]
                        [--reserve-memory-mb N] [--reserve-storage-mb N]

Reports what this process can see of the machine and checks, as a dry run, whether a contribution fits. It never
allocates, reserves, enforces or writes anything, and needs no network or token.

  --path DIR              Directory whose filesystem is measured (default '.'); must exist, be readable and searchable.
  --json                  One JSON object (schema_version 1) on stdout; errors are {"schema_version":1,"ok":false,"error":{...}}.
  --cpus N                Requested whole CPU slots (positive integer).
  --memory-mb N           Requested memory in MiB (positive integer).
  --storage-mb N          Requested storage in MiB (positive integer).
  --reserve-memory-mb N   Memory kept back for the owner (default 1024; 0 allowed).
  --reserve-storage-mb N  Storage kept back for the owner (default 1024; 0 allowed).

Unknown observations are reported as unknown (null in JSON), never as 0; a request whose maximum is unknown or too small
is rejected. Exit codes: 0 ok; 1 request rejected or the path/observation failed; 2 usage error.
)";

[[noreturn]] void usage_fail(const std::string& msg) { throw Error("invalid_argument", msg); }

// Strict decimal: digits only, no sign/space/prefix, must fit uint64.
uint64_t parse_whole(const std::string& opt, const std::string& v) {
  if (v.empty()) usage_fail(opt + " requires a whole number, got an empty value");
  uint64_t n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') usage_fail(opt + " must be a non-negative whole decimal number, got '" + v + "'");
    uint64_t d = static_cast<uint64_t>(c - '0');
    if (n > (UINT64_MAX - d) / 10) usage_fail(opt + " value '" + v + "' is too large");
    n = n * 10 + d;
  }
  return n;
}

// MiB -> bytes with the overflow checked before the multiplication.
uint64_t parse_mib(const std::string& opt, const std::string& v, bool allow_zero) {
  uint64_t n = parse_whole(opt, v);
  if (n == 0 && !allow_zero) usage_fail(opt + " must be a positive whole number, got '" + v + "'");
  if (n > UINT64_MAX / kMiB) usage_fail(opt + " value '" + v + "' MiB does not fit in 64-bit bytes");
  return n * kMiB;
}

json error_json(const std::string& code, const std::string& message) {
  // The message may echo raw argv/path bytes; show invalid UTF-8 as U+FFFD so the envelope can always be serialized.
  return {{"schema_version", 1}, {"ok", false}, {"error", {{"code", code}, {"message", to_valid_utf8(message)}}}};
}

std::string bytes_text(const std::optional<uint64_t>& v) {
  if (!v) return "unknown";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(*v) / static_cast<double>(1ull << 30));
  return std::to_string(*v) + " bytes (" + buf + ")";
}
std::string num_text(const std::optional<uint64_t>& v) { return v ? std::to_string(*v) : "unknown"; }
std::string cores_text(const std::optional<double>& v) {
  if (!v) return "unknown";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", *v);
  return buf;
}

void print_human(const Report& r, std::ostream& out) {
  out << "Coffee host resources (read-only observation; no resources were allocated, reserved or enforced)\n"
      << "Scope: this process's CPU affinity, the visible cgroup v2 hierarchy, /proc/meminfo and the filesystem of the path below.\n"
      << "It does not identify the physical host, and it is a snapshot, not a guarantee. 'unknown' means it could not be observed.\n\n";
  out << "CPU\n"
      << "  logical CPUs (affinity):   " << num_text(r.cpu.affinity_logical_cpus) << "\n"
      << "  physical cores (allowed):  " << num_text(r.cpu.physical_cores) << "  (descriptive, not scheduling slots)\n"
      << "  cgroup CPU quota:          " << to_string(r.cpu.quota_status);
  if (r.cpu.quota_cores) out << " (" << cores_text(r.cpu.quota_cores) << " cores)";
  out << "\n  effective cores:           " << cores_text(r.cpu.effective_cores) << "\n\n";
  out << "Memory\n"
      << "  host total:                " << bytes_text(r.memory.host_total_bytes) << "\n"
      << "  host available:            " << bytes_text(r.memory.host_available_bytes) << "\n"
      << "  cgroup limit:              " << to_string(r.memory.cgroup_status);
  if (r.memory.cgroup_limit_bytes) out << " (tightest limit " << bytes_text(r.memory.cgroup_limit_bytes) << ")";
  out << "\n  cgroup headroom:           " << bytes_text(r.memory.cgroup_headroom_bytes) << "\n"
      << "  effective available:       " << bytes_text(r.memory.effective_available_bytes) << "\n\n";
  out << "Storage (filesystem of " << r.storage.path << ")\n"
      << "  total:                     " << bytes_text(r.storage.total_bytes) << "\n"
      << "  available to this user:    " << bytes_text(r.storage.available_bytes) << "\n\n";
  out << "Budget (effective availability minus the owner reserve)\n"
      << "  reserve memory / storage:  " << bytes_text(r.budget.reserve_memory_bytes) << " / " << bytes_text(r.budget.reserve_storage_bytes) << "\n"
      << "  max CPU slots:             " << num_text(r.budget.max_cpu_slots) << "\n"
      << "  max memory:                " << bytes_text(r.budget.max_memory_bytes) << "\n"
      << "  max storage:               " << bytes_text(r.budget.max_storage_bytes) << "\n\n";
  out << "Request (dry run)\n";
  if (!r.request.cpus && !r.request.memory_bytes && !r.request.storage_bytes) {
    out << "  none requested\n";
  } else {
    if (r.request.cpus) out << "  cpus:     " << *r.request.cpus << "\n";
    if (r.request.memory_bytes) out << "  memory:   " << bytes_text(r.request.memory_bytes) << "\n";
    if (r.request.storage_bytes) out << "  storage:  " << bytes_text(r.request.storage_bytes) << "\n";
    out << (r.request.valid ? "  result:   fits the budget (nothing was reserved)\n" : "  result:   request rejected\n");
    for (const auto& e : r.request.errors) out << "  - " << e << "\n";
  }
  if (!r.warnings.empty()) {
    out << "\nWarnings\n";
    for (const auto& w : r.warnings) out << "  - " << w << "\n";
  }
}

}  // namespace

int run_resources(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  for (const auto& a : args)
    if (a == "--help" || a == "-h") return out << kResourcesHelp, kOk;

  Options o = parse_options(args, {"path", "cpus", "memory-mb", "storage-mb", "reserve-memory-mb", "reserve-storage-mb"}, {"json"});
  bool json_mode = o.has("json");
  if (!o.error.empty())  // parsing stopped early, so look for --json ourselves
    for (const auto& a : args)
      if (a == "--json") json_mode = true;

  auto fail = [&](int status, const std::string& code, const std::string& msg) {
    if (json_mode) out << error_json(code, msg).dump() << "\n";
    else if (status == kUsage) err << "coffee: resources: " << msg << "\nTry 'coffee resources --help'.\n";
    else err << "coffee: resources: " << msg << "\n";
    return status;
  };

  try {
    if (!o.error.empty()) usage_fail(o.error);
    if (!o.positional.empty()) usage_fail("unexpected argument '" + o.positional[0] + "'");
    std::string path = o.get("path").value_or(".");
    if (path.empty()) usage_fail("--path must not be empty");
    Reserves reserves;
    if (auto v = o.get("reserve-memory-mb")) reserves.memory_bytes = parse_mib("--reserve-memory-mb", *v, true);
    if (auto v = o.get("reserve-storage-mb")) reserves.storage_bytes = parse_mib("--reserve-storage-mb", *v, true);
    Request req;
    if (auto v = o.get("cpus")) {
      uint64_t n = parse_whole("--cpus", *v);
      if (n == 0) usage_fail("--cpus must be a positive whole number, got '" + *v + "'");
      req.cpus = n;
    }
    if (auto v = o.get("memory-mb")) req.memory_bytes = parse_mib("--memory-mb", *v, false);
    if (auto v = o.get("storage-mb")) req.storage_bytes = parse_mib("--storage-mb", *v, false);

    HostInputs inputs;
    try {
      inputs = observe_live(path);
    } catch (const Error& e) {
      return fail(kFailed, "io_error", e.what());
    }
    Report report = evaluate(inputs, reserves);
    apply_request(report, req);
    if (json_mode) {
      out << to_json(report).dump() << "\n";
    } else {
      print_human(report, out);
      if (!report.ok) err << "coffee: resources: request rejected (see the Request section)\n";
    }
    return report.ok ? kOk : kFailed;
  } catch (const Error& e) {
    return fail(e.code() == "invalid_argument" ? kUsage : kFailed, e.code() == "invalid_argument" ? "invalid_argument" : "io_error", e.what());
  } catch (const std::exception& e) {
    return fail(kFailed, "io_error", e.what());
  }
}

}  // namespace coffee::cli
