#include <catch_amalgamated.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "coffee/store.hpp"

using namespace coffee;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json task(const std::string& id, std::vector<std::string> deps = {}, int cpus = 1, int mem = 256, int attempts = 3, int timeout = 300) {
  return {{"id", id}, {"command", json::array({"true"})}, {"depends_on", deps}, {"resources", {{"cpus", cpus}, {"memory_mb", mem}}},
          {"max_attempts", attempts}, {"timeout_seconds", timeout}};
}

json workload(std::vector<json> tasks, const std::string& name = "w") {
  return {{"apiVersion", "coffee/v1alpha1"}, {"kind", "Workload"}, {"metadata", {{"name", name}}}, {"spec", {{"tasks", tasks}}}};
}

struct Env {
  fs::path dir = fs::temp_directory_path() / ("coffee-test-store-" + random_hex(6));
  std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1'000'000);
  StoreOptions opts;
  std::unique_ptr<Store> store;

  Env() {
    opts.clock = clock;
    opts.lease_ms = 1000;
    opts.heartbeat_timeout_ms = 3000;
    opts.deadline_grace_ms = 0;
    init_state_dir(dir);
    reopen();
  }
  ~Env() {
    store.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void reopen() {
    store.reset();
    store = Store::open(dir, opts);
  }
  std::string bean(const std::string& id, int cpus = 4, int mem = 4096) {
    BeanRegistration r;
    r.id = id;
    r.cpus = cpus;
    r.memory_mb = mem;
    return store->register_bean(r).id;
  }
  CompleteRequest done(const Claim& c, bool ok = true, json outputs = json::array()) {
    CompleteRequest r;
    r.lease_id = c.lease_id;
    r.bean_id = c.bean_id;
    r.success = ok;
    r.exit_code = ok ? 0 : 1;
    r.reason = ok ? "" : "exit 1";
    r.outputs = outputs;
    return r;
  }
  std::string task_state(const std::string& job, const std::string& t) {
    auto st = store->job_status(job);  // keep the temporary alive while iterating
    for (auto& x : (*st)["tasks"])
      if (x["id"] == t) return x["state"];
    return "?";
  }
};

}  // namespace

// ------------------------------------------------------------------ init

TEST_CASE("init creates private layout, token and database", "[init]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-init-" + random_hex(6));
  auto r = init_state_dir(dir);
  CHECK(r.created);
  CHECK(r.token_created);
  CHECK(r.database_created);
  CHECK(r.cluster_id.size() >= 16);
  for (auto sub : {"artifacts/inputs", "artifacts/outputs", "artifacts/logs", "artifacts/tmp"}) CHECK(fs::is_directory(dir / sub));
  CHECK(fs::exists(dir / "coffee.db"));
  CHECK(fs::exists(dir / "config.json"));
  auto token = read_cluster_token(dir);
  CHECK(token.size() == 64);
  CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
  auto none = fs::perms::group_all | fs::perms::others_all;
  CHECK((fs::status(dir / "token").permissions() & none) == fs::perms::none);
  CHECK((fs::status(dir).permissions() & none) == fs::perms::none);
  CHECK(fs::file_size(dir / "token") == 64);
  CHECK((fs::status(dir / "coffee.db").permissions() & none) == fs::perms::none);  // holds lease ids
  fs::remove_all(dir);
}

TEST_CASE("repeat init preserves identity, token and accepted results", "[init]") {
  Env e;
  auto id = e.store->cluster_id();
  auto token = read_cluster_token(e.dir);
  e.bean("b1");
  auto sub = e.store->submit(workload({task("a")}), "job-1");
  auto c = e.store->claim("b1");
  REQUIRE(c);
  REQUIRE(e.store->complete(e.done(*c, true, json::array({{{"path", "o"}, {"size", 1}}})) ).ok());
  e.store.reset();

  fs::permissions(e.dir / "token", fs::perms::group_read | fs::perms::others_read, fs::perm_options::add);
  auto again = init_state_dir(e.dir);
  CHECK_FALSE(again.created);
  CHECK_FALSE(again.token_created);
  CHECK_FALSE(again.database_created);
  CHECK(again.cluster_id == id);
  CHECK(read_cluster_token(e.dir) == token);
  CHECK((fs::status(e.dir / "token").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);

  e.reopen();
  CHECK(e.store->cluster_id() == id);
  auto st = e.store->job_status("job-1");
  REQUIRE(st);
  CHECK((*st)["state"] == "succeeded");
  CHECK((*st)["tasks"][0]["result"][0]["path"] == "o");
}

TEST_CASE("init refuses to overwrite corrupt identity files", "[init]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-init-" + random_hex(6));
  init_state_dir(dir);
  { std::ofstream(dir / "config.json") << "{not json"; }
  CHECK_THROWS_AS(init_state_dir(dir), Error);
  { std::ifstream f(dir / "config.json"); std::string s((std::istreambuf_iterator<char>(f)), {}); CHECK(s == "{not json"); }
  fs::remove_all(dir);
}

