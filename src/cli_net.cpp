#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "cli_internal.hpp"
#include "coffee/artifacts.hpp"
#include "coffee/client.hpp"
#include "coffee/model.hpp"
#include "coffee/server.hpp"
#include "coffee/worker.hpp"

namespace coffee::cli {

using json = nlohmann::json;
namespace fs = std::filesystem;

Options parse_options(const std::vector<std::string>& args, const std::set<std::string>& value_opts, const std::set<std::string>& flag_opts) {
  Options o;
  for (size_t i = 0; i < args.size() && o.error.empty(); i++) {
    const std::string& a = args[i];
    if (a == "--help" || a == "-h") {
      o.flags.insert("help");
    } else if (a.rfind("--", 0) == 0) {
      std::string name = a.substr(2), value;
      size_t eq = name.find('=');
      bool inline_value = eq != std::string::npos;
      if (inline_value) {
        value = name.substr(eq + 1);
        name = name.substr(0, eq);
      }
      if (flag_opts.count(name)) {
        if (inline_value) o.error = "option '--" + name + "' does not take a value";
        else o.flags.insert(name);
      } else if (value_opts.count(name)) {
        if (!inline_value) {
          if (i + 1 >= args.size()) {
            o.error = "option '--" + name + "' requires a value";
            break;
          }
          value = args[++i];
        }
        o.values[name] = value;
      } else {
        o.error = "unknown option '" + a + "'";
      }
    } else if (a.size() > 1 && a[0] == '-') {
      o.error = "unknown option '" + a + "'";
    } else {
      o.positional.push_back(a);
    }
  }
  return o;
}

namespace {

const std::set<std::string> kClientValues = {"server", "token-file", "state"};
const std::set<std::string> kClientFlags = {"allow-plaintext-remote", "json"};

std::set<std::string> with(std::set<std::string> base, std::initializer_list<const char*> more) {
  for (const char* m : more) base.insert(m);
  return base;
}

int usage(std::ostream& err, const std::string& cmd, const std::string& msg) {
  err << "coffee " << cmd << ": " << msg << "\nTry 'coffee " << cmd << " --help'.\n";
  return kUsage;
}

int64_t parse_int(const std::string& what, const std::string& v, int64_t lo, int64_t hi) {
  size_t used = 0;
  int64_t x = 0;
  try {
    x = std::stoll(v, &used);
  } catch (const std::exception&) {
    used = 0;
  }
  if (used != v.size() || v.empty() || x < lo || x > hi) throw Error("invalid_argument", what + " must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi));
  return x;
}

const char* kConnectionHelp = R"(
Connection options (client commands):
  --server URL           coordinator, default $COFFEE_SERVER or http://127.0.0.1:7077 (plain http only)
  --token-file FILE      cluster token file (default $COFFEE_TOKEN_FILE)
  --state DIR            read the token from DIR/token (same-host convenience)
  --allow-plaintext-remote   permit a non-loopback server over plaintext HTTP. This adds NO encryption: use an
                         SSH/WireGuard/Tailscale tunnel (see docs/native-protocol.md).
  --json                 machine-readable output where supported
Exit codes: 0 ok; 1 operation failed or job did not succeed; 2 usage error; 3 not implemented;
            4 coordinator unreachable / unauthorized / server error; 5 --wait timed out.
)";

struct Connection {
  std::unique_ptr<Client> client;
};

// Builds a client from the common options; returns an exit code (non-zero) on failure.
int connect(const Options& o, const std::string& cmd, std::ostream& err, Connection& c) {
  ClientOptions co;
  if (auto s = o.get("server")) co.url = *s;
  else if (const char* e = std::getenv("COFFEE_SERVER")) co.url = e;
  std::optional<fs::path> token_path;
  if (auto t = o.get("token-file")) token_path = *t;
  else if (auto st = o.get("state")) token_path = fs::path(*st) / "token";
  else if (const char* e = std::getenv("COFFEE_TOKEN_FILE")) token_path = e;
  if (!token_path) return usage(err, cmd, "no credentials configured: pass --token-file FILE or --state DIR (or set COFFEE_TOKEN_FILE)");
  try {
    std::string tok = read_file_limited(*token_path, 4096);
    while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
    co.token = tok;
  } catch (const Error& e) {
    err << "coffee " << cmd << ": cannot read token file: " << e.what() << "\n";
    return kFailed;
  }
  co.allow_plaintext_remote = o.has("allow-plaintext-remote");
  try {
    c.client = std::make_unique<Client>(co);
  } catch (const Error& e) {
    err << "coffee " << cmd << ": " << e.what() << "\n";
    return kUsage;
  }
  return kOk;
}

int api_failure(const std::string& cmd, const ApiResponse& r, std::ostream& err) {
  if (r.status == 401) {
    err << "coffee " << cmd << ": unauthorized: the coordinator rejected the cluster token\n";
    return kNetwork;
  }
  err << "coffee " << cmd << ": " << r.error_code << ": " << r.error_message << "\n";
  return r.status >= 500 ? kNetwork : kFailed;
}

// Runs `fn`, translating transport and Coffee errors into exit codes.
template <class F>
int guarded(const std::string& cmd, std::ostream& err, F fn) {
  try {
    return fn();
  } catch (const NetworkError& e) {
    err << "coffee " << cmd << ": coordinator unreachable: " << e.what() << "\n";
    return kNetwork;
  } catch (const WorkloadError& e) {
    err << "coffee " << cmd << ": invalid workload: " << e.what() << "\n";
    return kFailed;
  } catch (const Error& e) {
    err << "coffee " << cmd << ": " << e.what() << "\n";
    return e.code() == "invalid_argument" ? kUsage : kFailed;
  }
}

std::string percent_encode(const std::string& v) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char ch : v) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '/') out += static_cast<char>(ch);
    else {
      out += '%';
      out += hex[ch >> 4];
      out += hex[ch & 15];
    }
  }
  return out;
}

