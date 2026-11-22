#include "coffee/store.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <variant>

namespace coffee {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 2;
constexpr int kMaxBeanCpus = 4096;

// ------------------------------------------------------------------ small SQLite wrapper

using Val = std::variant<std::nullptr_t, int64_t, std::string>;

struct Row {
  std::vector<Val> v;
  bool null(size_t i) const { return std::holds_alternative<std::nullptr_t>(v.at(i)); }
  int64_t i(size_t k) const { return null(k) ? 0 : std::get<int64_t>(v.at(k)); }
  std::string s(size_t k) const { return null(k) ? std::string() : std::get<std::string>(v.at(k)); }
  json j(size_t k) const { return null(k) ? json(nullptr) : json::parse(std::get<std::string>(v.at(k))); }
};

[[noreturn]] void db_fail(sqlite3* db, const std::string& what) {
  throw Error("db_error", what + ": " + (db ? sqlite3_errmsg(db) : "no database"));
}

class Db {
 public:
  Db(const fs::path& path, int busy_ms) {
    // Pre-create the database private (SQLite would use 0666 & ~umask); an empty file is a valid empty database.
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) ::close(fd);
    if (sqlite3_open_v2(path.c_str(), &h_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      std::string msg = h_ ? sqlite3_errmsg(h_) : "out of memory";
      if (h_) sqlite3_close(h_);
      throw Error("db_error", "cannot open " + path.string() + ": " + msg);
    }
    sqlite3_busy_timeout(h_, busy_ms);
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA synchronous=FULL");
    auto mode = query("PRAGMA journal_mode=WAL");  // persistent; falls back silently on exotic filesystems
    (void)mode;
  }
  ~Db() {
    if (h_) sqlite3_close(h_);
  }
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string msg = err ? err : "unknown error";
      sqlite3_free(err);
      throw Error("db_error", msg + " [" + sql.substr(0, 60) + "]");
    }
  }

  std::vector<Row> query(const std::string& sql, const std::vector<Val>& args = {}) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) db_fail(h_, "prepare");
    struct Guard {
      sqlite3_stmt* s;
      ~Guard() { sqlite3_finalize(s); }
    } guard{st};
    for (size_t k = 0; k < args.size(); k++) {
      int idx = static_cast<int>(k) + 1, rc;
      if (std::holds_alternative<std::nullptr_t>(args[k])) rc = sqlite3_bind_null(st, idx);
      else if (std::holds_alternative<int64_t>(args[k])) rc = sqlite3_bind_int64(st, idx, std::get<int64_t>(args[k]));
      else {
        const auto& s = std::get<std::string>(args[k]);
        rc = sqlite3_bind_text(st, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
      }
      if (rc != SQLITE_OK) db_fail(h_, "bind");
    }
    std::vector<Row> rows;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      Row r;
      int n = sqlite3_column_count(st);
      for (int c = 0; c < n; c++) {
        switch (sqlite3_column_type(st, c)) {
          case SQLITE_INTEGER: r.v.emplace_back(static_cast<int64_t>(sqlite3_column_int64(st, c))); break;
          case SQLITE_NULL: r.v.emplace_back(nullptr); break;
          default: {
            const unsigned char* t = sqlite3_column_text(st, c);
            r.v.emplace_back(std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(st, c))));
          }
        }
      }
      rows.push_back(std::move(r));
    }
    if (rc != SQLITE_DONE) db_fail(h_, "step");
    return rows;
  }

  int64_t run(const std::string& sql, const std::vector<Val>& args = {}) {
    query(sql, args);
    return sqlite3_changes(h_);
  }

 private:
  sqlite3* h_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back on destruction unless committed.
class Tx {
 public:
  explicit Tx(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
  ~Tx() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    db_.exec("COMMIT");
    done_ = true;
  }

 private:
  Db& db_;
  bool done_ = false;
};

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS beans (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, cpus INTEGER NOT NULL, memory_mb INTEGER NOT NULL,
  attributes TEXT NOT NULL, draining INTEGER NOT NULL DEFAULT 0,
  registered_at_ms INTEGER NOT NULL, last_heartbeat_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS jobs (
  seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, name TEXT NOT NULL, workload TEXT NOT NULL,
  state TEXT NOT NULL, cancel_reason TEXT, created_at_ms INTEGER NOT NULL, finished_at_ms INTEGER);
CREATE TABLE IF NOT EXISTS job_files (
  job_id TEXT NOT NULL REFERENCES jobs(id), source TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL,
  PRIMARY KEY (job_id, source));
CREATE TABLE IF NOT EXISTS tasks (
  job_id TEXT NOT NULL REFERENCES jobs(id), task_id TEXT NOT NULL, idx INTEGER NOT NULL, spec TEXT NOT NULL,
  depends_on TEXT NOT NULL, state TEXT NOT NULL, cpus INTEGER NOT NULL, memory_mb INTEGER NOT NULL,
  timeout_seconds INTEGER NOT NULL, max_attempts INTEGER NOT NULL, attempts_used INTEGER NOT NULL DEFAULT 0,
  failure_reason TEXT, accepted_attempt INTEGER, result TEXT, updated_at_ms INTEGER NOT NULL,
  PRIMARY KEY (job_id, task_id));
CREATE INDEX IF NOT EXISTS tasks_state ON tasks(state);
CREATE TABLE IF NOT EXISTS attempts (
  lease_id TEXT PRIMARY KEY, job_id TEXT NOT NULL, task_id TEXT NOT NULL, attempt_no INTEGER NOT NULL,
  bean_id TEXT NOT NULL REFERENCES beans(id), state TEXT NOT NULL, cpus INTEGER NOT NULL, memory_mb INTEGER NOT NULL,
  claimed_at_ms INTEGER NOT NULL, lease_expires_at_ms INTEGER NOT NULL, deadline_at_ms INTEGER NOT NULL,
  finished_at_ms INTEGER, exit_code INTEGER, reason TEXT, outputs TEXT,
  UNIQUE (job_id, task_id, attempt_no),
  FOREIGN KEY (job_id, task_id) REFERENCES tasks(job_id, task_id));
CREATE UNIQUE INDEX IF NOT EXISTS attempts_one_running ON attempts(job_id, task_id) WHERE state = 'running';
CREATE INDEX IF NOT EXISTS attempts_bean_state ON attempts(bean_id, state);
)SQL";