TEST_CASE("open fails for an uninitialized state directory", "[init]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-init-" + random_hex(6));
  fs::create_directories(dir);
  CHECK_THROWS_AS(Store::open(dir), Error);
  CHECK_THROWS_AS(read_cluster_token(dir), Error);
  fs::remove_all(dir);
}

// ------------------------------------------------------------------ submit / beans

TEST_CASE("submit is idempotent per job id and rejects conflicts and invalid workloads", "[store]") {
  Env e;
  auto r = e.store->submit(workload({task("a")}));
  CHECK(r.created);
  CHECK(r.job_id.rfind("job-", 0) == 0);
  CHECK(r.task_count == 1);
  auto w = workload({task("a"), task("b", {"a"})});
  CHECK(e.store->submit(w, "fixed").created);
  CHECK_FALSE(e.store->submit(w, "fixed").created);
  CHECK_THROWS_AS(e.store->submit(workload({task("z")}), "fixed"), Error);
  CHECK_THROWS_AS(e.store->submit(workload({task("a", {"a"})})), WorkloadError);
  CHECK_THROWS_AS(e.store->submit(workload({task("a")}), "../bad id"), Error);
  CHECK(e.store->list_jobs().size() == 2);
  CHECK_FALSE(e.store->job_status("nope").has_value());
}

TEST_CASE("bean registration, heartbeat, reachability and drain", "[store]") {
  Env e;
  BeanRegistration r;
  r.name = "laptop";
  r.cpus = 2;
  r.memory_mb = 1024;
  auto b = e.store->register_bean(r);
  CHECK(b.id.rfind("bean-", 0) == 0);
  CHECK(b.state == "online");
  CHECK(b.cpus_used == 0);
  CHECK(e.store->heartbeat(b.id));
  CHECK_FALSE(e.store->heartbeat("bean-unknown"));
  e.clock->advance(3001);
  CHECK(e.store->list_beans().at(0).state == "unreachable");
  CHECK(e.store->heartbeat(b.id));
  CHECK(e.store->list_beans().at(0).state == "online");
  CHECK(e.store->set_draining(b.id, true));
  CHECK(e.store->list_beans().at(0).state == "draining");
  CHECK_FALSE(e.store->set_draining("nope", true));
  BeanRegistration bad;
  bad.cpus = 0;
  CHECK_THROWS_AS(e.store->register_bean(bad), Error);
  bad.cpus = 1;
  bad.memory_mb = -5;
  CHECK_THROWS_AS(e.store->register_bean(bad), Error);
  bad.memory_mb = 1;
  bad.id = "bad id/../";
  CHECK_THROWS_AS(e.store->register_bean(bad), Error);
}

// ------------------------------------------------------------------ claims and accounting

TEST_CASE("claim is first-fit in submission/declaration order and respects dependencies", "[store]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a"), task("b", {"a"}), task("c")}), "j1");
  e.store->submit(workload({task("x")}), "j2");
  auto c1 = e.store->claim("b1");
  REQUIRE(c1);
  CHECK(c1->job_id == "j1");
  CHECK(c1->task_id == "a");
  CHECK(c1->attempt == 1);
  CHECK(c1->lease_id.size() >= 32);
  CHECK(c1->lease_expires_at_ms == e.clock->now_ms() + 1000);
  CHECK(c1->task["id"] == "a");
  auto c2 = e.store->claim("b1");
  REQUIRE(c2);
  CHECK(c2->task_id == "c");  // b is blocked on a
  CHECK(e.store->claim("b1")->task_id == "x");
  CHECK_FALSE(e.store->claim("b1").has_value());  // only b left, still blocked
  REQUIRE(e.store->complete(e.done(*c1)).ok());
  auto c3 = e.store->claim("b1");
  REQUIRE(c3);
  CHECK(c3->task_id == "b");
  CHECK_THROWS_AS(e.store->claim("ghost"), Error);
}

