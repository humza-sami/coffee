#include "coffee/worker.hpp"

#include <sched.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <list>
#include <mutex>
#include <set>
#include <thread>

#include "worker_internal.hpp"

namespace coffee {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::vector<int> allowed_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::vector<int> out;
  if (::sched_getaffinity(0, sizeof set, &set) == 0)
    for (int i = 0; i < CPU_SETSIZE; i++)
      if (CPU_ISSET(i, &set)) out.push_back(i);
  return out;
}

int64_t half_of_mem_total_mb() {
  std::ifstream in("/proc/meminfo");
  std::string key;
  int64_t kb = 0;
  std::string unit;
  while (in >> key >> kb >> unit)
    if (key == "MemTotal:") return std::max<int64_t>(64, kb / 2048);
  return 1024;
}

}  // namespace

struct Worker::Impl {
  WorkerOptions opts;
  std::unique_ptr<Client> client;
  std::string bean_id;
  std::atomic<bool> stopping{false};
  std::mutex log_mu;

  struct Running {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::vector<int> cpus;
  };
  std::list<Running> running;
  std::set<int> free_cpus;

  void log(const std::string& msg) {
    std::lock_guard<std::mutex> l(log_mu);
    std::fprintf(stderr, "coffee join [%s]: %s\n", bean_id.c_str(), msg.c_str());
    std::fflush(stderr);
  }

  void reap_finished() {
    for (auto it = running.begin(); it != running.end();) {
      if (it->done->load()) {
        it->thread.join();
        for (int c : it->cpus) free_cpus.insert(c);
        it = running.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool register_bean(int cpus, int64_t mem) {
    json reg = {{"id", bean_id}, {"name", opts.name.empty() ? bean_id : opts.name}, {"cpus", cpus}, {"memory_mb", mem},
                {"attributes", {{"pid", static_cast<int64_t>(::getpid())}, {"supervisor", "execed"}}}};
    for (int attempt = 0; !stopping.load(); attempt++) {
      try {
        ApiResponse r = client->post("/v1/beans", reg);
        if (r.ok()) return true;
        if (r.status == 401 || r.status == 400) {
          log("registration rejected: " + r.error_code + ": " + r.error_message);
          return false;
        }
        log("registration failed: " + r.error_code + "; retrying");
      } catch (const NetworkError& e) {
        if (attempt % 10 == 0) log(std::string("coordinator unreachable: ") + e.what() + "; retrying");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
  }
};

Worker::Worker(WorkerOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->opts = std::move(options);
  WorkerOptions& o = impl_->opts;
  if (o.work_dir.empty()) throw Error("invalid_argument", "--work-dir is required");
  std::error_code ec;
  fs::create_directories(o.work_dir / "attempts", ec);
  if (ec) throw Error("io_error", "cannot create " + o.work_dir.string() + ": " + ec.message());
  ::chmod(o.work_dir.c_str(), 0700);
  if (o.heartbeat_ms < 50) throw Error("invalid_argument", "--heartbeat-ms must be at least 50");
  impl_->client = std::make_unique<Client>(o.client);
  std::string id = o.bean_id;
  fs::path idfile = o.work_dir / "bean-id";
  if (id.empty()) {
    if (fs::exists(idfile, ec)) id = read_file_limited(idfile, 256);
    while (!id.empty() && (id.back() == '\n' || id.back() == ' ')) id.pop_back();
    if (id.empty()) id = "bean-" + random_hex(4);
  }
  write_file_atomic(idfile, id + "\n", 0600);
  impl_->bean_id = id;
}

Worker::~Worker() = default;
const std::string& Worker::bean_id() const { return impl_->bean_id; }
void Worker::request_stop() { impl_->stopping.store(true); }

int Worker::run() {
  Impl& m = *impl_;
  WorkerOptions& o = m.opts;
  std::vector<int> cpus = allowed_cpus();
  int slots = o.cpus > 0 ? o.cpus : static_cast<int>(cpus.size());
  if (slots < 1 || static_cast<size_t>(slots) > cpus.size()) {
    m.log("cannot advertise " + std::to_string(slots) + " CPU slots: this process may use only " + std::to_string(cpus.size()) + " CPUs");
    return 1;
  }
  int64_t mem = o.memory_mb > 0 ? o.memory_mb : half_of_mem_total_mb();
  m.free_cpus.insert(cpus.begin(), cpus.begin() + slots);  // each slot is pinned to a distinct physical CPU id
  if (!m.register_bean(slots, mem)) return m.stopping.load() ? 0 : 1;
  m.log("registered with " + std::to_string(slots) + " CPU slots, " + std::to_string(mem) + " MiB budget (supervised, address-space limited)");

  using Steady = std::chrono::steady_clock;
  auto next_heartbeat = Steady::now();
  while (!m.stopping.load()) {
    m.reap_finished();
    try {
      if (Steady::now() >= next_heartbeat) {
        next_heartbeat = Steady::now() + std::chrono::milliseconds(o.heartbeat_ms);
        ApiResponse hb = m.client->post("/v1/beans/" + m.bean_id + "/heartbeat");
        if (hb.status == 404) {
          m.log("coordinator does not know this Bean; registering again");
          m.register_bean(slots, mem);
        }
      }
      if (!m.free_cpus.empty()) {
        ApiResponse r = m.client->post("/v1/beans/" + m.bean_id + "/claim");
        if (r.ok() && r.body.contains("claim") && r.body["claim"].is_object()) {
          json claim = r.body["claim"];
          int want = claim["cpus"].get<int>();
          if (static_cast<size_t>(want) > m.free_cpus.size()) {
            m.log("claimed task needs " + std::to_string(want) + " CPUs but only " + std::to_string(m.free_cpus.size()) + " are free locally; letting the lease expire");
            continue;
          }
          Impl::Running run;
          for (int i = 0; i < want; i++) {
            run.cpus.push_back(*m.free_cpus.begin());
            m.free_cpus.erase(m.free_cpus.begin());
          }
          detail::AttemptContext ctx;
          ctx.options = &m.opts;
          ctx.client = m.client.get();
          ctx.bean_id = m.bean_id;
          ctx.stopping = &m.stopping;
          ctx.cpu_ids = run.cpus;
          ctx.log = [&m](const std::string& s) { m.log(s); };
          auto done = run.done;
          run.thread = std::thread([ctx, claim, done] {
            detail::run_attempt(ctx, claim);
            done->store(true);
          });
          m.running.push_back(std::move(run));
          continue;  // try to fill remaining capacity immediately
        }
        if (r.status == 404) m.register_bean(slots, mem);
        else if (!r.ok()) m.log("claim failed: " + r.error_code);
      }
    } catch (const NetworkError& e) {
      m.log(std::string("coordinator unreachable: ") + e.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(1000, o.poll_ms * 2)));
      continue;
    }
    for (int64_t waited = 0; waited < o.poll_ms && !m.stopping.load(); waited += 25) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  // Graceful stop: attempt threads cancel their commands and report retryable failures, then we join them.
  m.log("stopping; cancelling " + std::to_string(m.running.size()) + " running attempt(s)");
  for (auto& r : m.running) r.thread.join();
  m.running.clear();
  return 0;
}

}  // namespace coffee