// ---------------------------------------------------------------- serve / join

void block_termination_signals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
}

void wait_for_termination_signal() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  int sig = 0;
  sigwait(&set, &sig);
}

const char* kServeHelp = R"(Usage: coffee serve --state DIR [options]

Runs the coordinator: durable state in DIR (see 'coffee init'), authenticated HTTP API under /v1.
  --listen HOST:PORT        default 127.0.0.1:7077 (port 0 picks a free port)
  --allow-plaintext-nonloopback
                            allow a non-loopback HOST. This build has no TLS and the flag adds no encryption:
                            only use it on a network that is already encrypted (WireGuard/Tailscale/VPN).
  --lease-ms N              lease duration (default 10000)
  --heartbeat-timeout-ms N  Bean considered unreachable after this silence (default 10000)
  --deadline-grace-ms N     slack added to a task's timeout for staging/transfer (default 30000)
  --max-artifact-bytes N    per-file artifact limit (default 268435456)
  --threads N               request threads (default 8; may grow to 4x; at most 128 queued requests)
  --ready-file FILE         write {"url","port","pid","cluster_id"} atomically once listening
Stops cleanly on SIGINT/SIGTERM. Never logs the token or lease ids. Exit: 0 clean stop, 1 failure, 2 usage.
)";

int cmd_serve(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, {"state", "listen", "lease-ms", "heartbeat-timeout-ms", "deadline-grace-ms", "max-artifact-bytes", "threads", "ready-file"},
                            {"allow-plaintext-nonloopback"});
  if (o.has("help")) return out << kServeHelp, kOk;
  if (!o.error.empty()) return usage(err, "serve", o.error);
  if (!o.positional.empty()) return usage(err, "serve", "unexpected argument '" + o.positional[0] + "'");
  if (!o.get("state")) return usage(err, "serve", "--state DIR is required");
  CoordinatorOptions co;
  try {
    co.state_dir = *o.get("state");
    protocol::ListenAddress la = protocol::parse_listen(o.get("listen").value_or("127.0.0.1:" + std::to_string(protocol::kDefaultPort)));
    co.host = la.host;
    co.port = la.port;
    co.allow_plaintext_nonloopback = o.has("allow-plaintext-nonloopback");
    if (auto v = o.get("lease-ms")) co.lease_ms = parse_int("--lease-ms", *v, 100, 3'600'000);
    if (auto v = o.get("heartbeat-timeout-ms")) co.heartbeat_timeout_ms = parse_int("--heartbeat-timeout-ms", *v, 100, 3'600'000);
    if (auto v = o.get("deadline-grace-ms")) co.deadline_grace_ms = parse_int("--deadline-grace-ms", *v, 0, 86'400'000);
    if (auto v = o.get("max-artifact-bytes")) co.max_artifact_bytes = static_cast<uint64_t>(parse_int("--max-artifact-bytes", *v, 1, int64_t{1} << 40));
    if (auto v = o.get("threads")) co.worker_threads = static_cast<size_t>(parse_int("--threads", *v, 1, 128));
  } catch (const Error& e) {
    return usage(err, "serve", e.what());
  }
  block_termination_signals();  // before any thread exists, so every thread inherits the mask
  try {
    Coordinator coordinator(co);
    int port = coordinator.start();
    std::string url = "http://" + (co.host.find(':') != std::string::npos ? "[" + co.host + "]" : co.host) + ":" + std::to_string(port);
    if (auto rf = o.get("ready-file"))
      write_file_atomic(*rf, json{{"url", url}, {"port", port}, {"pid", static_cast<int64_t>(::getpid())}, {"cluster_id", coordinator.store().cluster_id()}}.dump() + "\n", 0600);
    out << "coffee serve: listening on " << url << " (plaintext HTTP; bearer token required; cluster " << coordinator.store().cluster_id() << ")" << std::endl;
    wait_for_termination_signal();
    out << "coffee serve: stopping" << std::endl;
    coordinator.stop();
    return kOk;
  } catch (const Error& e) {
    err << "coffee serve: " << e.what() << "\n";
    return e.code() == "insecure_listen" ? kUsage : kFailed;
  }
}