TEST_CASE("resource reservation prevents double allocation and skips tasks that do not fit", "[store]") {
  Env e;
  e.bean("small", 3, 1000);
  e.store->submit(workload({task("big", {}, 2, 600), task("big2", {}, 2, 600), task("tiny", {}, 1, 100)}), "j");
  auto c1 = e.store->claim("small");
  REQUIRE(c1);
  CHECK(c1->task_id == "big");
  auto beans = e.store->list_beans();
  CHECK(beans[0].cpus_used == 2);
  CHECK(beans[0].memory_mb_used == 600);
  auto c2 = e.store->claim("small");  // big2 needs 2 cpus, only 1 free -> first fit picks tiny
  REQUIRE(c2);
  CHECK(c2->task_id == "tiny");
  CHECK_FALSE(e.store->claim("small").has_value());
  REQUIRE(e.store->complete(e.done(*c1)).ok());
  auto c3 = e.store->claim("small");
  REQUIRE(c3);
  CHECK(c3->task_id == "big2");
  CHECK(e.store->list_beans()[0].cpus_used == 3);
  REQUIRE(e.store->complete(e.done(*c2)).ok());
  REQUIRE(e.store->complete(e.done(*c3)).ok());
  CHECK(e.store->list_beans()[0].cpus_used == 0);
  CHECK(e.store->list_beans()[0].memory_mb_used == 0);
}

TEST_CASE("draining Beans claim nothing; unsatisfiable tasks are explained", "[store]") {
  Env e;
  e.bean("b1", 2, 512);
  e.store->submit(workload({task("huge", {}, 8, 100), task("dep", {"huge"})}), "j");
  CHECK_FALSE(e.store->claim("b1").has_value());
  auto ex = e.store->explain_task("j", "huge");
  REQUIRE(ex);
  CHECK((*ex)["state"] == "pending");
  CHECK((*ex)["unsatisfiable"] == true);
  CHECK((*ex)["placement"][0]["fits"] == false);
  CHECK((*ex)["placement"][0]["reasons"][0].get<std::string>().find("cpus") != std::string::npos);
  auto ex2 = e.store->explain_task("j", "dep");
  CHECK((*ex2)["blocked_by"] == json::array({"huge"}));
  CHECK_FALSE(e.store->explain_task("j", "nope").has_value());

  e.store->submit(workload({task("ok")}), "j2");
  e.store->set_draining("b1", true);
  CHECK_FALSE(e.store->claim("b1").has_value());
  e.store->set_draining("b1", false);
  CHECK(e.store->claim("b1")->task_id == "ok");
}

TEST_CASE("concurrent claimers never receive the same task", "[store][concurrency]") {
  Env e;
  const int n = 40;
  std::vector<json> tasks;
  for (int i = 0; i < n; i++) tasks.push_back(task("t" + std::to_string(i)));
  e.store->submit(workload(tasks), "j");
  auto store2 = Store::open(e.dir, e.opts);
  e.bean("b1", 100, 100000);
  e.bean("b2", 100, 100000);
  std::vector<std::string> got1, got2;
  auto run = [&](Store* s, const std::string& bean, std::vector<std::string>* out) {
    while (auto c = s->claim(bean)) out->push_back(c->task_id);
  };
  std::thread t1(run, e.store.get(), "b1", &got1), t2(run, store2.get(), "b2", &got2);
  t1.join();
  t2.join();
  std::set<std::string> all(got1.begin(), got1.end());
  all.insert(got2.begin(), got2.end());
  CHECK(got1.size() + got2.size() == static_cast<size_t>(n));
  CHECK(all.size() == static_cast<size_t>(n));
}

// ------------------------------------------------------------------ leases

TEST_CASE("renew extends a valid lease and completion records accepted output", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.clock->advance(900);
  auto r = e.store->renew(c.lease_id, "b1");
  CHECK(r.ok());
  CHECK(r.lease_expires_at_ms == e.clock->now_ms() + 1000);
  e.clock->advance(900);  // past the original expiry, within the renewed one
  CHECK(e.store->lease_valid(c.lease_id, "b1"));
  CHECK_FALSE(e.store->lease_valid(c.lease_id, "other"));
  CHECK((*e.store->job_status("j"))["state"] == "running");
  auto out = json::array({{{"path", "out.txt"}, {"size", 3}, {"sha256", "ab"}}});
  auto res = e.store->complete(e.done(c, true, out));
  CHECK(res.ok());
  CHECK(res.task_state == "succeeded");
  auto st = *e.store->job_status("j");
  CHECK(st["state"] == "succeeded");
  CHECK(st["tasks"][0]["result"] == out);
  CHECK(st["tasks"][0]["attempts"][0]["state"] == "succeeded");
  CHECK(st["tasks"][0]["attempts"][0]["bean_id"] == "b1");
  CHECK(st.dump().find(c.lease_id) == std::string::npos);
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::NotRunning);  // duplicate completion
  CHECK(e.store->renew(c.lease_id, "b1").status == LeaseStatus::NotRunning);
}