bool valid_id(const std::string& s) {
  if (s.empty() || s.size() > 63) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  return s[0] != '.' && s[0] != '-';
}

json nullable(const Row& r, size_t k) { return r.null(k) ? json(nullptr) : json(r.s(k)); }
json nullable_int(const Row& r, size_t k) { return r.null(k) ? json(nullptr) : json(r.i(k)); }

bool terminal(const std::string& s) { return s == "succeeded" || s == "failed" || s == "cancelled"; }

// ------------------------------------------------------------------ state directory helpers

// Create `path` with `data`/`mode` only if it does not exist (atomic, never overwrites).
bool create_exclusive(const fs::path& path, const std::string& data, unsigned mode) {
  fs::path tmp = path;
  tmp += ".new-" + random_hex(6);
  write_file_atomic(tmp, data, mode);
  int rc = ::link(tmp.c_str(), path.c_str());
  int err = errno;
  ::unlink(tmp.c_str());
  if (rc == 0) return true;
  if (err == EEXIST) return false;
  throw Error("io_error", "cannot create " + path.string() + ": " + std::strerror(err));
}

std::string read_cluster_id(const fs::path& dir) {
  fs::path cfg = dir / "config.json";
  std::string text = read_file_limited(cfg, 1 << 16);
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception&) {
    throw Error("conflict", cfg.string() + " is not valid JSON; refusing to overwrite it");
  }
  if (!j.is_object() || !j.contains("cluster_id") || !j["cluster_id"].is_string() || j["cluster_id"].get<std::string>().empty())
    throw Error("conflict", cfg.string() + " has no cluster_id; refusing to overwrite it");
  return j["cluster_id"].get<std::string>();
}

}  // namespace

// ------------------------------------------------------------------ public free functions

const char* to_string(LeaseStatus s) noexcept {
  switch (s) {
    case LeaseStatus::Ok: return "ok";
    case LeaseStatus::UnknownLease: return "unknown_lease";
    case LeaseStatus::WrongBean: return "wrong_bean";
    case LeaseStatus::NotRunning: return "not_running";
    case LeaseStatus::Expired: return "expired";
    case LeaseStatus::DeadlineExceeded: return "deadline_exceeded";
    case LeaseStatus::Cancelled: return "cancelled";
  }
  return "unknown";
}

json BeanInfo::to_json() const {
  return {{"id", id}, {"name", name}, {"cpus", cpus}, {"memory_mb", memory_mb}, {"cpus_used", cpus_used},
          {"memory_mb_used", memory_mb_used}, {"state", state}, {"draining", draining},
          {"registered_at_ms", registered_at_ms}, {"last_heartbeat_ms", last_heartbeat_ms}, {"attributes", attributes}};
}

InitResult init_state_dir(const fs::path& state_dir) {
  InitResult r;
  r.state_dir = state_dir;
  r.token_path = state_dir / "token";
  r.database_path = state_dir / "coffee.db";
  std::error_code ec;
  if (fs::exists(state_dir, ec) && !fs::is_directory(state_dir, ec)) throw Error("io_error", state_dir.string() + " exists and is not a directory");
  bool dir_new = !fs::exists(state_dir, ec);
  if (dir_new) {
    fs::create_directories(state_dir, ec);
    if (ec) throw Error("io_error", "cannot create " + state_dir.string() + ": " + ec.message());
    fs::permissions(state_dir, fs::perms::owner_all, ec);
  }
  for (const char* sub : {"artifacts", "artifacts/inputs", "artifacts/outputs", "artifacts/logs", "artifacts/tmp"}) {
    fs::path p = state_dir / sub;
    if (!fs::exists(p, ec)) {
      fs::create_directory(p, ec);
      if (ec) throw Error("io_error", "cannot create " + p.string() + ": " + ec.message());
      fs::permissions(p, fs::perms::owner_all, ec);
    }
  }

  // Token: exists -> keep (tighten mode); else create exclusively with 0600.
  if (fs::exists(r.token_path, ec)) {
    struct stat st{};
    if (::stat(r.token_path.c_str(), &st) == 0 && (st.st_mode & 077) != 0) ::chmod(r.token_path.c_str(), 0600);
  } else {
    r.token_created = create_exclusive(r.token_path, random_hex(32), 0600);
  }
  if (fs::file_size(r.token_path, ec) == 0) throw Error("conflict", r.token_path.string() + " is empty; refusing to overwrite it");

  // Cluster identity.
  fs::path cfg = state_dir / "config.json";
  bool cfg_created = false;
  if (!fs::exists(cfg, ec)) {
    json j = {{"format", 1}, {"cluster_id", "cluster-" + random_hex(8)}, {"created_at_ms", SystemClock().now_ms()}, {"coffee_version", kVersion}};
    cfg_created = create_exclusive(cfg, j.dump(2) + "\n", 0600);
  }
  r.cluster_id = read_cluster_id(state_dir);  // validates existing files too

  r.database_created = !fs::exists(r.database_path, ec);
  Store::open(state_dir);  // creates/migrates the schema
  r.created = dir_new || cfg_created;
  return r;
}

std::string read_cluster_token(const fs::path& state_dir) {
  fs::path p = state_dir / "token";
  std::error_code ec;
  if (!fs::exists(p, ec)) throw Error("not_found", "no cluster token at " + p.string() + " (run 'coffee init --state DIR')");
  std::string t = read_file_limited(p, 4096);
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  return t;
}

