// Coffee coordinator: authenticated, bounded HTTP/1.1 service over the durable Store and ArtifactStore.
//
// Security model: one cluster bearer token (state DIR/token). This build has no TLS: the service listens on
// loopback by default and refuses a non-loopback address unless `allow_plaintext_nonloopback` is set, which
// provides NO encryption by itself; use an encrypted overlay (SSH tunnel, WireGuard, Tailscale).
// All limits are explicit: JSON bodies kMaxJsonBytes, artifacts max_artifact_bytes, worker threads and queue
// depth, per-socket read/write timeouts. Lease ids and the token are never logged.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "coffee/protocol.hpp"
#include "coffee/store.hpp"

namespace coffee {

struct CoordinatorOptions {
  std::filesystem::path state_dir;
  std::string host = "127.0.0.1";
  int port = protocol::kDefaultPort;        // 0 = ephemeral
  bool allow_plaintext_nonloopback = false;
  int64_t lease_ms = 10'000;
  int64_t heartbeat_timeout_ms = 10'000;
  int64_t deadline_grace_ms = 30'000;
  uint64_t max_artifact_bytes = protocol::kDefaultMaxArtifactBytes;
  size_t worker_threads = 8;                // base request threads (grows to 4x under load)
  size_t max_queued_requests = 128;         // beyond this, new connections are dropped
  int read_timeout_s = 15;
  int write_timeout_s = 30;
  std::shared_ptr<const Clock> clock;       // null -> SystemClock
};

class Coordinator {
 public:
  // Opens the state directory (must be initialized). Throws Error("insecure_listen") for a plaintext
  // non-loopback address without the override, Error("not_found") for a missing state directory.
  explicit Coordinator(CoordinatorOptions options);
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Binds and starts serving on background threads; returns the bound port. Throws Error("io_error") when the
  // address cannot be bound.
  int start();
  void stop();   // idempotent; waits for in-flight requests
  Store& store();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace coffee