TEST_CASE("unknown lease and wrong bean are rejected without effect", "[lease]") {
  Env e;
  e.bean("b1");
  e.bean("b2");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  CHECK(e.store->renew("lease-nope", "b1").status == LeaseStatus::UnknownLease);
  auto bad = e.done(c);
  bad.bean_id = "b2";
  CHECK(e.store->complete(bad).status == LeaseStatus::WrongBean);
  CHECK(e.store->renew(c.lease_id, "b2").status == LeaseStatus::WrongBean);
  CHECK(e.task_state("j", "a") == "running");
  CHECK(e.store->complete(e.done(c)).ok());
}

TEST_CASE("expired lease is re-queued and the old attempt cannot replace accepted output", "[lease]") {
  Env e;
  e.bean("b1");
  e.bean("b2");
  e.store->submit(workload({task("a")}), "j");
  auto old = *e.store->claim("b1");
  e.clock->advance(1001);
  auto late = e.store->renew(old.lease_id, "b1");
  CHECK(late.status == LeaseStatus::Expired);
  CHECK(e.task_state("j", "a") == "pending");
  auto fresh = e.store->claim("b2");
  REQUIRE(fresh);
  CHECK(fresh->attempt == 2);
  CHECK(fresh->lease_id != old.lease_id);
  auto good = json::array({{{"path", "o"}, {"size", 1}, {"sha256", "new"}}});
  REQUIRE(e.store->complete(e.done(*fresh, true, good)).ok());
  auto stale = e.store->complete(e.done(old, true, json::array({{{"path", "o"}, {"size", 9}, {"sha256", "old"}}})));
  CHECK(stale.status == LeaseStatus::Expired);
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["result"] == good);
  CHECK(st["tasks"][0]["attempts"].size() == 2);
  CHECK(st["tasks"][0]["attempts"][0]["state"] == "expired");
  CHECK(st["tasks"][0]["attempts"][0]["bean_id"] == "b1");
  CHECK(st["tasks"][0]["attempts"][1]["bean_id"] == "b2");
}

TEST_CASE("completion after lease expiry is rejected even if nobody reclaimed", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.clock->advance(1000);
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::Expired);
  CHECK(e.task_state("j", "a") == "pending");
  CHECK(e.store->list_beans()[0].cpus_used == 0);  // reservation released
}

TEST_CASE("wall-clock deadline fences attempts even when the lease is renewed", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a", {}, 1, 256, 2, 5)}), "j");  // timeout 5 s, grace 0
  auto c = *e.store->claim("b1");
  CHECK(c.deadline_at_ms == e.clock->now_ms() + 5000);
  for (int i = 0; i < 4; i++) {
    e.clock->advance(900);
    REQUIRE(e.store->renew(c.lease_id, "b1").ok());
  }
  e.clock->advance(900);  // 4.5 s elapsed, still fine
  REQUIRE(e.store->renew(c.lease_id, "b1").ok());
  e.clock->advance(600);  // 5.1 s
  CHECK(e.store->renew(c.lease_id, "b1").status == LeaseStatus::DeadlineExceeded);
  CHECK(e.task_state("j", "a") == "pending");
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["attempts"][0]["state"] == "timed_out");
}

TEST_CASE("max attempts bound retries and the failure is visible", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a", {}, 1, 256, 3)}), "j");
  for (int i = 1; i <= 3; i++) {
    auto c = e.store->claim("b1");
    REQUIRE(c);
    CHECK(c->attempt == i);
    auto r = e.store->complete(e.done(*c, false));
    CHECK(r.ok());
    CHECK(r.task_state == (i < 3 ? "pending" : "failed"));
  }
  CHECK_FALSE(e.store->claim("b1").has_value());
  auto st = *e.store->job_status("j");
  CHECK(st["state"] == "failed");
  CHECK(st["tasks"][0]["state"] == "failed");
  CHECK(st["tasks"][0]["failure_reason"].get<std::string>().find("max attempts") != std::string::npos);
  CHECK(st["tasks"][0]["attempts"].size() == 3);
  CHECK(st["tasks"][0]["attempts"][2]["exit_code"] == 1);
  CHECK(st["tasks"][0]["attempts"][2]["reason"] == "exit 1");
}