// ------------------------------------------------------------------ Store

struct Store::Impl {
  fs::path dir;
  StoreOptions opts;
  std::shared_ptr<const Clock> clock;
  std::string cluster_id;
  std::unique_ptr<Db> db;
  std::mutex mu;

  int64_t now() const { return clock->now_ms(); }

  // ---- task/attempt transitions (call inside a transaction)

  void end_attempt(const std::string& lease_id, const std::string& state, int64_t now_ms, const std::string& reason, const json& exit_code = nullptr,
                   const json& outputs = nullptr) {
    db->run("UPDATE attempts SET state=?, finished_at_ms=?, reason=?, exit_code=?, outputs=? WHERE lease_id=? AND state='running'",
            {state, now_ms, reason.empty() ? Val(nullptr) : Val(reason), exit_code.is_null() ? Val(nullptr) : Val(exit_code.get<int64_t>()),
             outputs.is_null() ? Val(nullptr) : Val(outputs.dump()), lease_id});
  }

  // After a running attempt ended unsuccessfully: retry while attempts remain, else fail the task.
  void requeue_or_fail(const std::string& job_id, const std::string& task_id, const std::string& reason, bool retryable, int64_t now_ms) {
    auto rows = db->query("SELECT state, attempts_used, max_attempts FROM tasks WHERE job_id=? AND task_id=?", {job_id, task_id});
    if (rows.empty() || rows[0].s(0) != "running") return;
    int64_t used = rows[0].i(1), max = rows[0].i(2);
    if (retryable && used < max) {
      db->run("UPDATE tasks SET state='pending', updated_at_ms=? WHERE job_id=? AND task_id=?", {now_ms, job_id, task_id});
    } else {
      std::string why = retryable ? "max attempts exhausted (" + std::to_string(used) + "/" + std::to_string(max) + "): " + reason
                                  : "non-retryable failure: " + reason;
      db->run("UPDATE tasks SET state='failed', failure_reason=?, updated_at_ms=? WHERE job_id=? AND task_id=?", {why, now_ms, job_id, task_id});
    }
    settle_job(job_id, now_ms);
  }