const char* kJoinHelp = R"(Usage: coffee join --work-dir DIR [connection options] [options]

Runs a Bean (long-running worker): registers, heartbeats, claims tasks that fit, runs each command under an
exec'd supervisor and publishes verified outputs. Stops on SIGINT/SIGTERM (running attempts are returned for retry).
  --work-dir DIR            attempt workspaces and the persistent Bean id (mode 0700)
  --id ID  --name NAME      Bean identity (default: id persisted in DIR/bean-id, generated once)
  --cpus N                  CPU slots to advertise; each running slot is pinned to a distinct CPU id (default: allowed CPUs)
  --memory-mb N             memory budget to advertise; each task's RLIMIT_AS is its memory_mb (default: half of MemTotal)
  --heartbeat-ms N          heartbeat/lease-renewal period (default 2000)
  --poll-ms N               idle claim polling period (default 500)
  --max-artifact-bytes N    per-file output limit, also applied as RLIMIT_FSIZE (default 268435456)
  --max-workspace-bytes N   aggregate bytes one attempt may hold below its workspace, inputs included (default 1073741824).
                            Sampled every 100 ms while running and re-measured after the command tree is dead, so a fast
                            writer is still rejected; it may overshoot by about (write rate x 100 ms) before being killed.
                            Per attempt (N parallel attempts can use N x), not a filesystem quota; /tmp and other absolute
                            paths outside the workspace are not counted.
  --keep-attempts           keep finished attempt workspaces
)";