TEST_CASE("non-retryable failure fails at once and failure propagates to dependents", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a"), task("b", {"a"}), task("c", {"b"}), task("free")}), "j");
  auto a = *e.store->claim("b1");
  auto req = e.done(a, false);
  req.retryable = false;
  CHECK(e.store->complete(req).task_state == "failed");
  CHECK(e.task_state("j", "b") == "failed");
  CHECK(e.task_state("j", "c") == "failed");
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][1]["failure_reason"] == "dependency_failed: a");
  CHECK(st["tasks"][2]["failure_reason"] == "dependency_failed: b");
  CHECK(st["state"] == "running");  // 'free' not finished yet
  auto f = *e.store->claim("b1");
  CHECK(f.task_id == "free");
  CHECK(e.store->complete(e.done(f)).ok());
  CHECK((*e.store->job_status("j"))["state"] == "failed");
}

TEST_CASE("re-registering a Bean abandons its running attempts", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.bean("b1");  // worker restarted
  CHECK(e.task_state("j", "a") == "pending");
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::NotRunning);
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["attempts"][0]["state"] == "abandoned");
  CHECK(e.store->claim("b1")->attempt == 2);
}

// ------------------------------------------------------------------ cancellation

TEST_CASE("cancel fences running attempts and cancels dependents", "[cancel]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a"), task("b", {"a"}), task("c")}), "j");
  auto a = *e.store->claim("b1");
  auto r = e.store->cancel("j", "a", "no longer needed");
  CHECK(r.found);
  CHECK(r.tasks_cancelled == 2);  // a and its dependent b
  CHECK(r.attempts_cancelled == 1);
  CHECK(e.store->renew(a.lease_id, "b1").status == LeaseStatus::Cancelled);
  CHECK(e.store->complete(e.done(a, true, json::array({{{"path", "o"}}}))).status == LeaseStatus::Cancelled);
  CHECK(e.task_state("j", "a") == "cancelled");
  CHECK(e.task_state("j", "b") == "cancelled");
  CHECK((*e.store->job_status("j"))["tasks"][0]["result"].is_null());
  CHECK(e.store->list_beans()[0].cpus_used == 0);
  CHECK(e.store->claim("b1")->task_id == "c");
}

TEST_CASE("cancel job stops everything, keeps accepted results and is idempotent", "[cancel]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a"), task("b"), task("c", {"b"})}), "j");
  auto a = *e.store->claim("b1");
  REQUIRE(e.store->complete(e.done(a, true, json::array({{{"path", "keep"}}}))).ok());
  auto b = *e.store->claim("b1");
  auto r = e.store->cancel("j");
  CHECK(r.found);
  CHECK(r.job_state == "cancelled");
  CHECK(r.tasks_cancelled == 2);
  CHECK(e.store->complete(e.done(b)).status == LeaseStatus::Cancelled);
  CHECK_FALSE(e.store->claim("b1").has_value());
  CHECK(e.task_state("j", "a") == "succeeded");
  CHECK((*e.store->job_status("j"))["tasks"][0]["result"][0]["path"] == "keep");
  CHECK(e.store->cancel("j").tasks_cancelled == 0);
  CHECK_FALSE(e.store->cancel("missing").found);
  CHECK_FALSE(e.store->cancel("j", "ghost").found);
}

// ------------------------------------------------------------------ restart & publication hook

TEST_CASE("restart preserves accepted results, history and fences stale leases", "[restart]") {
  Env e;
  e.bean("b1");
  e.bean("b2");
  e.store->submit(workload({task("a"), task("b", {"a"}), task("c")}), "j");
  auto a = *e.store->claim("b1");
  auto good = json::array({{{"path", "a.out"}, {"size", 2}, {"sha256", "x"}}});
  REQUIRE(e.store->complete(e.done(a, true, good)).ok());
  auto b = *e.store->claim("b1");  // b running on b1
  auto c = *e.store->claim("b2");  // c running on b2
  e.reopen();

  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["result"] == good);
  CHECK(st["tasks"][0]["attempts"][0]["bean_id"] == "b1");
  CHECK(e.store->list_beans().size() == 2);
  CHECK(e.store->list_beans()[0].cpus_used + e.store->list_beans()[1].cpus_used == 2);  // reservations survive

  // reopening changes no timestamps: a lease that has not yet expired is still valid and can be completed
  CHECK(e.store->lease_valid(b.lease_id, "b1"));
  CHECK(e.store->complete(e.done(b)).ok());

  // c's worker never comes back: its lease lapses at its original expiry, task re-queued for the survivor
  e.clock->advance(1001);
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::Expired);
  auto again = e.store->claim("b1");
  REQUIRE(again);
  CHECK(again->task_id == "c");
  CHECK(again->attempt == 2);
  CHECK(e.store->complete(e.done(*again)).ok());
  CHECK((*e.store->job_status("j"))["state"] == "succeeded");
}