  // Propagate dependency failure/cancellation and recompute the job state.
  void settle_job(const std::string& job_id, int64_t now_ms) {
    auto tasks = db->query("SELECT task_id, state, depends_on FROM tasks WHERE job_id=? ORDER BY idx", {job_id});
    std::map<std::string, std::string> state;
    for (auto& t : tasks) state[t.s(0)] = t.s(1);
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& t : tasks) {
        if (state[t.s(0)] != "pending") continue;
        std::string bad_failed, bad_cancelled;
        for (auto& d : t.j(2)) {
          const std::string& ds = state[d.get<std::string>()];
          if (ds == "failed" && bad_failed.empty()) bad_failed = d.get<std::string>();
          if (ds == "cancelled" && bad_cancelled.empty()) bad_cancelled = d.get<std::string>();
        }
        if (bad_failed.empty() && bad_cancelled.empty()) continue;
        bool failed = !bad_failed.empty();
        state[t.s(0)] = failed ? "failed" : "cancelled";
        db->run("UPDATE tasks SET state=?, failure_reason=?, updated_at_ms=? WHERE job_id=? AND task_id=?",
                {failed ? "failed" : "cancelled", (failed ? "dependency_failed: " + bad_failed : "dependency_cancelled: " + bad_cancelled), now_ms, job_id, t.s(0)});
        changed = true;
      }
    }
    auto job = db->query("SELECT state FROM jobs WHERE id=?", {job_id});
    if (job.empty()) return;
    std::string cur = job[0].s(0);
    if (cur == "cancelled") return;
    size_t pending = 0, ok = 0, bad = 0, cancelled = 0;
    for (auto& [id, s] : state) {
      if (s == "pending") pending++;
      else if (s == "succeeded") ok++;
      else if (s == "failed") bad++;
      else if (s == "cancelled") cancelled++;
    }
    std::string next;
    if (pending + (state.size() - ok - bad - cancelled - pending) == 0)  // all terminal
      next = bad ? "failed" : cancelled ? "cancelled" : "succeeded";
    else
      next = pending == state.size() ? "pending" : "running";
    if (next != cur)
      db->run("UPDATE jobs SET state=?, finished_at_ms=? WHERE id=?", {next, terminal(next) ? Val(now_ms) : Val(nullptr), job_id});
  }

  int reap(int64_t now_ms) {
    auto due = db->query(
        "SELECT lease_id, job_id, task_id, lease_expires_at_ms <= ? FROM attempts "
        "WHERE state='running' AND (lease_expires_at_ms <= ? OR deadline_at_ms <= ?) ORDER BY claimed_at_ms, lease_id",
        {now_ms, now_ms, now_ms});
    for (auto& r : due) {
      bool lease_gone = r.i(3) != 0;
      std::string reason = lease_gone ? "lease expired" : "deadline exceeded";
      end_attempt(r.s(0), lease_gone ? "expired" : "timed_out", now_ms, reason);
      requeue_or_fail(r.s(1), r.s(2), reason, true, now_ms);
    }
    return static_cast<int>(due.size());
  }

  // ---- beans

  BeanInfo bean_info(const Row& r, int64_t now_ms) {
    BeanInfo b;
    b.id = r.s(0);
    b.name = r.s(1);
    b.cpus = static_cast<int>(r.i(2));
    b.memory_mb = r.i(3);
    b.attributes = r.j(4);
    b.draining = r.i(5) != 0;
    b.registered_at_ms = r.i(6);
    b.last_heartbeat_ms = r.i(7);
    b.cpus_used = static_cast<int>(r.i(8));
    b.memory_mb_used = r.i(9);
    b.state = b.draining ? "draining" : (now_ms - b.last_heartbeat_ms > opts.heartbeat_timeout_ms ? "unreachable" : "online");
    return b;
  }

  static constexpr const char* kBeanSelect =
      "SELECT b.id, b.name, b.cpus, b.memory_mb, b.attributes, b.draining, b.registered_at_ms, b.last_heartbeat_ms, "
      "COALESCE((SELECT SUM(cpus) FROM attempts a WHERE a.bean_id=b.id AND a.state='running'),0), "
      "COALESCE((SELECT SUM(memory_mb) FROM attempts a WHERE a.bean_id=b.id AND a.state='running'),0) FROM beans b ";

  // Input manifest for a claim: local files by their recorded digests, dependency outputs from the producer's
  // accepted (immutable) result only.
  json resolve_inputs(const std::string& job_id, const json& task) {
    json out = json::array();
    for (const auto& in : task.value("inputs", json::array())) {
      json e = {{"target", in["target"]}, {"sha256", nullptr}, {"size", nullptr}};
      if (in.contains("from_task")) {
        e["kind"] = "dependency";
        e["from_task"] = in["from_task"];
        e["output"] = in["output"];
        auto r = db->query("SELECT result FROM tasks WHERE job_id=? AND task_id=? AND state='succeeded'", {job_id, in["from_task"].get<std::string>()});
        if (!r.empty())
          for (const auto& o : r[0].j(0))
            if (o["path"] == in["output"]) {
              e["sha256"] = o["sha256"];
              e["size"] = o["size"];
            }
      } else {
        e["kind"] = "file";
        e["source"] = in["source"];
        auto r = db->query("SELECT sha256, size FROM job_files WHERE job_id=? AND source=?", {job_id, in["source"].get<std::string>()});
        if (!r.empty()) {
          e["sha256"] = r[0].s(0);
          e["size"] = r[0].i(1);
        }
      }
      out.push_back(std::move(e));
    }
    return out;
  }

  // ---- leases

  LeaseResult check_lease(const std::string& lease_id, const std::string& bean_id, Row* attempt_out) {
    auto rows = db->query("SELECT a.state, a.bean_id, a.job_id, a.task_id, a.attempt_no, t.state, a.lease_expires_at_ms, a.reason FROM attempts a "
                          "JOIN tasks t ON t.job_id=a.job_id AND t.task_id=a.task_id WHERE a.lease_id=?",
                          {lease_id});
    LeaseResult res;
    if (rows.empty()) {
      res.status = LeaseStatus::UnknownLease;
      res.detail = "no such lease";
      return res;
    }
    const Row& r = rows[0];
    res.task_state = r.s(5);
    if (r.s(1) != bean_id) {
      res.status = LeaseStatus::WrongBean;
      res.detail = "lease belongs to a different Bean";
      res.task_state.clear();
      return res;
    }
    const std::string st = r.s(0);
    if (st == "running") {
      res.status = LeaseStatus::Ok;
      res.lease_expires_at_ms = r.i(6);
      if (attempt_out) *attempt_out = r;
    } else if (st == "expired") {
      res.status = LeaseStatus::Expired;
      res.detail = "lease expired (attempt " + std::to_string(r.i(4)) + " was superseded or re-queued)";
    } else if (st == "timed_out") {
      res.status = LeaseStatus::DeadlineExceeded;
      res.detail = "attempt exceeded its deadline";
    } else if (st == "cancelled") {
      res.status = LeaseStatus::Cancelled;
      res.detail = r.s(7).empty() ? "cancelled" : r.s(7);
    } else {
      res.status = LeaseStatus::NotRunning;
      res.detail = "attempt already " + st;
    }
    return res;
  }

  void cancel_running_attempts(const std::string& job_id, const std::string& task_id, int64_t now_ms, const std::string& reason, int* counter) {
    auto running = db->query("SELECT lease_id FROM attempts WHERE job_id=? AND state='running'" + std::string(task_id.empty() ? "" : " AND task_id=?"),
                             task_id.empty() ? std::vector<Val>{job_id} : std::vector<Val>{job_id, task_id});
    for (auto& r : running) {
      end_attempt(r.s(0), "cancelled", now_ms, reason);
      (*counter)++;
    }
  }
};

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::~Store() = default;