int cmd_join(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, with(kClientValues, {"work-dir", "id", "name", "cpus", "memory-mb", "heartbeat-ms", "poll-ms", "max-artifact-bytes", "max-workspace-bytes"}),
                            with(kClientFlags, {"keep-attempts"}));
  if (o.has("help")) return out << kJoinHelp << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "join", o.error);
  if (!o.positional.empty()) return usage(err, "join", "unexpected argument '" + o.positional[0] + "'");
  if (!o.get("work-dir")) return usage(err, "join", "--work-dir DIR is required");
  Connection c;
  if (int rc = connect(o, "join", err, c)) return rc;
  WorkerOptions wo;
  try {
    wo.work_dir = *o.get("work-dir");
    wo.bean_id = o.get("id").value_or("");
    wo.name = o.get("name").value_or("");
    if (auto v = o.get("cpus")) wo.cpus = static_cast<int>(parse_int("--cpus", *v, 1, 4096));
    if (auto v = o.get("memory-mb")) wo.memory_mb = parse_int("--memory-mb", *v, 1, int64_t{1} << 30);
    if (auto v = o.get("heartbeat-ms")) wo.heartbeat_ms = parse_int("--heartbeat-ms", *v, 50, 600'000);
    if (auto v = o.get("poll-ms")) wo.poll_ms = parse_int("--poll-ms", *v, 10, 600'000);
    if (auto v = o.get("max-artifact-bytes")) wo.max_artifact_bytes = static_cast<uint64_t>(parse_int("--max-artifact-bytes", *v, 1, int64_t{1} << 40));
    if (auto v = o.get("max-workspace-bytes")) wo.max_workspace_bytes = static_cast<uint64_t>(parse_int("--max-workspace-bytes", *v, 1, int64_t{1} << 50));
    wo.keep_attempt_dirs = o.has("keep-attempts");
    // reuse the validated client options (token, URL, override)
    ClientOptions co;
    if (auto s = o.get("server")) co.url = *s;
    else if (const char* e = std::getenv("COFFEE_SERVER")) co.url = e;
    std::string tok = read_file_limited(o.get("token-file") ? fs::path(*o.get("token-file")) : o.get("state") ? fs::path(*o.get("state")) / "token" : fs::path(std::getenv("COFFEE_TOKEN_FILE")), 4096);
    while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
    co.token = tok;
    co.allow_plaintext_remote = o.has("allow-plaintext-remote");
    wo.client = co;
  } catch (const Error& e) {
    return usage(err, "join", e.what());
  }
  block_termination_signals();
  try {
    Worker worker(wo);
    std::thread sig_thread([&worker] {
      wait_for_termination_signal();
      worker.request_stop();
    });
    sig_thread.detach();
    out << "coffee join: Bean " << worker.bean_id() << " serving " << wo.client.url << std::endl;
    return worker.run() == 0 ? kOk : kFailed;
  } catch (const Error& e) {
    err << "coffee join: " << e.what() << "\n";
    return kFailed;
  }
}

// ---------------------------------------------------------------- beans / drain / cancel / status / logs / explain

const char* kBeansHelp = "Usage: coffee beans [connection options]\n\nLists registered Beans with capacity, usage and state (online|unreachable|draining).\n";

int cmd_beans(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, kClientValues, kClientFlags);
  if (o.has("help")) return out << kBeansHelp << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "beans", o.error);
  Connection c;
  if (int rc = connect(o, "beans", err, c)) return rc;
  return guarded("beans", err, [&] {
    ApiResponse r = c.client->get("/v1/beans");
    if (!r.ok()) return api_failure("beans", r, err);
    if (o.has("json")) return out << r.body.dump() << "\n", kOk;
    out << "ID\tSTATE\tCPUS\tMEMORY_MB\tNAME\n";
    for (const auto& b : r.body["beans"])
      out << b["id"].get<std::string>() << "\t" << b["state"].get<std::string>() << "\t" << b["cpus_used"] << "/" << b["cpus"] << "\t" << b["memory_mb_used"] << "/" << b["memory_mb"]
          << "\t" << b["name"].get<std::string>() << "\n";
    return kOk;
  });
}

int cmd_drain(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, kClientValues, with(kClientFlags, {"undo"}));
  if (o.has("help")) return out << "Usage: coffee drain BEAN [--undo] [connection options]\n\nStops new claims on a Bean; running attempts finish. --undo resumes it.\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "drain", o.error);
  if (o.positional.size() != 1) return usage(err, "drain", "expected exactly one BEAN id");
  Connection c;
  if (int rc = connect(o, "drain", err, c)) return rc;
  return guarded("drain", err, [&] {
    ApiResponse r = c.client->post("/v1/beans/" + o.positional[0] + "/drain", {{"draining", !o.has("undo")}});
    if (!r.ok()) return api_failure("drain", r, err);
    if (o.has("json")) out << r.body.dump() << "\n";
    else out << "Bean " << o.positional[0] << (o.has("undo") ? " resumed" : " draining (no new tasks; running attempts continue)") << "\n";
    return kOk;
  });
}