TEST_CASE("cancellation survives restart and still fences publication", "[restart]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.store->cancel("j");
  e.reopen();
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::Cancelled);
  CHECK(e.task_state("j", "a") == "cancelled");
}

TEST_CASE("before_commit hook runs only for accepted completions and can veto", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  int calls = 0;
  auto req = e.done(c);
  req.before_commit = [&] { calls++; throw std::runtime_error("disk full"); };
  CHECK_THROWS_AS(e.store->complete(req), std::runtime_error);
  CHECK(calls == 1);
  CHECK(e.task_state("j", "a") == "running");  // nothing committed

  req.before_commit = [&] { calls++; };
  auto bad = req;
  bad.bean_id = "b2";
  e.bean("b2");
  CHECK(e.store->complete(bad).status == LeaseStatus::WrongBean);
  CHECK(calls == 1);  // not invoked for rejected completions
  CHECK(e.store->complete(req).ok());
  CHECK(calls == 2);
  CHECK(e.task_state("j", "a") == "succeeded");
}

TEST_CASE("reap_expired reports ended attempts", "[lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a"), task("b")}), "j");
  e.store->claim("b1");
  e.store->claim("b1");
  CHECK(e.store->reap_expired() == 0);
  e.clock->advance(1500);
  CHECK(e.store->reap_expired() == 2);
  CHECK(e.store->reap_expired() == 0);
  CHECK(e.store->list_beans()[0].cpus_used == 0);
}

// ------------------------------------------------------------------ correction round 1: no lease revival, post-hook revalidation

namespace {
// Shared assertions: the attempt was fenced, its reservation released and retry state recovered.
void expect_fenced(Env& e, const std::string& job, const std::string& attempt_state) {
  auto st = *e.store->job_status(job);
  CHECK(st["tasks"][0]["state"] == "pending");
  CHECK(st["tasks"][0]["result"].is_null());
  CHECK(st["tasks"][0]["accepted_attempt"].is_null());
  CHECK(st["tasks"][0]["attempts"][0]["state"] == attempt_state);
  CHECK(e.store->list_beans()[0].cpus_used == 0);
  CHECK(e.store->list_beans()[0].memory_mb_used == 0);
}
}  // namespace

TEST_CASE("opening a second Store exactly at lease expiry does not revive the lease", "[restart][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.clock->advance(1000);  // now == lease_expires_at
  auto second = Store::open(e.dir, e.opts);
  CHECK_FALSE(e.store->lease_valid(c.lease_id, "b1"));
  CHECK_FALSE(second->lease_valid(c.lease_id, "b1"));
  CHECK(e.store->complete(e.done(c, true, json::array({{{"path", "o"}}}))).status == LeaseStatus::Expired);
  expect_fenced(e, "j", "expired");
}

TEST_CASE("opening a second Store after lease expiry does not revive the lease", "[restart][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.clock->advance(1500);
  auto second = Store::open(e.dir, e.opts);
  CHECK(second->complete(e.done(c)).status == LeaseStatus::Expired);
  expect_fenced(e, "j", "expired");
  CHECK(e.store->claim("b1")->attempt == 2);
}

TEST_CASE("opening a second Store before expiry does not extend the lease", "[restart][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.clock->advance(500);
  auto second = Store::open(e.dir, e.opts);
  CHECK((*second->job_status("j"))["tasks"][0]["attempts"][0]["lease_expires_at_ms"] == c.lease_expires_at_ms);
  CHECK(e.store->lease_valid(c.lease_id, "b1"));  // still valid under its original expiry
  e.clock->advance(501);                          // original expiry passed; a revived lease would run to +1500
  CHECK_FALSE(e.store->lease_valid(c.lease_id, "b1"));
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::Expired);
  expect_fenced(e, "j", "expired");
}

TEST_CASE("default reopen after an outage past expiry keeps the lease expired", "[restart][fencing]") {
  Env e;
  e.bean("b1");
  e.bean("b2");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  e.store.reset();
  e.clock->advance(5000);
  e.reopen();  // default options
  CHECK_FALSE(e.store->lease_valid(c.lease_id, "b1"));
  CHECK(e.store->complete(e.done(c, true, json::array({{{"path", "o"}}}))).status == LeaseStatus::Expired);
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["state"] == "pending");
  CHECK(st["tasks"][0]["result"].is_null());
  CHECK(e.store->list_beans()[0].cpus_used == 0);
  auto again = e.store->claim("b2");
  REQUIRE(again);
  CHECK(again->attempt == 2);
  CHECK(e.store->complete(e.done(*again)).ok());
}