std::unique_ptr<Store> Store::open(const fs::path& state_dir, StoreOptions options) {
  auto impl = std::make_unique<Impl>();
  impl->dir = state_dir;
  impl->opts = options;
  impl->clock = options.clock ? options.clock : std::make_shared<SystemClock>();
  std::error_code ec;
  if (!fs::exists(state_dir / "config.json", ec)) throw Error("not_found", state_dir.string() + " is not an initialized Coffee state directory (run 'coffee init --state DIR')");
  impl->cluster_id = read_cluster_id(state_dir);
  impl->db = std::make_unique<Db>(state_dir / "coffee.db", options.busy_timeout_ms);
  {
    Tx tx(*impl->db);
    auto ver = impl->db->query("PRAGMA user_version");
    int64_t v = ver.empty() ? 0 : ver[0].i(0);
    if (v > kSchemaVersion) throw Error("db_error", "database schema version " + std::to_string(v) + " is newer than supported " + std::to_string(kSchemaVersion));
    impl->db->exec(kSchema);
    if (v < kSchemaVersion) impl->db->exec("PRAGMA user_version=" + std::to_string(kSchemaVersion));
    impl->db->run("INSERT OR IGNORE INTO meta(key,value) VALUES('cluster_id', ?)", {impl->cluster_id});
    auto stored = impl->db->query("SELECT value FROM meta WHERE key='cluster_id'");
    if (!stored.empty() && stored[0].s(0) != impl->cluster_id) throw Error("conflict", "database belongs to a different cluster than config.json");
    // Opening never touches lease timestamps: valid leases keep their original expiry, expired ones stay
    // expired and are recovered by ordinary serialized reaping on the next mutating call.
    tx.commit();
  }
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

const std::string& Store::cluster_id() const { return impl_->cluster_id; }
const fs::path& Store::state_dir() const { return impl_->dir; }
int64_t Store::now_ms() const { return impl_->now(); }

static bool valid_sha256(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

SubmitResult Store::submit(const json& workload, const std::string& job_id_in, const json& input_files) {
  json w = normalize_workload(workload);
  if (!input_files.is_object()) throw Error("invalid_argument", "input_files must be a JSON object");
  for (auto it = input_files.begin(); it != input_files.end(); ++it) {
    if (auto why = check_safe_relative_path(it.key())) throw Error("invalid_argument", "input file path '" + it.key() + "': " + *why);
    const json& f = it.value();
    if (!f.is_object() || !f.contains("sha256") || !f["sha256"].is_string() || !valid_sha256(f["sha256"].get<std::string>()) || !f.contains("size") ||
        !f["size"].is_number_integer() || f["size"].get<int64_t>() < 0)
      throw Error("invalid_argument", "input file '" + it.key() + "' needs {\"sha256\": <64 lowercase hex>, \"size\": <bytes>}");
  }
  std::string job_id = job_id_in.empty() ? "job-" + random_hex(6) : job_id_in;
  if (!valid_id(job_id)) throw Error("invalid_argument", "invalid job id '" + job_id + "' (use [A-Za-z0-9._-], at most 63 characters)");
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  SubmitResult res;
  res.job_id = job_id;
  res.task_count = w["spec"]["tasks"].size();
  auto existing = impl_->db->query("SELECT workload FROM jobs WHERE id=?", {job_id});
  if (!existing.empty()) {
    if (existing[0].j(0) != w) throw Error("conflict", "job '" + job_id + "' already exists with a different workload");
    json stored = json::object();
    for (auto& f : impl_->db->query("SELECT source, sha256, size FROM job_files WHERE job_id=?", {job_id})) stored[f.s(0)] = {{"sha256", f.s(1)}, {"size", f.i(2)}};
    if (stored != input_files) throw Error("conflict", "job '" + job_id + "' already exists with different input files");
    res.created = false;
    return res;
  }
  impl_->db->run("INSERT INTO jobs(id,name,workload,state,created_at_ms) VALUES(?,?,?,'pending',?)", {job_id, w["metadata"]["name"].get<std::string>(), w.dump(), now});
  for (auto it = input_files.begin(); it != input_files.end(); ++it)
    impl_->db->run("INSERT INTO job_files(job_id,source,sha256,size) VALUES(?,?,?,?)", {job_id, it.key(), it.value()["sha256"].get<std::string>(), it.value()["size"].get<int64_t>()});
  int64_t i = 0;
  for (const auto& t : w["spec"]["tasks"]) {
    impl_->db->run(
        "INSERT INTO tasks(job_id,task_id,idx,spec,depends_on,state,cpus,memory_mb,timeout_seconds,max_attempts,updated_at_ms) VALUES(?,?,?,?,?,'pending',?,?,?,?,?)",
        {job_id, t["id"].get<std::string>(), i++, t.dump(), t["depends_on"].dump(), t["resources"]["cpus"].get<int64_t>(),
         t["resources"]["memory_mb"].get<int64_t>(), t["timeout_seconds"].get<int64_t>(), t["max_attempts"].get<int64_t>(), now});
  }
  tx.commit();
  return res;
}

BeanInfo Store::register_bean(const BeanRegistration& reg) {
  if (reg.cpus < 1 || reg.cpus > kMaxBeanCpus) throw Error("invalid_argument", "bean cpus must be between 1 and " + std::to_string(kMaxBeanCpus));
  if (reg.memory_mb < 1) throw Error("invalid_argument", "bean memory_mb must be positive");
  if (!reg.attributes.is_object()) throw Error("invalid_argument", "bean attributes must be a JSON object");
  if (!reg.id.empty() && !valid_id(reg.id)) throw Error("invalid_argument", "invalid bean id '" + reg.id + "'");
  if (reg.name.size() > 128) throw Error("invalid_argument", "bean name longer than 128 bytes");
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  std::string id = reg.id.empty() ? "bean-" + random_hex(4) : reg.id;
  std::string name = reg.name.empty() ? id : reg.name;
  impl_->reap(now);
  auto existing = impl_->db->query("SELECT id FROM beans WHERE id=?", {id});
  if (existing.empty()) {
    impl_->db->run("INSERT INTO beans(id,name,cpus,memory_mb,attributes,registered_at_ms,last_heartbeat_ms) VALUES(?,?,?,?,?,?,?)",
                   {id, name, static_cast<int64_t>(reg.cpus), reg.memory_mb, reg.attributes.dump(), now, now});
  } else {
    // A re-registering Bean is a new process: whatever the old one was running is abandoned.
    auto running = impl_->db->query("SELECT lease_id, job_id, task_id FROM attempts WHERE bean_id=? AND state='running'", {id});
    for (auto& r : running) {
      impl_->end_attempt(r.s(0), "abandoned", now, "bean re-registered");
      impl_->requeue_or_fail(r.s(1), r.s(2), "bean re-registered", true, now);
    }
    impl_->db->run("UPDATE beans SET name=?, cpus=?, memory_mb=?, attributes=?, last_heartbeat_ms=? WHERE id=?",
                   {name, static_cast<int64_t>(reg.cpus), reg.memory_mb, reg.attributes.dump(), now, id});
  }
  auto rows = impl_->db->query(std::string(Impl::kBeanSelect) + "WHERE b.id=?", {id});
  BeanInfo info = impl_->bean_info(rows.at(0), now);
  tx.commit();
  return info;
}

bool Store::heartbeat(const std::string& bean_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  impl_->reap(now);
  bool known = impl_->db->run("UPDATE beans SET last_heartbeat_ms=? WHERE id=?", {now, bean_id}) > 0;
  tx.commit();
  return known;
}

bool Store::set_draining(const std::string& bean_id, bool draining) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  bool known = impl_->db->run("UPDATE beans SET draining=? WHERE id=?", {static_cast<int64_t>(draining ? 1 : 0), bean_id}) > 0;
  tx.commit();
  return known;
}

std::vector<BeanInfo> Store::list_beans() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  impl_->reap(now);
  std::vector<BeanInfo> out;
  for (auto& r : impl_->db->query(std::string(Impl::kBeanSelect) + "ORDER BY b.registered_at_ms, b.id")) out.push_back(impl_->bean_info(r, now));
  tx.commit();
  return out;
}

