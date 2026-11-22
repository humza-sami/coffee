// Coffee utilities: errors, injectable clock, randomness, hashing, safe relative paths.
// All functions are thread-safe unless stated otherwise.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace coffee {

inline constexpr const char* kVersion = "0.1.0-alpha.1";
inline constexpr const char* kApiVersion = "coffee/v1alpha1";

// Controlled error. `code` is a stable machine-readable token, e.g.
// "invalid_argument", "not_found", "conflict", "io_error", "db_error", "invalid_workload".
class Error : public std::runtime_error {
 public:
  Error(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

// Time source in milliseconds since the Unix epoch. Durable timestamps use this
// scale so they remain comparable across coordinator restarts.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual int64_t now_ms() const = 0;
};

class SystemClock final : public Clock {
 public:
  int64_t now_ms() const override;
};

// Deterministic clock for tests. Thread-safe.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(int64_t start_ms = 1'000'000) : now_(start_ms) {}
  int64_t now_ms() const override { return now_.load(); }
  void advance(int64_t delta_ms) { now_.fetch_add(delta_ms); }
  void set(int64_t ms) { now_.store(ms); }

 private:
  std::atomic<int64_t> now_;
};

// `nbytes` of operating-system randomness (getrandom) as lowercase hex. Throws Error on failure.
std::string random_hex(size_t nbytes);

// Incremental SHA-256 (streaming transfers). finish() may be called once.
class Sha256Stream {
 public:
  Sha256Stream();
  ~Sha256Stream();
  Sha256Stream(const Sha256Stream&) = delete;
  Sha256Stream& operator=(const Sha256Stream&) = delete;
  void update(const void* data, size_t len);
  std::string finish();  // lowercase hex

 private:
  struct Impl;
  Impl* impl_;
};

// SHA-256 of a buffer / file as lowercase hex. sha256_file throws Error("io_error") on failure.
std::string sha256_hex(std::string_view data);
std::string sha256_file(const std::filesystem::path& path);

// Strict safe relative path check used for task inputs, targets and outputs.
// A safe path is non-empty, relative, uses '/' separators, has no "", "." or ".." components,
// no trailing slash, no control characters or backslashes, components <= 255 bytes, total <= 1024.
// Returns std::nullopt when safe, otherwise a human-readable reason.
std::optional<std::string> check_safe_relative_path(std::string_view path);

// Resolve `relative` below `root` and verify that the real location (following symlinks of existing
// components) stays inside `root`. Throws Error("invalid_argument") on traversal/escape.
// The result is not required to exist.
std::filesystem::path confine_path(const std::filesystem::path& root, std::string_view relative);

// Read a whole regular file, refusing files larger than `max_bytes`.
// Throws Error("io_error") / Error("too_large").
std::string read_file_limited(const std::filesystem::path& path, size_t max_bytes);

// Write `data` to `path` atomically (temp file + fsync + rename) with the given POSIX mode.
void write_file_atomic(const std::filesystem::path& path, std::string_view data, unsigned mode = 0600);

}  // namespace coffee
