#include "coffee/cli.hpp"

#include <algorithm>
#include <iostream>
#include <optional>

#include "cli_internal.hpp"
#include "coffee/executor.hpp"
#include "coffee/model.hpp"
#include "coffee/store.hpp"
#include "coffee/util.hpp"

namespace coffee {

using json = nlohmann::json;

namespace {

constexpr int kOk = cli::kOk, kFailed = cli::kFailed, kUsage = cli::kUsage, kUnimplemented = cli::kUnimplemented;

const char* kMainHelp = R"(coffee - native cooperative compute scheduler (Beans run tasks for a coordinator)

Usage: coffee <command> [options]

Local commands:
  init --state DIR [--json]     Create/verify a state directory (identity, token, database, artifacts)
  validate FILE [--json]        Parse and validate a workload YAML file, printing defaults with --json
  plan FILE [--json]            Show dependency layers; with --server also check Bean capacity

Coordinator and Beans:
  serve --state DIR             Run the coordinator (loopback HTTP API, bearer token, durable state)
  join --work-dir DIR           Run a Bean: register, claim, execute under a supervisor, publish verified outputs
  beans                         List Beans (capacity, usage, online/unreachable/draining)
  drain BEAN [--undo]           Stop (or resume) new claims on a Bean

Jobs:
  brew FILE [--wait] [--out D]  Upload inputs and submit a workload; --wait follows it to completion
  status [JOB]                  List jobs, or show a job's tasks and attempt history
  logs JOB TASK [--stderr]      Captured stdout/stderr of an attempt
  explain JOB TASK              Why a task is (not) running, per-Bean fit
  cancel JOB [--task T]         Cancel a job or one task; late results are rejected

Planned (recognised, but not implemented in this build; exit status 3):
  prepare

Run 'coffee <command> --help' for options. Client commands take --server URL and --token-file FILE (or --state DIR).
Options:
  -h, --help       Show this help (also: coffee <command> --help)
  -V, --version    Print the version

Exit codes: 0 success; 1 operation failed or job did not succeed; 2 usage error; 3 not implemented;
            4 coordinator unreachable/unauthorized; 5 --wait timeout.
)";

const char* kInitHelp = R"(Usage: coffee init --state DIR [--json]

Creates DIR (mode 0700) with config.json (cluster id), token (random, mode 0600), coffee.db and
artifacts/{inputs,outputs,logs,tmp}. Running it again never overwrites the identity, the token or
stored results. The token is never printed.
)";

const char* kValidateHelp = R"(Usage: coffee validate FILE [--json]

Parses FILE as a coffee/v1alpha1 Workload and checks the schema, dependencies, resources and paths
(input sources must exist below the file's directory). Exit 0 if valid, 1 if invalid.
With --json prints {"valid":true,"workload":<normalized with defaults>} or
{"valid":false,"error":{"code","field","message"}}.
)";

const char* kPlanned[] = {"prepare"};

struct Parsed {
  std::vector<std::string> positional;
  std::optional<std::string> state;
  bool json = false;
  bool help = false;
  std::string error;
};

// Minimal option parser: --json, --help/-h, and --state DIR / --state=DIR (when allowed).
Parsed parse_args(const std::vector<std::string>& args, bool allow_state) {
  Parsed p;
  for (size_t i = 0; i < args.size() && p.error.empty(); i++) {
    const std::string& a = args[i];
    if (a == "--json") p.json = true;
    else if (a == "--help" || a == "-h") p.help = true;
    else if (allow_state && a == "--state") {
      if (i + 1 >= args.size()) p.error = "--state requires a directory argument";
      else p.state = args[++i];
    } else if (allow_state && a.rfind("--state=", 0) == 0) p.state = a.substr(8);
    else if (a.size() > 1 && a[0] == '-') p.error = "unknown option '" + a + "'";
    else p.positional.push_back(a);
  }
  return p;
}

int usage_error(std::ostream& err, const std::string& msg) {
  err << "coffee: " << msg << "\nTry 'coffee --help'.\n";
  return kUsage;
}

int cmd_init(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Parsed p = parse_args(args, true);
  if (p.help) return out << kInitHelp, kOk;
  if (!p.error.empty()) return usage_error(err, "init: " + p.error);
  if (!p.positional.empty()) return usage_error(err, "init: unexpected argument '" + p.positional[0] + "'");
  if (!p.state || p.state->empty()) return usage_error(err, "init: --state DIR is required");
  try {
    InitResult r = init_state_dir(*p.state);
    if (p.json) {
      out << json{{"state_dir", r.state_dir.string()}, {"cluster_id", r.cluster_id}, {"created", r.created}, {"token_created", r.token_created},
                  {"database_created", r.database_created}, {"token_path", r.token_path.string()}, {"database_path", r.database_path.string()}}
                 .dump()
          << "\n";
    } else if (r.created) {
      out << "Initialized Coffee state in " << r.state_dir.string() << "\n  cluster id: " << r.cluster_id << "\n  token file: " << r.token_path.string()
          << " (mode 0600; keep it private)\n  database:   " << r.database_path.string() << "\n";
    } else {
      out << "Coffee state in " << r.state_dir.string() << " is already initialized (cluster " << r.cluster_id << "); nothing was overwritten.\n";
    }
    return kOk;
  } catch (const Error& e) {
    err << "coffee: init failed: " << e.what() << "\n";
    return kFailed;
  }
}

int cmd_validate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Parsed p = parse_args(args, false);
  if (p.help) return out << kValidateHelp, kOk;
  if (!p.error.empty()) return usage_error(err, "validate: " + p.error);
  if (p.positional.size() != 1) return usage_error(err, "validate: expected exactly one FILE argument");
  try {
    json w = parse_workload(p.positional[0]);
    if (p.json) out << json{{"valid", true}, {"workload", w}}.dump() << "\n";
    else out << "OK: workload '" << w["metadata"]["name"].get<std::string>() << "' with " << w["spec"]["tasks"].size() << " task(s) is valid\n";
    return kOk;
  } catch (const WorkloadError& e) {
    if (p.json) out << json{{"valid", false}, {"error", {{"code", e.code()}, {"field", e.field()}, {"message", e.detail()}}}}.dump() << "\n";
    else err << "coffee: invalid workload: " << e.what() << "\n";
    return kFailed;
  } catch (const Error& e) {
    if (p.json) out << json{{"valid", false}, {"error", {{"code", e.code()}, {"field", ""}, {"message", e.what()}}}}.dump() << "\n";
    else err << "coffee: cannot read workload: " << e.what() << "\n";
    return kFailed;
  }
}

}  // namespace

int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    err << kMainHelp;
    return kUsage;
  }
  const std::string& cmd = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());
  if (cmd == "--help" || cmd == "-h" || cmd == "help") return out << kMainHelp, kOk;
  if (cmd == "--version" || cmd == "-V" || cmd == "version") return out << "coffee " << kVersion << " (api " << kApiVersion << ")\n", kOk;
  if (cmd == "__supervise") return supervisor_main(rest);  // internal: spawned by Beans, see executor.hpp
  {
    int net_status = 0;
    if (cli::run_net_command(cmd, rest, out, err, net_status)) return net_status;
  }
  if (cmd == "init") return cmd_init(rest, out, err);
  if (cmd == "validate") return cmd_validate(rest, out, err);
  if (std::find(std::begin(kPlanned), std::end(kPlanned), cmd) != std::end(kPlanned)) {
    err << "coffee: '" << cmd << "' is not implemented in this build\n";
    return kUnimplemented;
  }
  return usage_error(err, "unknown command '" + cmd + "'");
}

}  // namespace coffee