std::optional<Claim> Store::claim(const std::string& bean_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  Db& db = *impl_->db;
  int64_t now = impl_->now();
  impl_->reap(now);
  auto beans = db.query(std::string(Impl::kBeanSelect) + "WHERE b.id=?", {bean_id});
  if (beans.empty()) throw Error("not_found", "unknown Bean '" + bean_id + "'");
  BeanInfo bean = impl_->bean_info(beans[0], now);
  db.run("UPDATE beans SET last_heartbeat_ms=? WHERE id=?", {now, bean_id});
  if (bean.draining) {
    tx.commit();
    return std::nullopt;
  }
  int64_t free_cpus = bean.cpus - bean.cpus_used, free_mem = bean.memory_mb - bean.memory_mb_used;

  auto candidates = db.query(
      "SELECT t.job_id, t.task_id, t.cpus, t.memory_mb, t.depends_on, t.spec, t.timeout_seconds FROM tasks t JOIN jobs j ON j.id=t.job_id "
      "WHERE t.state='pending' AND j.state IN ('pending','running') ORDER BY j.seq, t.idx");
  std::map<std::string, std::map<std::string, std::string>> job_states;
  for (auto& c : candidates) {
    if (c.i(2) > free_cpus || c.i(3) > free_mem) continue;
    auto& states = job_states[c.s(0)];
    if (states.empty())
      for (auto& s : db.query("SELECT task_id, state FROM tasks WHERE job_id=?", {c.s(0)})) states[s.s(0)] = s.s(1);
    bool ready = true;
    for (auto& d : c.j(4)) ready = ready && states[d.get<std::string>()] == "succeeded";
    if (!ready) continue;

    Claim claim;
    claim.job_id = c.s(0);
    claim.task_id = c.s(1);
    claim.bean_id = bean_id;
    claim.cpus = static_cast<int>(c.i(2));
    claim.memory_mb = c.i(3);
    claim.timeout_seconds = static_cast<int>(c.i(6));
    claim.task = c.j(5);
    claim.inputs = impl_->resolve_inputs(claim.job_id, claim.task);
    claim.lease_id = "lease-" + random_hex(24);
    claim.lease_expires_at_ms = now + impl_->opts.lease_ms;
    claim.deadline_at_ms = now + c.i(6) * 1000 + impl_->opts.deadline_grace_ms;
    auto n = db.query("SELECT attempts_used + 1 FROM tasks WHERE job_id=? AND task_id=?", {claim.job_id, claim.task_id});
    claim.attempt = static_cast<int>(n.at(0).i(0));
    db.run("INSERT INTO attempts(lease_id,job_id,task_id,attempt_no,bean_id,state,cpus,memory_mb,claimed_at_ms,lease_expires_at_ms,deadline_at_ms) "
           "VALUES(?,?,?,?,?,'running',?,?,?,?,?)",
           {claim.lease_id, claim.job_id, claim.task_id, static_cast<int64_t>(claim.attempt), bean_id, static_cast<int64_t>(claim.cpus), claim.memory_mb, now,
            claim.lease_expires_at_ms, claim.deadline_at_ms});
    db.run("UPDATE tasks SET state='running', attempts_used=attempts_used+1, updated_at_ms=? WHERE job_id=? AND task_id=?", {now, claim.job_id, claim.task_id});
    impl_->settle_job(claim.job_id, now);
    tx.commit();
    return claim;
  }
  tx.commit();
  return std::nullopt;
}

LeaseResult Store::renew(const std::string& lease_id, const std::string& bean_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  impl_->reap(now);
  LeaseResult res = impl_->check_lease(lease_id, bean_id, nullptr);
  if (res.ok()) {
    res.lease_expires_at_ms = now + impl_->opts.lease_ms;
    impl_->db->run("UPDATE attempts SET lease_expires_at_ms=? WHERE lease_id=?", {res.lease_expires_at_ms, lease_id});
    impl_->db->run("UPDATE beans SET last_heartbeat_ms=? WHERE id=?", {now, bean_id});
  }
  tx.commit();
  return res;
}