int cmd_cancel(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, with(kClientValues, {"task", "reason"}), kClientFlags);
  if (o.has("help")) return out << "Usage: coffee cancel JOB [--task TASK] [--reason TEXT] [connection options]\n\nCancels a job (or one task and its dependents). Running attempts lose their lease at once; late results are rejected.\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "cancel", o.error);
  if (o.positional.size() != 1) return usage(err, "cancel", "expected exactly one JOB id");
  Connection c;
  if (int rc = connect(o, "cancel", err, c)) return rc;
  return guarded("cancel", err, [&] {
    json body = {{"reason", o.get("reason").value_or("cancelled by owner")}};
    if (auto t = o.get("task")) body["task_id"] = *t;
    ApiResponse r = c.client->post("/v1/jobs/" + o.positional[0] + "/cancel", body);
    if (!r.ok()) return api_failure("cancel", r, err);
    if (o.has("json")) out << r.body.dump() << "\n";
    else out << "job " << o.positional[0] << ": " << r.body["job_state"].get<std::string>() << " (" << r.body["tasks_cancelled"] << " task(s), " << r.body["attempts_cancelled"] << " running attempt(s) cancelled)\n";
    return kOk;
  });
}

void print_job(const json& j, std::ostream& out) {
  out << "job " << j["id"].get<std::string>() << " (" << j["name"].get<std::string>() << "): " << j["state"].get<std::string>() << "\n";
  for (const auto& t : j["tasks"]) {
    out << "  " << t["id"].get<std::string>() << "\t" << t["state"].get<std::string>() << "\tattempts " << t["attempts_used"] << "/" << t["max_attempts"];
    if (!t["failure_reason"].is_null()) out << "\t" << t["failure_reason"].get<std::string>();
    out << "\n";
    for (const auto& a : t["attempts"])
      out << "      #" << a["attempt"] << " on " << a["bean_id"].get<std::string>() << ": " << a["state"].get<std::string>()
          << (a["reason"].is_null() ? "" : " (" + a["reason"].get<std::string>() + ")") << "\n";
  }
}

int cmd_status(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, kClientValues, kClientFlags);
  if (o.has("help")) return out << "Usage: coffee status [JOB] [connection options]\n\nWithout JOB lists jobs; with JOB prints its tasks and full attempt history (Bean, state, reason).\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "status", o.error);
  if (o.positional.size() > 1) return usage(err, "status", "at most one JOB id");
  Connection c;
  if (int rc = connect(o, "status", err, c)) return rc;
  return guarded("status", err, [&] {
    ApiResponse r = c.client->get(o.positional.empty() ? "/v1/jobs" : "/v1/jobs/" + o.positional[0]);
    if (!r.ok()) return api_failure("status", r, err);
    if (o.has("json")) return out << r.body.dump() << "\n", kOk;
    if (o.positional.empty()) {
      for (const auto& j : r.body["jobs"]) out << j["id"].get<std::string>() << "\t" << j["state"].get<std::string>() << "\t" << j["name"].get<std::string>() << "\ttasks " << j["tasks"]["succeeded"] << "/" << j["tasks"]["total"] << " done\n";
    } else {
      print_job(r.body, out);
    }
    return kOk;
  });
}

int cmd_logs(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, with(kClientValues, {"attempt"}), with(kClientFlags, {"stderr"}));
  if (o.has("help")) return out << "Usage: coffee logs JOB TASK [--attempt N] [--stderr] [connection options]\n\nPrints the bounded captured stdout (or stderr) of an attempt (default: the latest).\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "logs", o.error);
  if (o.positional.size() != 2) return usage(err, "logs", "expected JOB and TASK");
  Connection c;
  if (int rc = connect(o, "logs", err, c)) return rc;
  return guarded("logs", err, [&] {
    std::string path = "/v1/jobs/" + o.positional[0] + "/tasks/" + o.positional[1] + "/logs?stream=" + (o.has("stderr") ? "stderr" : "stdout");
    if (auto a = o.get("attempt")) path += "&attempt=" + *a;
    std::string text;
    ApiResponse r = c.client->get_text(path, &text);
    if (!r.ok()) return api_failure("logs", r, err);
    out << text;
    return kOk;
  });
}

