#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <future>

#include "coffee/artifacts.hpp"

using namespace coffee;
namespace fs = std::filesystem;

namespace {
struct Tmp {
  fs::path dir = fs::temp_directory_path() / ("coffee-test-art-" + random_hex(6));
  Tmp() { fs::create_directories(dir); }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};
void put(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}
std::string slurp(const fs::path& p) { return read_file_limited(p, 1 << 20); }
}  // namespace

TEST_CASE("staged inputs are created confined and never follow symlinks", "[artifacts][paths]") {
  Tmp t;
  fs::path ws = t.dir / "ws", outside = t.dir / "outside";
  fs::create_directories(ws);
  fs::create_directories(outside);
  int fd = create_confined_file(ws, "a/b/c.txt", 0600);
  REQUIRE(fd >= 0);
  CHECK(::write(fd, "hi", 2) == 2);
  ::close(fd);
  CHECK(slurp(ws / "a/b/c.txt") == "hi");

  CHECK_THROWS_AS(create_confined_file(ws, "a/b/c.txt", 0600), Error);  // duplicate destination (O_EXCL)
  CHECK_THROWS_AS(create_confined_file(ws, "../escape", 0600), Error);
  CHECK_THROWS_AS(create_confined_file(ws, "/abs", 0600), Error);

  fs::create_directory_symlink(outside, ws / "link");
  CHECK_THROWS_AS(create_confined_file(ws, "link/pwn.txt", 0600), Error);  // symlinked directory component
  CHECK_FALSE(fs::exists(outside / "pwn.txt"));
  fs::create_symlink(outside / "victim", ws / "leaf");
  CHECK_THROWS_AS(create_confined_file(ws, "leaf", 0600), Error);  // symlinked leaf (even dangling)
  CHECK_FALSE(fs::exists(outside / "victim"));
}

TEST_CASE("outputs are collected only from regular files below the workspace", "[artifacts][paths]") {
  Tmp t;
  fs::path ws = t.dir / "ws", outside = t.dir / "outside";
  fs::create_directories(ws / "sub");
  fs::create_directories(outside);
  put(ws / "ok.txt", "payload");
  put(outside / "secret", "s3cret");
  fs::create_symlink(outside / "secret", ws / "leak.txt");
  fs::create_directory_symlink(outside, ws / "dirlink");
  put(ws / "sub/dir-file", "x");

  auto d = digest_confined_regular(ws, "ok.txt", 1024);
  CHECK(d.size == 7);
  CHECK(d.sha256 == sha256_hex("payload"));
  CHECK_THROWS_AS(digest_confined_regular(ws, "leak.txt", 1024), Error);            // symlink leaf
  CHECK_THROWS_AS(digest_confined_regular(ws, "dirlink/secret", 1024), Error);      // symlinked directory
  CHECK_THROWS_AS(digest_confined_regular(ws, "sub", 1024), Error);                 // directory, not a file
  CHECK_THROWS_AS(digest_confined_regular(ws, "missing.txt", 1024), Error);
  CHECK_THROWS_AS(digest_confined_regular(ws, "../outside/secret", 1024), Error);
  CHECK_THROWS_AS(digest_confined_regular(ws, "ok.txt", 3), Error);                 // over the size limit
}