LeaseResult Store::complete(const CompleteRequest& req) {
  if (!req.outputs.is_array()) throw Error("invalid_argument", "outputs must be a JSON array");
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int64_t now = impl_->now();
  impl_->reap(now);
  Row attempt;
  LeaseResult res = impl_->check_lease(req.lease_id, req.bean_id, &attempt);
  if (!res.ok()) {
    tx.commit();  // keep the reaping side effects
    return res;
  }
  const std::string job_id = attempt.s(2), task_id = attempt.s(3);
  if (req.success) {
    if (req.before_commit) {
      req.before_commit();
      // The hook may have been slow (artifact publication): re-read the clock and revalidate the lease and
      // deadline before accepting. On rejection nothing accepted is recorded; published files stay unreferenced.
      now = impl_->now();
      impl_->reap(now);
      res = impl_->check_lease(req.lease_id, req.bean_id, &attempt);
      if (!res.ok()) {
        tx.commit();  // keep the expiry/timeout transition and the released reservation
        return res;
      }
    }
    impl_->end_attempt(req.lease_id, "succeeded", now, "", static_cast<int64_t>(req.exit_code), req.outputs);
    impl_->db->run("UPDATE tasks SET state='succeeded', accepted_attempt=?, result=?, failure_reason=NULL, updated_at_ms=? WHERE job_id=? AND task_id=?",
                   {attempt.i(4), req.outputs.dump(), now, job_id, task_id});
    impl_->settle_job(job_id, now);
  } else {
    std::string reason = req.reason.empty() ? "attempt failed" : req.reason;
    impl_->end_attempt(req.lease_id, "failed", now, reason, static_cast<int64_t>(req.exit_code));
    impl_->requeue_or_fail(job_id, task_id, reason, req.retryable, now);
  }
  res.task_state = impl_->db->query("SELECT state FROM tasks WHERE job_id=? AND task_id=?", {job_id, task_id}).at(0).s(0);
  tx.commit();
  return res;
}

std::optional<LeaseInfo> Store::lease_info(const std::string& lease_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  auto r = impl_->db->query("SELECT a.job_id, a.task_id, a.bean_id, a.attempt_no, a.state, a.lease_expires_at_ms, a.deadline_at_ms, t.spec "
                            "FROM attempts a JOIN tasks t ON t.job_id=a.job_id AND t.task_id=a.task_id WHERE a.lease_id=?",
                            {lease_id});
  if (r.empty()) return std::nullopt;
  LeaseInfo info;
  info.job_id = r[0].s(0);
  info.task_id = r[0].s(1);
  info.bean_id = r[0].s(2);
  info.attempt = static_cast<int>(r[0].i(3));
  info.state = r[0].s(4);
  info.lease_expires_at_ms = r[0].i(5);
  info.deadline_at_ms = r[0].i(6);
  info.task = r[0].j(7);
  info.inputs = impl_->resolve_inputs(info.job_id, info.task);
  return info;
}

LeaseResult Store::lease_status(const std::string& lease_id, const std::string& bean_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  int64_t now = impl_->now();
  Row attempt;
  LeaseResult res = impl_->check_lease(lease_id, bean_id, &attempt);
  if (res.ok() && attempt.i(6) <= now) {
    res.status = LeaseStatus::Expired;
    res.detail = "lease expired";
  } else if (res.ok()) {
    auto d = impl_->db->query("SELECT deadline_at_ms FROM attempts WHERE lease_id=?", {lease_id});
    if (!d.empty() && d[0].i(0) <= now) {
      res.status = LeaseStatus::DeadlineExceeded;
      res.detail = "attempt exceeded its deadline";
    }
  }
  return res;
}

bool Store::lease_valid(const std::string& lease_id, const std::string& bean_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  int64_t now = impl_->now();
  auto rows = impl_->db->query("SELECT 1 FROM attempts WHERE lease_id=? AND bean_id=? AND state='running' AND lease_expires_at_ms>? AND deadline_at_ms>?",
                               {lease_id, bean_id, now, now});
  return !rows.empty();
}

CancelResult Store::cancel(const std::string& job_id, const std::string& task_id, const std::string& reason) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  Db& db = *impl_->db;
  int64_t now = impl_->now();
  impl_->reap(now);
  CancelResult res;
  auto job = db.query("SELECT state FROM jobs WHERE id=?", {job_id});
  if (job.empty()) {
    tx.commit();
    return res;
  }
  if (!task_id.empty() && db.query("SELECT 1 FROM tasks WHERE job_id=? AND task_id=?", {job_id, task_id}).empty()) {
    tx.commit();
    return res;
  }
  res.found = true;
  auto cancelled_count = [&] { return db.query("SELECT COUNT(*) FROM tasks WHERE job_id=? AND state='cancelled'", {job_id}).at(0).i(0); };
  int64_t before = cancelled_count();
  if (task_id.empty()) {
    if (!terminal(job[0].s(0))) {
      impl_->cancel_running_attempts(job_id, "", now, reason, &res.attempts_cancelled);
      db.run("UPDATE tasks SET state='cancelled', failure_reason=?, updated_at_ms=? WHERE job_id=? AND state IN ('pending','running')", {reason, now, job_id});
      db.run("UPDATE jobs SET state='cancelled', cancel_reason=?, finished_at_ms=? WHERE id=?", {reason, now, job_id});
    }
  } else {
    auto t = db.query("SELECT state FROM tasks WHERE job_id=? AND task_id=?", {job_id, task_id});
    if (!terminal(t.at(0).s(0))) {
      impl_->cancel_running_attempts(job_id, task_id, now, reason, &res.attempts_cancelled);
      db.run("UPDATE tasks SET state='cancelled', failure_reason=?, updated_at_ms=? WHERE job_id=? AND task_id=?", {reason, now, job_id, task_id});
      impl_->settle_job(job_id, now);
    }
  }
  res.tasks_cancelled = static_cast<int>(cancelled_count() - before);
  res.job_state = db.query("SELECT state FROM jobs WHERE id=?", {job_id}).at(0).s(0);
  tx.commit();
  return res;
}

int Store::reap_expired() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  int n = impl_->reap(impl_->now());
  tx.commit();
  return n;
}