int cmd_explain(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, kClientValues, kClientFlags);
  if (o.has("help")) return out << "Usage: coffee explain JOB TASK [--json] [connection options]\n\nExplains why a task is not running: blocking dependencies and, per Bean, why it does or does not fit.\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "explain", o.error);
  if (o.positional.size() != 2) return usage(err, "explain", "expected JOB and TASK");
  Connection c;
  if (int rc = connect(o, "explain", err, c)) return rc;
  return guarded("explain", err, [&] {
    ApiResponse r = c.client->get("/v1/jobs/" + o.positional[0] + "/tasks/" + o.positional[1] + "/explain");
    if (!r.ok()) return api_failure("explain", r, err);
    if (o.has("json")) return out << r.body.dump() << "\n", kOk;
    out << r.body["job_id"].get<std::string>() << "/" << r.body["task_id"].get<std::string>() << ": " << r.body["summary"].get<std::string>() << "\n";
    for (const auto& b : r.body["blocked_by"]) out << "  waiting for: " << b.get<std::string>() << "\n";
    for (const auto& p : r.body["placement"]) {
      out << "  " << p["bean_id"].get<std::string>() << ": " << (p["fits"].get<bool>() ? "fits" : "does not fit");
      for (const auto& why : p["reasons"]) out << "; " << why.get<std::string>();
      out << "\n";
    }
    return kOk;
  });
}

// ---------------------------------------------------------------- plan

int cmd_plan(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, kClientValues, kClientFlags);
  if (o.has("help"))
    return out << "Usage: coffee plan FILE [--json] [connection options]\n\nValidates FILE and prints the dependency layers (tasks of one layer may run in parallel). With --server (or\n$COFFEE_SERVER) and credentials it also checks each task against the registered Beans' capacity.\n" << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "plan", o.error);
  if (o.positional.size() != 1) return usage(err, "plan", "expected exactly one FILE");
  return guarded("plan", err, [&] {
    json w = parse_workload(o.positional[0]);
    const json& tasks = w["spec"]["tasks"];
    std::map<std::string, int> level;
    int max_level = 0;
    for (bool changed = true; changed;) {  // longest-path layering; the graph is acyclic (validated)
      changed = false;
      for (const auto& t : tasks) {
        int l = 0;
        for (const auto& d : t["depends_on"]) l = std::max(l, level[d.get<std::string>()] + 1);
        if (level.count(t["id"].get<std::string>()) == 0 || level[t["id"].get<std::string>()] != l) {
          level[t["id"].get<std::string>()] = l;
          changed = true;
        }
        max_level = std::max(max_level, l);
      }
    }
    std::optional<json> beans;
    bool want_server = o.has("server") || std::getenv("COFFEE_SERVER");
    if (want_server) {
      Connection c;
      if (int rc = connect(o, "plan", err, c)) return rc;
      ApiResponse r = c.client->get("/v1/beans");
      if (!r.ok()) return api_failure("plan", r, err);
      beans = r.body["beans"];
    }
    json layers = json::array();
    for (int l = 0; l <= max_level; l++) {
      json layer = json::array();
      for (const auto& t : tasks) {
        if (level[t["id"].get<std::string>()] != l) continue;
        json e = {{"id", t["id"]}, {"depends_on", t["depends_on"]}, {"resources", t["resources"]}, {"timeout_seconds", t["timeout_seconds"]}, {"max_attempts", t["max_attempts"]}};
        if (beans) {
          bool fits = false;
          for (const auto& b : *beans) fits = fits || (b["cpus"].get<int64_t>() >= t["resources"]["cpus"].get<int64_t>() && b["memory_mb"].get<int64_t>() >= t["resources"]["memory_mb"].get<int64_t>());
          e["placeable"] = fits;
        }
        layer.push_back(std::move(e));
      }
      layers.push_back(std::move(layer));
    }
    if (o.has("json")) return out << json{{"workload", w["metadata"]["name"]}, {"layers", layers}, {"beans_checked", beans.has_value()}}.dump() << "\n", kOk;
    out << "workload " << w["metadata"]["name"].get<std::string>() << ": " << tasks.size() << " task(s) in " << layers.size() << " layer(s)\n";
    for (size_t l = 0; l < layers.size(); l++) {
      out << "layer " << l << ":\n";
      for (const auto& t : layers[l]) {
        out << "  " << t["id"].get<std::string>() << "  cpus=" << t["resources"]["cpus"] << " memory_mb=" << t["resources"]["memory_mb"] << " timeout=" << t["timeout_seconds"] << "s max_attempts=" << t["max_attempts"];
        if (!t["depends_on"].empty()) {
          out << "  after:";
          for (const auto& d : t["depends_on"]) out << " " << d.get<std::string>();
        }
        if (t.contains("placeable") && !t["placeable"].get<bool>()) out << "  [cannot be placed: no registered Bean has this capacity]";
        out << "\n";
      }
    }
    if (!beans) out << "(offline plan: Bean capacity not checked; pass --server to check)\n";
    return kOk;
  });
}