TEST_CASE("verified blob ingest rejects wrong digests, wrong sizes and oversize data", "[artifacts][integrity]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 100);
  const std::string data = "hello world";
  const std::string sha = sha256_hex(data);

  SECTION("a correct upload becomes an immutable blob") {
    auto up = store.begin_upload("scope1");
    up->write(data.data(), data.size());
    store.publish(BlobKind::Output, *up, sha, data.size());
    auto p = store.find(BlobKind::Output, sha, data.size());
    REQUIRE(p);
    CHECK(slurp(*p) == data);
    CHECK((fs::status(*p).permissions() & fs::perms::owner_write) == fs::perms::none);
    store.verify(BlobKind::Output, sha, data.size());
  }
  SECTION("digest mismatch publishes nothing") {
    auto up = store.begin_upload("scope1");
    up->write(data.data(), data.size());
    CHECK_THROWS_AS(store.publish(BlobKind::Output, *up, std::string(64, '0'), data.size()), Error);
    CHECK_FALSE(store.find(BlobKind::Output, sha, data.size()));
    CHECK_FALSE(store.find(BlobKind::Output, std::string(64, '0'), data.size()));
  }
  SECTION("size mismatch publishes nothing") {
    auto up = store.begin_upload("scope1");
    up->write(data.data(), data.size());
    CHECK_THROWS_AS(store.publish(BlobKind::Output, *up, sha, data.size() + 1), Error);
    CHECK_FALSE(store.find(BlobKind::Output, sha, data.size()));
  }
  SECTION("writes beyond the artifact limit fail") {
    auto up = store.begin_upload("scope1");
    std::string big(101, 'x');
    CHECK_THROWS_AS(up->write(big.data(), big.size()), Error);
  }
  SECTION("an abandoned upload leaves no accepted blob and cleans its temp file") {
    {
      auto up = store.begin_upload("scope-abandoned");
      up->write(data.data(), data.size());
    }  // destroyed without publish: connection dropped mid-transfer
    CHECK_FALSE(store.find(BlobKind::Output, sha, data.size()));
    CHECK_FALSE(fs::exists(t.dir / "artifacts" / "tmp" / "scope-abandoned"));
  }
  SECTION("malformed digests are rejected as path-unsafe names") {
    CHECK_THROWS_AS(store.find(BlobKind::Input, "../../etc/passwd", 1), Error);
    CHECK_THROWS_AS(store.find(BlobKind::Input, "ABCDEF", 1), Error);
  }
  SECTION("corruption of a stored blob is detected on verify") {
    auto up = store.begin_upload("scope1");
    up->write(data.data(), data.size());
    store.publish(BlobKind::Input, *up, sha, data.size());
    fs::path p = *store.find(BlobKind::Input, sha, data.size());
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    std::ofstream(p, std::ios::binary | std::ios::trunc) << "HELLO WORLD";
    CHECK_THROWS_AS(store.verify(BlobKind::Input, sha, data.size()), Error);
  }
}

// ------------------------------------------------------------------ correction round 1: existing destinations

TEST_CASE("an existing corrupt same-length blob is atomically repaired from verified bytes", "[artifacts][integrity]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 1000);
  const std::string good = "abc", bad = "xyz", sha = sha256_hex(good);
  for (BlobKind kind : {BlobKind::Output, BlobKind::Input}) {
    {
      auto up = store.begin_upload("s1");
      up->write(good.data(), good.size());
      store.publish(kind, *up, sha, good.size());
    }
    fs::path p = *store.find(kind, sha, good.size());
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bad;  // same length, different content
    CHECK_THROWS_AS(store.verify(kind, sha, good.size()), Error);
    {
      auto up = store.begin_upload("s2");  // an independent, valid upload of the same content
      up->write(good.data(), good.size());
      store.publish(kind, *up, sha, good.size());
    }
    CHECK_NOTHROW(store.verify(kind, sha, good.size()));
    CHECK(slurp(*store.find(kind, sha, good.size())) == good);
    for (auto& e : fs::directory_iterator(p.parent_path())) CHECK(e.path().filename().string().find("repair") == std::string::npos);
  }
}

TEST_CASE("staged publication repairs a wrong-length destination and is idempotent for a valid one", "[artifacts][integrity]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 1000);
  const std::string good = "payload", sha = sha256_hex(good);
  auto stage = [&](const std::string& scope) {
    auto up = store.begin_upload(scope);
    up->write(good.data(), good.size());
    store.stage(*up, sha, good.size());
  };
  put(t.dir / "artifacts" / "outputs" / sha, "short");  // squatting file with the wrong length
  CHECK_FALSE(store.find(BlobKind::Output, sha, good.size()));
  stage("lease-a");
  store.publish_staged(BlobKind::Output, "lease-a", sha, good.size());
  CHECK_NOTHROW(store.verify(BlobKind::Output, sha, good.size()));
  stage("lease-b");
  store.publish_staged(BlobKind::Output, "lease-b", sha, good.size());  // already valid: kept
  CHECK_NOTHROW(store.verify(BlobKind::Output, sha, good.size()));
}