std::optional<json> Store::job_status(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  Db& db = *impl_->db;
  impl_->reap(impl_->now());
  auto job = db.query("SELECT id, name, state, cancel_reason, created_at_ms, finished_at_ms FROM jobs WHERE id=?", {job_id});
  if (job.empty()) {
    tx.commit();
    return std::nullopt;
  }
  json out = {{"id", job[0].s(0)}, {"name", job[0].s(1)}, {"state", job[0].s(2)}, {"cancel_reason", nullable(job[0], 3)},
              {"created_at_ms", job[0].i(4)}, {"finished_at_ms", nullable_int(job[0], 5)}, {"tasks", json::array()}};
  for (auto& t : db.query("SELECT task_id, state, depends_on, cpus, memory_mb, timeout_seconds, max_attempts, attempts_used, failure_reason, accepted_attempt, result "
                          "FROM tasks WHERE job_id=? ORDER BY idx", {job_id})) {
    json jt = {{"id", t.s(0)}, {"state", t.s(1)}, {"depends_on", t.j(2)}, {"resources", {{"cpus", t.i(3)}, {"memory_mb", t.i(4)}}},
               {"timeout_seconds", t.i(5)}, {"max_attempts", t.i(6)}, {"attempts_used", t.i(7)}, {"failure_reason", nullable(t, 8)},
               {"accepted_attempt", nullable_int(t, 9)}, {"result", t.j(10)}, {"attempts", json::array()}};
    for (auto& a : db.query("SELECT attempt_no, bean_id, state, claimed_at_ms, lease_expires_at_ms, deadline_at_ms, finished_at_ms, exit_code, reason "
                            "FROM attempts WHERE job_id=? AND task_id=? ORDER BY attempt_no", {job_id, t.s(0)}))
      jt["attempts"].push_back({{"attempt", a.i(0)}, {"bean_id", a.s(1)}, {"state", a.s(2)}, {"claimed_at_ms", a.i(3)}, {"lease_expires_at_ms", a.i(4)},
                                {"deadline_at_ms", a.i(5)}, {"finished_at_ms", nullable_int(a, 6)}, {"exit_code", nullable_int(a, 7)}, {"reason", nullable(a, 8)}});
    out["tasks"].push_back(std::move(jt));
  }
  tx.commit();
  return out;
}

json Store::list_jobs() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  Db& db = *impl_->db;
  impl_->reap(impl_->now());
  json out = json::array();
  for (auto& j : db.query("SELECT id, name, state, created_at_ms, finished_at_ms FROM jobs ORDER BY seq")) {
    json counts = {{"total", 0}, {"pending", 0}, {"running", 0}, {"succeeded", 0}, {"failed", 0}, {"cancelled", 0}};
    for (auto& c : db.query("SELECT state, COUNT(*) FROM tasks WHERE job_id=? GROUP BY state", {j.s(0)})) {
      counts[c.s(0)] = c.i(1);
      counts["total"] = counts["total"].get<int64_t>() + c.i(1);
    }
    out.push_back({{"id", j.s(0)}, {"name", j.s(1)}, {"state", j.s(2)}, {"created_at_ms", j.i(3)}, {"finished_at_ms", nullable_int(j, 4)}, {"tasks", counts}});
  }
  tx.commit();
  return out;
}

std::optional<json> Store::explain_task(const std::string& job_id, const std::string& task_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Tx tx(*impl_->db);
  Db& db = *impl_->db;
  int64_t now = impl_->now();
  impl_->reap(now);
  auto t = db.query("SELECT state, depends_on, cpus, memory_mb, attempts_used, max_attempts, failure_reason FROM tasks WHERE job_id=? AND task_id=?", {job_id, task_id});
  if (t.empty()) {
    tx.commit();
    return std::nullopt;
  }
  json blocked = json::array();
  for (auto& d : t[0].j(1)) {
    auto ds = db.query("SELECT state FROM tasks WHERE job_id=? AND task_id=?", {job_id, d.get<std::string>()});
    if (ds.empty() || ds[0].s(0) != "succeeded") blocked.push_back(d);
  }
  int64_t need_cpus = t[0].i(2), need_mem = t[0].i(3);
  json placement = json::array();
  bool satisfiable = false;
  for (auto& r : db.query(std::string(Impl::kBeanSelect) + "ORDER BY b.registered_at_ms, b.id")) {
    BeanInfo b = impl_->bean_info(r, now);
    json reasons = json::array();
    if (need_cpus > b.cpus) reasons.push_back("needs cpus " + std::to_string(need_cpus) + " but Bean capacity is " + std::to_string(b.cpus));
    else if (need_cpus > b.cpus - b.cpus_used) reasons.push_back("needs cpus " + std::to_string(need_cpus) + " but only " + std::to_string(b.cpus - b.cpus_used) + " free");
    if (need_mem > b.memory_mb) reasons.push_back("needs memory_mb " + std::to_string(need_mem) + " but Bean capacity is " + std::to_string(b.memory_mb));
    else if (need_mem > b.memory_mb - b.memory_mb_used) reasons.push_back("needs memory_mb " + std::to_string(need_mem) + " but only " + std::to_string(b.memory_mb - b.memory_mb_used) + " free");
    if (b.draining) reasons.push_back("Bean is draining");
    if (b.state == "unreachable") reasons.push_back("Bean is unreachable (no recent heartbeat)");
    if (need_cpus <= b.cpus && need_mem <= b.memory_mb) satisfiable = true;
    placement.push_back({{"bean_id", b.id}, {"fits", reasons.empty()}, {"reasons", reasons}});
  }
  std::string state = t[0].s(0), summary;
  if (state != "pending") summary = "task is " + state;
  else if (!blocked.empty()) summary = "waiting for dependencies to succeed";
  else if (placement.empty()) summary = "no Beans are registered";
  else if (!satisfiable) summary = "no registered Bean has enough capacity for this task's resources";
  else summary = "eligible; waiting for a Bean with free resources to claim it";
  tx.commit();
  return json{{"job_id", job_id}, {"task_id", task_id}, {"state", state}, {"attempts_used", t[0].i(4)}, {"max_attempts", t[0].i(5)},
              {"failure_reason", nullable(t[0], 6)}, {"blocked_by", blocked}, {"placement", placement},
              {"unsatisfiable", state == "pending" && !satisfiable}, {"summary", summary}};
}

}  // namespace coffee
