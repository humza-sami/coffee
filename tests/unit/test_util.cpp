#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>

#include "coffee/util.hpp"

using namespace coffee;
namespace fs = std::filesystem;

static fs::path temp_dir(const std::string& name) {
  fs::path p = fs::temp_directory_path() / ("coffee-test-" + name + "-" + random_hex(6));
  fs::create_directories(p);
  return p;
}

TEST_CASE("sha256 matches known vectors", "[util]") {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("sha256_file hashes file content and reports missing files", "[util]") {
  auto dir = temp_dir("sha");
  { std::ofstream(dir / "f") << "abc"; }
  CHECK(sha256_file(dir / "f") == sha256_hex("abc"));
  CHECK_THROWS_AS(sha256_file(dir / "missing"), Error);
  fs::remove_all(dir);
}

TEST_CASE("random_hex returns distinct lowercase hex of requested size", "[util]") {
  auto a = random_hex(16), b = random_hex(16);
  CHECK(a.size() == 32);
  CHECK(a != b);
  CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("safe relative path rules", "[util]") {
  for (const char* ok : {"a", "a/b.txt", "dir/sub/file-1_2.dat", ".hidden", "a..b"})
    CHECK_FALSE(check_safe_relative_path(ok).has_value());
  for (const char* bad : {"", "/abs", "../x", "a/../b", "a/./b", ".", "..", "a//b", "a/", "a\\b", "a\nb"})
    CHECK(check_safe_relative_path(bad).has_value());
  CHECK(check_safe_relative_path(std::string(256, 'x')).has_value());
  CHECK(check_safe_relative_path(std::string("a\0b", 3)).has_value());
}

TEST_CASE("confine_path rejects symlink escapes", "[util]") {
  auto root = temp_dir("confine");
  auto outside = temp_dir("outside");
  fs::create_directories(root / "d");
  fs::create_directory_symlink(outside, root / "escape");
  CHECK(confine_path(root, "d/file") == fs::weakly_canonical(root) / "d/file");
  CHECK_THROWS_AS(confine_path(root, "escape/file"), Error);
  CHECK_THROWS_AS(confine_path(root, "../x"), Error);
  fs::remove_all(root);
  fs::remove_all(outside);
}

TEST_CASE("write_file_atomic and read_file_limited", "[util]") {
  auto dir = temp_dir("rw");
  write_file_atomic(dir / "x", "hello", 0600);
  CHECK(read_file_limited(dir / "x", 100) == "hello");
  CHECK((fs::status(dir / "x").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
  CHECK_THROWS_AS(read_file_limited(dir / "x", 3), Error);
  CHECK_THROWS_AS(read_file_limited(dir / "nope", 3), Error);
  fs::remove_all(dir);
}

TEST_CASE("ManualClock and SystemClock", "[util]") {
  ManualClock c(100);
  c.advance(50);
  CHECK(c.now_ms() == 150);
  SystemClock s;
  CHECK(s.now_ms() > 1'700'000'000'000);
}

TEST_CASE("incremental SHA-256 matches the one-shot digest", "[util][hash]") {
  Sha256Stream s;
  s.update("abc", 1);
  s.update("bc", 2);
  CHECK(s.finish() == sha256_hex("abc"));
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
