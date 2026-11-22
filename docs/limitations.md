# Limitations of Coffee 0.1.0-alpha.1

This list is deliberately plain. "Unsupported" means there is no code and no evidence; "untested" means code exists but this
release has no test showing it works.

## Not supported (no code, no evidence)

* **Pooled RAM / a larger address space / one big virtual machine.** Each task process must fit one Bean's budget.
* **Transparent distributed threads** or shared memory between Beans.
* **Durable pooled disk.** Artifacts live in the coordinator's state directory (and Bean scratch); nothing is striped or replicated.
* **Automatic coordinator HA/failover.** One coordinator; restart it from its state directory.
* **Built-in encryption (TLS/HTTPS).** Use an SSH tunnel or an overlay; see [operations](operations.md).
* **Multi-tenant isolation / sandboxing / per-user credentials.** One shared token, trusted owner, commands run as the Bean user.
* **Hard resource quotas** (cgroup memory/CPU/disk). `RLIMIT_AS`, CPU pinning and a sampled per-attempt scratch cap prevent accidents
  and are not quotas ([enforcement table](native-protocol.md#beans-supervision-and-the-resources-actually-enforced)).
* `coffee prepare` (exits 3), Windows/macOS, resumable partial uploads, blob/job garbage collection.

## Evidence that does not exist yet

* **No physical multi-device run**, no WAN or real-network failure test, no remote-host SSH/WireGuard/Tailscale run. The
  documented recipes follow the implemented flags only.
* **No power-loss or disk-full test.** Durability checks use fault injection (`EIO` on `fsync`, injected delays), not real crashes.
* **Benchmarks are local-process simulations.** Two Beans on one machine share one CPU package, memory bus and page cache; they are
  not two devices ([benchmarking](benchmarking.md)). No C++ vs Rust or other language performance winner is claimed.
* **CI is configured, not run.** `.github/workflows/ci.yml` has never executed on a hosted runner as of this release. Only Ubuntu
  26.04 (GCC 15.2, local) was exercised; Ubuntu 24.04 and other matrices are configured but unverified. A locally built binary is
  not shown to run on older distributions (it links the build host's `libstdc++`/`libc`).

## Known behaviours and gaps

* **Exec-window race (fixed in this release):** the first benchmark attempt was killed by a spurious `cannot inspect the open files of
  process N (Permission denied)`: while a process is inside `execve()` its `/proc/<pid>/fd` is briefly unreadable and the scratch-cap
  sampler failed closed. A unit regression reproduced it (1–2 spurious failures in ~63 000 samples of a process that re-execs itself) and
  the sampler now retries for up to ~50 ms. A process that stays unreadable longer is still killed fail-closed.

* **Supervisor `SIGKILL`:** if the supervisor process itself is killed, only the command is stopped; its descendants may survive.
  Deliberate escapes (`setsid`, double fork) are cleaned up only while the supervisor is alive; fork bombs, `ptrace`, namespace tricks
  are out of scope.
* **Scratch cap** is sampled every 100 ms (plus a final check): a writer can overshoot before being killed; writes outside the
  workspace (`/tmp`, absolute paths) are not counted; the cap is per attempt, with no cross-Bean disk accounting.
* **Connection pressure:** the coordinator has 8 base request threads and a 128-request queue. In a local test, 40 idle TCP
  connections delayed the next authenticated request by about 5 s (keep-alive timeout) before service resumed; it did not crash.
  There is no per-client throttling: keep the port private.
* **Input re-verification cost:** `POST /v1/jobs` re-hashes every referenced input; each `PUT /v1/inputs` receives the full body.
* **State from the pre-network build:** upgraded to schema v2 correctly, but its jobs never uploaded inputs, so their unfinished
  tasks fail with `input '…' has no recorded digest`.
* **Temporary transfer cleanup:** empty upload scopes are removed when the upload ends, including rejected or interrupted
  uploads. The coordinator scans temporary scopes after each lease-reaper pass (nominal pause: lease duration / 4, clamped
  to 50–500 ms), including on startup. It retains valid leases with their original expiry and skips active uploads/completions;
  cancelled, expired, abandoned, terminal and unknown scopes are removed once idle. Cleanup retries on later scans after IO
  failures. Scans and deletion take filesystem time, so this is eventual reclamation, with no hard wall-clock or disk quota
  guarantee. Slow active transfers can defer reclamation until they finish or hit the configured read timeout. Disk work runs
  outside the Store mutex, so renewals do not wait for cleanup IO.
* **Unreferenced immutable blobs** left by crashes or lost publication races are never reclaimed; temporary cleanup does not
  remove accepted blobs, attempt logs or history.
* A Bean that gets `404` on heartbeat (coordinator lost all state) re-registers and abandons its own running attempts. Not tested end to
  end.
* **Timing-based tests** use real 3 s leases; on a heavily loaded host they may need larger margins.
* **Resource numbers** (RSS, CPU ticks) in the benchmark are best-effort `/proc` readings of the Coffee processes; per-task RSS inside
  Coffee is not recorded.
