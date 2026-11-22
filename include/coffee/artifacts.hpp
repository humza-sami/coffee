// Coffee artifacts: coordinator-side content-addressed blob store with quarantined uploads, and the
// worker-side symlink-safe workspace file operations.
//
// Coordinator layout (DIR/artifacts): inputs/<sha256> and outputs/<sha256> are immutable (mode 0444) blobs
// published by hard-link-without-overwrite after the content was re-hashed; tmp/<scope>/ holds in-flight
// uploads and staged (verified, not yet accepted) results. Nothing in tmp/ is ever referenced by the
// database. A crash can leave unreferenced files, never a referenced partial one.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "coffee/util.hpp"

namespace coffee {

enum class BlobKind { Input, Output };

// True for exactly 64 lowercase hex characters.
bool is_sha256_hex(std::string_view s);

class ArtifactStore;

// What publication found at the destination (the blob is verified and durable in every case).
enum class PublishOutcome { Created, AlreadyPresent, Repaired };

// A streaming upload into quarantine. Destroying it without publish()/stage() removes the temp file, so an
// interrupted transfer leaves nothing behind.
class Upload {
 public:
  ~Upload();
  Upload(const Upload&) = delete;
  Upload& operator=(const Upload&) = delete;
  // Throws Error("too_large") once the artifact limit is exceeded, Error("io_error") on write failure.
  void write(const char* data, size_t len);
  uint64_t size() const { return size_; }

 private:
  friend class ArtifactStore;
  Upload(std::filesystem::path scope_dir, uint64_t limit);
  std::filesystem::path scope_dir_, path_;
  int fd_ = -1;
  uint64_t size_ = 0, limit_ = 0;
  std::unique_ptr<Sha256Stream> hash_;
};

class ArtifactStore {
 public:
  // Creates artifacts_dir and its inputs/outputs/logs/tmp subdirectories (0700) if absent.
  ArtifactStore(std::filesystem::path artifacts_dir, uint64_t max_artifact_bytes);

  uint64_t max_artifact_bytes() const { return max_bytes_; }
  const std::filesystem::path& root() const { return root_; }

  // Serialize transfers/completion for one scope. The reaper only tries this lock; it never
  // waits behind network/disk work. Callers acquire it before consulting Store, never inside Store.
  class ScopePin {
   public:
    ScopePin(std::shared_ptr<std::mutex> mutex, bool try_only);
    bool owns_lock() const { return lock_.owns_lock(); }
   private:
    std::shared_ptr<std::mutex> mutex_;
    std::unique_lock<std::mutex> lock_;
  };
  ScopePin pin_scope(const std::string& scope, bool try_only = false);
  // Scan only temporary scopes. Skip active operations, then retain only scopes for which keep
  // returns true. Callback may read Store; no Store mutex remains held during disk removal.
  void reap_scopes(const std::function<bool(const std::string&)>& keep);

  // `scope` is a safe name ([A-Za-z0-9._-]+) isolating one transfer owner (e.g. a lease id).
  std::unique_ptr<Upload> begin_upload(const std::string& scope);

  // Verify the streamed digest/size against the claim and publish an immutable blob. An existing blob with
  // the same digest is kept. Throws Error("digest_mismatch") / Error("io_error"); nothing is published then.
  PublishOutcome publish(BlobKind kind, Upload& upload, const std::string& sha256, uint64_t size);

  // Verify and move an upload into the scope's staging area (still unaccepted). Same errors as publish.
  void stage(Upload& upload, const std::string& sha256, uint64_t size);
  bool staged(const std::string& scope, const std::string& sha256, uint64_t size);
  // Re-hash the staged file, then publish it as an immutable output (idempotent). Error("missing_upload")
  // if not staged, Error("digest_mismatch") if it changed since staging.
  PublishOutcome publish_staged(BlobKind kind, const std::string& scope, const std::string& sha256, uint64_t size);
  void discard_scope(const std::string& scope);  // removes the scope's temp files; idempotent

  // Path of a published blob if it exists with this size (not re-hashed). Throws Error("invalid_argument")
  // for a malformed digest.
  std::optional<std::filesystem::path> find(BlobKind kind, const std::string& sha256, uint64_t size);
  // Establish that an existing published blob is intact AND durable (data fsync + parent directory fsync) before it is
  // referenced. Returns false when it is absent or its content/size differ (the caller must have it uploaded again);
  // throws Error("io_error") when a required sync fails. A blob that merely exists and hashes correctly is not proof
  // that an earlier publication ever completed its synchronization.
  bool ensure_durable(BlobKind kind, const std::string& sha256, uint64_t size);
  // Re-hash a published blob. Throws Error("artifact_corrupt") if it is missing, resized or altered.
  void verify(BlobKind kind, const std::string& sha256, uint64_t size);

  // Per-attempt log files below logs/<job>/<task>/ (ids must be valid names). Written atomically.
  std::filesystem::path log_path(const std::string& job_id, const std::string& task_id, int attempt, const std::string& stream);
  void write_log(const std::string& job_id, const std::string& task_id, int attempt, const std::string& stream, std::string_view data);

 private:
  std::filesystem::path blob_path(BlobKind kind, const std::string& sha256) const;
  std::filesystem::path scope_dir(const std::string& scope) const;
  std::filesystem::path root_;
  uint64_t max_bytes_;
  std::mutex scopes_mutex_;
  std::map<std::string, std::weak_ptr<std::mutex>> scopes_;
};

// ---- worker-side workspace operations (never follow symlinks; every path is checked with
// check_safe_relative_path and each directory component is opened O_NOFOLLOW | O_DIRECTORY)

struct FileDigest {
  uint64_t size = 0;
  std::string sha256;
};

// Create `rel` below `root` exclusively (O_EXCL | O_NOFOLLOW), creating missing parent directories (0700).
// Returns a writable fd; throws Error("unsafe_path") for traversal/symlinks, Error("conflict") if it exists.
int create_confined_file(const std::filesystem::path& root, std::string_view rel, unsigned mode);

// Open an existing regular file below root for reading. Throws Error("unsafe_path") for symlinks and
// non-regular files, Error("not_found") if missing. `size_out` receives st_size.
int open_confined_regular(const std::filesystem::path& root, std::string_view rel, uint64_t* size_out = nullptr);

// open_confined_regular + size limit + SHA-256. Throws Error("too_large") above max_bytes.
FileDigest digest_confined_regular(const std::filesystem::path& root, std::string_view rel, uint64_t max_bytes);

}  // namespace coffee