TEST_CASE("a hook that crosses the lease expiry is not accepted", "[lease][fencing]") {
  Env e;
  e.bean("b1");
  e.bean("b2");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  int calls = 0;
  auto req = e.done(c, true, json::array({{{"path", "o"}, {"sha256", "stale"}}}));
  req.before_commit = [&] { calls++; e.clock->advance(1001); };  // slow publication outlives the lease
  auto res = e.store->complete(req);
  CHECK(calls == 1);
  CHECK(res.status == LeaseStatus::Expired);
  CHECK(res.task_state == "pending");
  expect_fenced(e, "j", "expired");
  auto again = e.store->claim("b2");
  REQUIRE(again);
  CHECK(again->attempt == 2);
  auto good = json::array({{{"path", "o"}, {"sha256", "fresh"}}});
  CHECK(e.store->complete(e.done(*again, true, good)).ok());
  CHECK((*e.store->job_status("j"))["tasks"][0]["result"] == good);
}

TEST_CASE("a hook that crosses the attempt deadline is not accepted", "[lease][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a", {}, 1, 256, 2, 5)}), "j");  // timeout 5 s, grace 0
  auto c = *e.store->claim("b1");
  for (int i = 0; i < 5; i++) {
    e.clock->advance(900);
    REQUIRE(e.store->renew(c.lease_id, "b1").ok());
  }  // 4.5 s elapsed, lease good until 5.5 s, deadline 5.0 s
  auto req = e.done(c, true, json::array({{{"path", "o"}}}));
  req.before_commit = [&] { e.clock->advance(600); };  // lease still valid (5.1 < 5.5) but deadline passed
  auto res = e.store->complete(req);
  CHECK(res.status == LeaseStatus::DeadlineExceeded);
  expect_fenced(e, "j", "timed_out");
}

TEST_CASE("a hook within the lease and deadline still completes", "[lease][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  auto out = json::array({{{"path", "o"}, {"sha256", "ok"}}});
  auto req = e.done(c, true, out);
  req.before_commit = [&] { e.clock->advance(999); };  // finishes one millisecond before expiry
  CHECK(e.store->complete(req).ok());
  auto st = *e.store->job_status("j");
  CHECK(st["tasks"][0]["state"] == "succeeded");
  CHECK(st["tasks"][0]["result"] == out);
  CHECK(e.store->list_beans()[0].cpus_used == 0);
}

TEST_CASE("cancellation after an expired-during-hook rejection stays fenced", "[lease][fencing]") {
  Env e;
  e.bean("b1");
  e.store->submit(workload({task("a")}), "j");
  auto c = *e.store->claim("b1");
  auto req = e.done(c);
  req.before_commit = [&] { e.clock->advance(1001); };
  CHECK(e.store->complete(req).status == LeaseStatus::Expired);
  CHECK(e.store->cancel("j").tasks_cancelled == 1);
  CHECK(e.store->complete(e.done(c)).status == LeaseStatus::Expired);
  CHECK(e.store->list_beans()[0].cpus_used == 0);
}

// ------------------------------------------------------------------ Input transfer contracts: input digests, dependency outputs, lease info