// ---------------------------------------------------------------- brew

const char* kBrewHelp = R"(Usage: coffee brew FILE [--job-id ID] [--wait] [--timeout SECONDS] [--out DIR] [connection options]

Uploads the workload's input files (content-addressed, digest-verified) and submits the job. Resubmitting the same
--job-id with an identical workload is a no-op. With --wait, polls until the job is terminal: exit 0 if it
succeeded, 1 if it failed or was cancelled, 5 if --timeout (default 3600) elapsed first. --out DIR (needs --wait)
downloads every accepted output to DIR/TASK/PATH after the job finishes.
)";

int download_outputs(Client& client, const std::string& job_id, const json& job, const fs::path& dir, std::ostream& err) {
  std::error_code mkdir_ec;
  fs::create_directories(dir, mkdir_ec);
  if (mkdir_ec) {
    err << "coffee brew: cannot create output directory " << dir.string() << ": " << mkdir_ec.message() << "\n";
    return kFailed;
  }
  for (const auto& t : job["tasks"]) {
    if (t["state"] != "succeeded" || !t["result"].is_array()) continue;
    for (const auto& o : t["result"]) {
      const std::string rel = t["id"].get<std::string>() + "/" + o["path"].get<std::string>();
      int fd = create_confined_file(dir, rel, 0600);
      std::string sha_seen;
      Sha256Stream hash;
      uint64_t got = 0;
      ApiResponse r = client.get_stream("/v1/jobs/" + job_id + "/tasks/" + t["id"].get<std::string>() + "/outputs?path=" + percent_encode(o["path"].get<std::string>()),
                                        [&](const char* d, size_t n) {
                                          hash.update(d, n);
                                          got += n;
                                          for (size_t off = 0; off < n;) {
                                            ssize_t w = ::write(fd, d + off, n - off);
                                            if (w < 0) return false;
                                            off += static_cast<size_t>(w);
                                          }
                                          return true;
                                        });
      ::close(fd);
      if (!r.ok()) {
        err << "coffee brew: cannot download " << rel << ": " << r.error_code << "\n";
        return kFailed;
      }
      if (got != o["size"].get<uint64_t>() || hash.finish() != o["sha256"].get<std::string>()) {
        err << "coffee brew: downloaded " << rel << " failed digest verification\n";
        return kFailed;
      }
    }
  }
  return kOk;
}

