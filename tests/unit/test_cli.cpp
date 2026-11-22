#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "coffee/cli.hpp"
#include "coffee/store.hpp"

using namespace coffee;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct Result {
  int code;
  std::string out, err;
};
Result run(std::vector<std::string> args) {
  std::ostringstream out, err;
  int code = run_cli(args, out, err);
  return {code, out.str(), err.str()};
}
std::string example() { return (fs::path(COFFEE_TEST_SOURCE_DIR) / "examples/coffee.yaml").string(); }
}  // namespace

TEST_CASE("--version and --help", "[cli]") {
  auto v = run({"--version"});
  CHECK(v.code == 0);
  CHECK(v.out.find(kVersion) != std::string::npos);
  CHECK(v.out.find("coffee") != std::string::npos);
  for (auto flag : {"--help", "-h", "help"}) {
    auto h = run({flag});
    CHECK(h.code == 0);
    CHECK(h.out.find("init") != std::string::npos);
    CHECK(h.out.find("validate") != std::string::npos);
    CHECK(h.out.find("Exit codes") != std::string::npos);
  }
  CHECK(run({}).code == 2);
  CHECK(run({"init", "--help"}).out.find("--state") != std::string::npos);
  CHECK(run({"validate", "--help"}).out.find("--json") != std::string::npos);
}

TEST_CASE("unknown and unimplemented commands fail clearly", "[cli]") {
  auto u = run({"frobnicate"});
  CHECK(u.code == 2);
  CHECK(u.err.find("unknown command") != std::string::npos);
  for (auto cmd : {"prepare"}) {
    auto r = run({cmd});
    INFO(cmd);
    CHECK(r.code == 3);
    CHECK(r.err.find("not implemented") != std::string::npos);
    CHECK(r.out.empty());
  }
  auto o = run({"validate", "--bogus", example()});
  CHECK(o.code == 2);
  CHECK(o.err.find("--bogus") != std::string::npos);
}

TEST_CASE("coffee init creates state, is repeatable and never prints the token", "[cli]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-cli-" + random_hex(6));
  auto first = run({"init", "--state", dir.string()});
  CHECK(first.code == 0);
  CHECK(fs::exists(dir / "token"));
  auto token = read_cluster_token(dir);
  CHECK(first.out.find(token) == std::string::npos);
  auto again = run({"init", "--state=" + dir.string(), "--json"});
  CHECK(again.code == 0);
  auto j = json::parse(again.out);
  CHECK(j["created"] == false);
  CHECK(j["cluster_id"].is_string());
  CHECK(again.out.find(token) == std::string::npos);
  CHECK(read_cluster_token(dir) == token);
  CHECK(run({"init"}).code == 2);
  CHECK(run({"init", "--state"}).code == 2);
  CHECK(run({"init", "--state", dir.string(), "extra"}).code == 2);
  fs::remove_all(dir);
}

TEST_CASE("coffee init fails with exit 1 when the path is unusable", "[cli]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-cli-" + random_hex(6));
  fs::create_directories(dir);
  { std::ofstream(dir / "file") << "x"; }
  auto r = run({"init", "--state", (dir / "file").string()});
  CHECK(r.code == 1);
  CHECK_FALSE(r.err.empty());
  fs::remove_all(dir);
}

TEST_CASE("coffee validate accepts the example and reports defaults as JSON", "[cli]") {
  auto human = run({"validate", example()});
  CHECK(human.code == 0);
  CHECK(human.out.find("word-count-demo") != std::string::npos);
  auto r = run({"validate", example(), "--json"});
  CHECK(r.code == 0);
  auto j = json::parse(r.out);
  CHECK(j["valid"] == true);
  CHECK(j["workload"]["spec"]["tasks"][2]["max_attempts"] == 3);
}

TEST_CASE("coffee validate reports invalid workloads with exit 1", "[cli]") {
  auto dir = fs::temp_directory_path() / ("coffee-test-cli-" + random_hex(6));
  fs::create_directories(dir);
  { std::ofstream(dir / "bad.yaml") << "apiVersion: coffee/v1alpha1\nkind: Workload\nmetadata: {name: x}\nspec:\n  tasks:\n    - id: a\n      command: [x]\n      depends_on: [a]\n"; }
  auto r = run({"validate", (dir / "bad.yaml").string()});
  CHECK(r.code == 1);
  CHECK(r.err.find("spec.tasks[0].depends_on") != std::string::npos);
  auto rj = run({"validate", (dir / "bad.yaml").string(), "--json"});
  CHECK(rj.code == 1);
  auto j = json::parse(rj.out);
  CHECK(j["valid"] == false);
  CHECK(j["error"]["field"] == "spec.tasks[0].depends_on[0]");
  CHECK(run({"validate", (dir / "nope.yaml").string()}).code == 1);
  CHECK(run({"validate"}).code == 2);
  fs::remove_all(dir);
}