namespace {
const std::string kShaA(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c');

json pipeline() {
  json produce = task("produce");
  produce["inputs"] = json::array({json{{"source", "data/in.txt"}, {"target", "in.txt"}}});
  produce["outputs"] = json::array({"up.txt"});
  json consume = task("consume", {"produce"});
  consume["inputs"] = json::array({json{{"from_task", "produce"}, {"output", "up.txt"}, {"target", "in/up.txt"}}});
  return workload({produce, consume});
}
}  // namespace

TEST_CASE("submit records input digests; claims carry the resolved input manifest", "[store][deps]") {
  Env e;
  e.bean("b1");
  json files = {{"data/in.txt", {{"sha256", kShaA}, {"size", 12}}}};
  auto r = e.store->submit(pipeline(), "pipe", files);
  CHECK(r.created);
  CHECK_FALSE(e.store->submit(pipeline(), "pipe", files).created);  // identical resubmit is a no-op
  CHECK_THROWS_AS(e.store->submit(pipeline(), "pipe", json{{"data/in.txt", {{"sha256", kShaB}, {"size", 12}}}}), Error);  // different files conflict

  auto c1 = e.store->claim("b1");
  REQUIRE(c1);
  CHECK(c1->task_id == "produce");
  REQUIRE(c1->inputs.size() == 1);
  CHECK(c1->inputs[0] == json{{"kind", "file"}, {"source", "data/in.txt"}, {"target", "in.txt"}, {"sha256", kShaA}, {"size", 12}});
  CHECK_FALSE(e.store->claim("b1"));  // consumer still blocked

  json out = json::array({json{{"path", "up.txt"}, {"size", 12}, {"sha256", kShaB}}});
  REQUIRE(e.store->complete(e.done(*c1, true, out)).ok());
  auto c2 = e.store->claim("b1");
  REQUIRE(c2);
  CHECK(c2->task_id == "consume");
  REQUIRE(c2->inputs.size() == 1);
  CHECK(c2->inputs[0] == json{{"kind", "dependency"}, {"from_task", "produce"}, {"output", "up.txt"}, {"target", "in/up.txt"}, {"sha256", kShaB}, {"size", 12}});
}

TEST_CASE("dependency outputs resolve only the accepted attempt, never a failed or stale one", "[store][deps]") {
  Env e;
  e.bean("b1");
  e.store->submit(pipeline(), "pipe", json{{"data/in.txt", {{"sha256", kShaA}, {"size", 1}}}});
  auto first = e.store->claim("b1");
  REQUIRE(first);
  e.clock->advance(5000);  // lease (1s) expires -> re-queued
  auto second = e.store->claim("b1");
  REQUIRE(second);
  REQUIRE(second->attempt == 2);
  json out2 = json::array({json{{"path", "up.txt"}, {"size", 3}, {"sha256", kShaC}}});
  REQUIRE(e.store->complete(e.done(*second, true, out2)).ok());
  // the superseded attempt cannot replace the accepted output
  json out1 = json::array({json{{"path", "up.txt"}, {"size", 9}, {"sha256", kShaB}}});
  CHECK_FALSE(e.store->complete(e.done(*first, true, out1)).ok());
  auto c = e.store->claim("b1");
  REQUIRE(c);
  CHECK(c->inputs[0]["sha256"] == kShaC);
  CHECK(c->inputs[0]["size"] == 3);
}

TEST_CASE("a failed producer fails the consumer without it ever being claimed", "[store][deps]") {
  Env e;
  e.bean("b1");
  e.store->submit(pipeline(), "pipe", json{{"data/in.txt", {{"sha256", kShaA}, {"size", 1}}}});
  auto c = e.store->claim("b1");
  REQUIRE(c);
  CompleteRequest fail = e.done(*c, false);
  fail.retryable = false;
  e.store->complete(fail);
  CHECK(e.task_state("pipe", "consume") == "failed");
  CHECK_FALSE(e.store->claim("b1"));
}

TEST_CASE("lease_info exposes the attempt identity and declared outputs without leaking other state", "[store][lease]") {
  Env e;
  e.bean("b1");
  e.store->submit(pipeline(), "pipe", json{{"data/in.txt", {{"sha256", kShaA}, {"size", 1}}}});
  auto c = e.store->claim("b1");
  REQUIRE(c);
  auto info = e.store->lease_info(c->lease_id);
  REQUIRE(info);
  CHECK(info->job_id == "pipe");
  CHECK(info->task_id == "produce");
  CHECK(info->attempt == 1);
  CHECK(info->bean_id == "b1");
  CHECK(info->state == "running");
  CHECK(info->task["outputs"] == json::array({"up.txt"}));
  CHECK_FALSE(e.store->lease_info("lease-nope"));
  REQUIRE(e.store->complete(e.done(*c, true, json::array({json{{"path", "up.txt"}, {"size", 1}, {"sha256", kShaB}}}))).ok());
  CHECK(e.store->lease_info(c->lease_id)->state == "succeeded");
}

TEST_CASE("input digests and dependency resolution survive a restart", "[store][deps][restart]") {
  Env e;
  e.bean("b1");
  e.store->submit(pipeline(), "pipe", json{{"data/in.txt", {{"sha256", kShaA}, {"size", 7}}}});
  e.reopen();
  auto c = e.store->claim("b1");
  REQUIRE(c);
  CHECK(c->inputs[0]["sha256"] == kShaA);
  REQUIRE(e.store->complete(e.done(*c, true, json::array({json{{"path", "up.txt"}, {"size", 2}, {"sha256", kShaB}}}))).ok());
  e.reopen();
  auto c2 = e.store->claim("b1");
  REQUIRE(c2);
  CHECK(c2->inputs[0]["sha256"] == kShaB);
}