int cmd_brew(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Options o = parse_options(args, with(kClientValues, {"job-id", "timeout", "out", "poll-ms"}), with(kClientFlags, {"wait"}));
  if (o.has("help")) return out << kBrewHelp << kConnectionHelp, kOk;
  if (!o.error.empty()) return usage(err, "brew", o.error);
  if (o.positional.size() != 1) return usage(err, "brew", "expected exactly one FILE");
  if (o.get("out") && !o.has("wait")) return usage(err, "brew", "--out requires --wait");
  int64_t timeout_s = 3600, poll_ms = 250;
  try {
    if (auto v = o.get("timeout")) timeout_s = parse_int("--timeout", *v, 1, 31'536'000);
    if (auto v = o.get("poll-ms")) poll_ms = parse_int("--poll-ms", *v, 20, 60'000);
  } catch (const Error& e) {
    return usage(err, "brew", e.what());
  }
  Connection c;
  if (int rc = connect(o, "brew", err, c)) return rc;
  return guarded("brew", err, [&] {
    json workload = parse_workload(o.positional[0]);  // full local validation, including that every input source exists
    fs::path root = fs::absolute(o.positional[0]).parent_path();
    ApiResponse ver = c.client->get("/v1/version");
    if (!ver.ok()) return api_failure("brew", ver, err);
    const uint64_t max_bytes = ver.body.value("max_artifact_bytes", protocol::kDefaultMaxArtifactBytes);
    json files = json::object();
    for (const auto& t : workload["spec"]["tasks"])
      for (const auto& in : t["inputs"]) {
        if (!in.contains("source")) continue;
        const std::string src = in["source"].get<std::string>();
        if (files.contains(src)) continue;
        uint64_t size = 0;
        int fd = open_confined_regular(root, src, &size);
        struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
        if (size > max_bytes) throw Error("too_large", "input '" + src + "' is " + std::to_string(size) + " bytes, above the coordinator limit of " + std::to_string(max_bytes));
        FileDigest d = digest_confined_regular(root, src, max_bytes);
        ApiResponse up = c.client->put_fd("/v1/inputs/" + d.sha256, fd, size);
        if (!up.ok()) return api_failure("brew", up, err);
        files[src] = {{"sha256", d.sha256}, {"size", d.size}};
      }
    json body = {{"workload", workload}, {"files", files}};
    if (auto id = o.get("job-id")) body["job_id"] = *id;
    ApiResponse r = c.client->post("/v1/jobs", body);
    if (!r.ok()) return api_failure("brew", r, err);
    const std::string job_id = r.body["job_id"].get<std::string>();
    if (!o.has("wait")) {
      if (o.has("json")) out << r.body.dump() << "\n";
      else out << "submitted job " << job_id << " (" << r.body["task_count"] << " task(s)" << (r.body["created"].get<bool>() ? "" : ", already existed") << ")\n";
      return kOk;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    for (;;) {
      ApiResponse st = c.client->get("/v1/jobs/" + job_id);
      if (!st.ok()) return api_failure("brew", st, err);
      const std::string state = st.body["state"].get<std::string>();
      if (state == "succeeded" || state == "failed" || state == "cancelled") {
        int rc = state == "succeeded" ? kOk : kFailed;
        if (rc == kOk && o.get("out")) {
          rc = download_outputs(*c.client, job_id, st.body, *o.get("out"), err);
        }
        if (o.has("json")) out << st.body.dump() << "\n";
        else {
          print_job(st.body, out);
          if (rc == kOk && o.get("out")) out << "outputs written to " << *o.get("out") << "\n";
        }
        return rc;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        err << "coffee brew: job " << job_id << " is still " << state << " after " << timeout_s << "s (it keeps running; see 'coffee status " << job_id << "')\n";
        return kTimeout;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
  });
}

}  // namespace

bool run_net_command(const std::string& cmd, const std::vector<std::string>& args, std::ostream& out, std::ostream& err, int& status) {
  struct Entry {
    const char* name;
    int (*fn)(const std::vector<std::string>&, std::ostream&, std::ostream&);
  };
  static const Entry table[] = {{"serve", cmd_serve}, {"join", cmd_join}, {"beans", cmd_beans}, {"plan", cmd_plan}, {"brew", cmd_brew},
                                {"status", cmd_status}, {"logs", cmd_logs}, {"explain", cmd_explain}, {"cancel", cmd_cancel}, {"drain", cmd_drain}};
  for (const auto& e : table)
    if (cmd == e.name) {
      status = e.fn(args, out, err);
      return true;
    }
  return false;
}

}  // namespace coffee::cli