TEST_CASE("a staged file that changed after staging is never published", "[artifacts][integrity]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 1000);
  const std::string good = "payload", sha = sha256_hex(good);
  auto up = store.begin_upload("lease-x");
  up->write(good.data(), good.size());
  store.stage(*up, sha, good.size());
  fs::path staged = t.dir / "artifacts" / "tmp" / "lease-x" / (sha + ".staged");
  std::ofstream(staged, std::ios::binary | std::ios::trunc) << "PAYLOAD";
  CHECK_THROWS_AS(store.publish_staged(BlobKind::Output, "lease-x", sha, good.size()), Error);
  CHECK_FALSE(store.find(BlobKind::Output, sha, good.size()));
}

TEST_CASE("atomic file writes surface directory durability failures", "[util][durability]") {
  Tmp t;
  // Parent is a directory we cannot open for sync (no search permission after creation): the write must not report success.
  fs::path sub = t.dir / "locked";
  fs::create_directories(sub);
  write_file_atomic(sub / "ok.txt", "x", 0600);
  CHECK(slurp(sub / "ok.txt") == "x");
  CHECK_THROWS_AS(write_file_atomic(t.dir / "no-such-dir" / "f.txt", "x", 0600), Error);
}

TEST_CASE("temporary upload scopes disappear on all non-staging exits", "[artifacts][cleanup]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 10);
  for (int mode = 0; mode < 5; ++mode) {
    const auto scope = "input-" + std::to_string(mode);
    {
      auto up = store.begin_upload(scope);
      up->write("abc", 3);
      if (mode < 2) store.publish(BlobKind::Input, *up, sha256_hex("abc"), 3);
      if (mode == 2) CHECK_THROWS_AS(store.publish(BlobKind::Input, *up, sha256_hex("bad"), 3), Error);
      if (mode == 3) CHECK_THROWS_AS(up->write("0123456789", 10), Error);
    }
    CHECK_FALSE(fs::exists(store.root() / "tmp" / scope));
  }
  CHECK_NOTHROW(store.verify(BlobKind::Input, sha256_hex("abc"), 3));
}

TEST_CASE("failed upload initialization removes an empty scope", "[artifacts][cleanup]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 10);
  auto dir = store.root() / "tmp" / "unwritable";
  fs::create_directory(dir);
  fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
  CHECK_THROWS_AS(store.begin_upload("unwritable"), Error);
  CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("temporary scope reaper skips pinned IO and preserves retained stages", "[artifacts][cleanup]") {
  Tmp t;
  ArtifactStore store(t.dir / "artifacts", 10);
  auto stage = [&](const std::string& scope) {
    auto up = store.begin_upload(scope);
    up->write("abc", 3);
    store.stage(*up, sha256_hex("abc"), 3);
  };
  stage("active"); stage("valid"); stage("obsolete");
  {
    auto pin = store.pin_scope("active");
    std::async(std::launch::async, [&] {
      store.reap_scopes([](const std::string& scope) { return scope == "valid"; });
    }).get();
    CHECK(store.staged("active", sha256_hex("abc"), 3));
    CHECK(store.staged("valid", sha256_hex("abc"), 3));
    CHECK_FALSE(fs::exists(store.root() / "tmp" / "obsolete"));
  }
  store.reap_scopes([](const std::string& scope) { return scope == "valid"; });
  CHECK_FALSE(fs::exists(store.root() / "tmp" / "active"));
  CHECK(store.staged("valid", sha256_hex("abc"), 3));
}
