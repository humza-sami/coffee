# Coffee native protocol, CLI and enforcement reference (Beta 1, `coffee/v1alpha1`)

Everything here describes Coffee 0.1.0-beta.1. All demonstrations so far are **local-process
simulations on one machine over loopback**; nothing here is a multi-machine measurement.

## Transport and security

* Plain HTTP/1.1, JSON metadata, bounded binary artifact bodies. **There is no TLS in this build** (no OpenSSL
  development files were available and none were vendored). `https://` URLs are refused with an explanation.
* Authentication: every route except `GET /v1/health` requires `Authorization: Bearer <cluster token>` (the 64-hex
  token in `STATE/token`, mode 0600, compared in constant time). The token is read from a file, never from argv,
  and is never logged or echoed. Unauthorized requests get `401` + `WWW-Authenticate: Bearer` before any body is stored.
* `coffee serve` listens on `127.0.0.1:7077` by default and **refuses** a non-loopback address unless
  `--allow-plaintext-nonloopback` is given (exit 2). Clients refuse a non-loopback server unless
  `--allow-plaintext-remote` is given (exit 2). **Neither flag adds encryption**; they only acknowledge the risk.
* Encrypted remote connectivity that works with this build (documented, not automated):
  * SSH tunnel: on a Bean host `ssh -N -L 7077:127.0.0.1:7077 user@coordinator-host`, then
    `coffee join --server http://127.0.0.1:7077 --token-file TOKEN --work-dir DIR`. Copy the token out of band (e.g. `scp`).
  * WireGuard/Tailscale: bind the coordinator to the overlay address with `--listen 100.x.y.z:7077
    --allow-plaintext-nonloopback` and point Beans/CLI at `http://100.x.y.z:7077 --allow-plaintext-remote`. Only do this when
    every peer on that overlay is trusted: the token and artifacts cross the overlay in clear HTTP inside the encrypted tunnel.
  * Native HTTPS is **not implemented**; it needs a TLS library and is a later task.
* Trust model: one shared cluster token, trusted owner-controlled Beans. No per-Bean credentials, no multi-tenant isolation,
  no brute-force throttling of the token (the token is 256 random bits).

## Network limits

JSON bodies ≤ 4 MiB (`413 too_large`); artifacts ≤ `--max-artifact-bytes` (default 256 MiB, streamed, never buffered);
logs ≤ 1 MiB per stream; socket read timeout 15 s, write timeout 30 s, keep-alive 5 s/100 requests; request threads
`--threads` (8, growing to 4x) with at most 128 queued requests; per task ≤ 256 inputs and ≤ 256 outputs, ≤ 4096 tasks per
workload. There is no coordinator-side global disk quota and no staged-input garbage collection (blobs are kept); Bean scratch is bounded per attempt by `--max-workspace-bytes`.

## Errors

Every error is `{"error":{"code":"…","message":"…","field":"…"?}}`. Codes → status: `invalid_json`, `invalid_argument`,
`invalid_workload` (+`field`), `invalid_outputs`, `digest_mismatch` → 400; `unauthorized` → 401; `wrong_bean` → 403;
`unknown_bean|unknown_job|unknown_task|unknown_lease|no_such_input|no_such_output|no_logs|not_found` → 404;
`conflict`, `missing_input` (+`missing[]`), `missing_upload`, `expired`, `deadline_exceeded`, `cancelled`, `not_running` → 409;
`too_large` → 413; `artifact_corrupt` and `internal_error` → 500. Internal details are logged server-side only.

## Routes (all under `/v1`)

| Method & path | Auth | Body → response | Statuses |
| --- | --- | --- | --- |
| `GET /health` | no | → `{"status":"ok"}` | 200 |
| `GET /version` | yes | → version, api_version, cluster_id, lease_ms, heartbeat_timeout_ms, limits, transport | 200 |
| `POST /beans` | yes | `{id?,name?,cpus,memory_mb,attributes?}` → Bean | 200, 400 |
| `GET /beans` | yes | → `{"beans":[Bean]}` (cpus_used, memory_mb_used, state online/unreachable/draining) | 200 |
| `POST /beans/{id}/heartbeat` | yes | → `{"ok":true}` | 200, 404 `unknown_bean` (register again) |
| `POST /beans/{id}/drain` | yes | `{"draining":bool=true}` → Bean | 200, 404 |
| `POST /beans/{id}/claim` | yes | → `{"claim":null \| Claim}` | 200, 404 |
| `PUT /inputs/{sha256}` | yes | raw bytes (digest and size verified while streaming; the body is always received) → `{sha256,size,created,repaired?}`. The blob is published through the common verified path: file **and** parent-directory fsync must succeed, also when the digest already exists (an existing valid blob is not proof of an earlier successful sync) | 201 new, 200 existing/repaired, 400 `digest_mismatch`, 413, 500 `internal_error` when a required sync fails |
| `POST /jobs` | yes | `{job_id?,workload,files:{source:{sha256,size}}}` → `{job_id,created,task_count}`; every referenced input is re-verified and fsynced (file + directory) before the job is recorded (`missing_input` if absent/damaged, 500 and nothing recorded if a sync fails) | 201, 200 identical resubmit, 400, 409 `conflict`/`missing_input`, 413 |
| `GET /jobs`, `GET /jobs/{id}` | yes | job list / job + tasks + attempt history (no lease ids) | 200, 404 |
| `POST /jobs/{id}/cancel` | yes | `{task_id?,reason?}` → `{job_state,tasks_cancelled,attempts_cancelled}` | 200, 404 |
| `GET /jobs/{id}/tasks/{t}/explain` | yes | dependency blockers + per-Bean fit | 200, 404 |
| `GET /jobs/{id}/tasks/{t}/logs?stream=stdout\|stderr&attempt=N` | yes | text (≤ 1 MiB) | 200, 404 `no_logs` |
| `GET /jobs/{id}/tasks/{t}/outputs?path=REL` | yes | accepted output bytes + `X-Coffee-Sha256` (re-verified) | 200, 404, 500 `artifact_corrupt` |
| `POST /leases/{lease}/renew` | yes | `{bean_id}` → `{status,lease_expires_at_ms,lease_ms}` | 200, 403, 404, 409 |
| `GET /leases/{lease}/inputs/{i}` | yes + `X-Coffee-Bean` | verified input *i* of the claim | 200, 403, 404, 409, 500 |
| `PUT /leases/{lease}/uploads/{sha256}` | yes + `X-Coffee-Bean` | raw bytes staged for this attempt → `{sha256,size}` | 201, 400, 403, 404, 409, 413 |
| `PUT /leases/{lease}/logs/{stdout\|stderr}` | yes + `X-Coffee-Bean` | text ≤ 1 MiB | 200, 403, 404, 409, 413 |
| `POST /leases/{lease}/complete` | yes | `{bean_id,success,exit_code,reason?,retryable?,outputs:[{path,size,sha256}]}` → `{status,task_state}` | 200, 400 `invalid_outputs`, 403, 404, 409 |

Claim: `{job_id,task_id,attempt,lease_id,lease_expires_at_ms,lease_ms,deadline_at_ms,cpus,memory_mb,timeout_seconds,task,inputs}`
where `inputs[i]` is `{kind:"file"|"dependency",target,sha256,size,(source|from_task+output)}`. The lease id is a bearer
secret for that attempt: it selects what may be downloaded, so a caller never names an arbitrary artifact path.

### Fenced publication

1. The Bean uploads each output to `PUT /leases/{lease}/uploads/{sha256}`. The server checks the lease first, hashes while
   streaming into `artifacts/tmp/<lease>/`, checks the lease **again** after the last byte (cancellation/expiry during the
   transfer ⇒ `409`, nothing staged), then stages the verified file. Dropped connections leave no staged file.
2. `POST …/complete` validates the outputs equal the task's declared outputs exactly, checks the lease, and requires every digest
   to be staged for **this** lease. The slow preparation then runs **outside** the Store mutex and SQLite transaction: each staged
   file is re-hashed, its data fsynced, hard-linked as an immutable `artifacts/outputs/<sha256>` blob (mode 0444) and the parent
   directory fsynced. Every required open/chmod/fsync/link failure aborts the request (HTTP 500, nothing accepted; the attempt's
   lease later expires and the task retries or fails). Only afterwards does the short fenced `Store::complete` transaction
   re-check lease, clock and deadline and record the references. If the lease expired or was cancelled during preparation the
   completion is refused (409) and the already-durable blobs simply stay unreferenced. A slow or stalled filesystem therefore
   delays only that publication; renewals and other completions proceed (regression: `test_faults.py`, injected 4–5 s directory
   fsync). Beans keep renewing their lease while `complete` is in flight, so a slow-but-progressing publication is not expired by it.
   *Existing destinations:* if `outputs/<sha256>` (or `inputs/<sha256>`) already exists it is read back and verified; a valid blob
   is kept (idempotent), a damaged or wrong-length one is atomically replaced (hard-link to a temp name, `rename`) with the verified
   bytes before the new reference is accepted. `PUT /inputs/{sha}` answers `200 {"repaired":true}` in that case.
3. Only accepted results are readable (`GET …/outputs`) or resolvable as dependency inputs. Dependency inputs come from the
   producer's committed result of the successful attempt, never from an attempt workspace.

## Workload schema amendment: dependency outputs

Task `inputs[]` entries are now either `{source, target}` (a local file next to the workload) or
`{from_task: ID, output: REL_PATH, target: REL_PATH}`. Exactly one of `source`/`from_task`; `output` only with `from_task`.
Rules: the producer must be in the consumer's explicit `depends_on` and `output` must exactly equal one of the producer's
declared `outputs`; a task cannot consume itself; targets follow the existing safe-path, duplicate and file-vs-directory
conflict rules; the whole DAG is validated before anything is stored. Normalized form keeps the three keys. See
[examples/pipeline/coffee.yaml](../examples/pipeline/coffee.yaml). A failed producer fails the consumer
(`dependency_failed: <id>`) and the consumer never runs.

## CLI

`coffee init|validate` plus:

| Command | Key flags | Notes |
| --- | --- | --- |
| `serve` | `--state DIR` `--listen HOST:PORT` `--allow-plaintext-nonloopback` `--lease-ms` `--heartbeat-timeout-ms` `--deadline-grace-ms` `--max-artifact-bytes` `--threads` `--ready-file F` | SIGINT/SIGTERM stop cleanly; port 0 = ephemeral |
| `join` | `--work-dir DIR` `--id` `--name` `--cpus` `--memory-mb` `--heartbeat-ms` `--poll-ms` `--max-artifact-bytes` `--max-workspace-bytes` `--keep-attempts` + connection flags | Bean id persisted in `DIR/bean-id`; re-registering the same id abandons its previous attempts |
| `beans` / `drain BEAN [--undo]` | connection flags | drain: no new claims, running attempts finish |
| `plan FILE` | `--json`, optional `--server` | layers, resources; with a server it flags tasks no Bean can ever fit |
| `brew FILE` | `--job-id ID` `--wait` `--timeout S` `--out DIR` `--poll-ms` | uploads inputs (digest-verified), submits; `--wait` follows to the end; `--out` downloads verified accepted outputs |
| `status [JOB]` / `logs JOB TASK [--attempt N] [--stderr]` / `explain JOB TASK` / `cancel JOB [--task T] [--reason R]` | connection flags | |
| `prepare` | | **not implemented**, exit 3 |

Connection flags: `--server URL` (`$COFFEE_SERVER`, default `http://127.0.0.1:7077`), `--token-file FILE` (`$COFFEE_TOKEN_FILE`) or
`--state DIR`, `--allow-plaintext-remote`, `--json`. Exit codes: 0 ok; 1 operation failed / job failed or cancelled; 2 usage
(including refused insecure transport and missing credentials); 3 not implemented; 4 coordinator unreachable, unauthorized or
server error; 5 `brew --wait` timed out (the job keeps running). `coffee __supervise` is an internal command spawned by Beans.

## Beans, supervision and the resources actually enforced

The Bean is multi-threaded, so it never runs complex code between `fork` and `exec`. It uses `posix_spawn` to start
`coffee __supervise` (a fresh single-threaded image) per attempt. The supervisor:

| Mechanism | What it enforces | What it does **not** claim |
| --- | --- | --- |
| Wall timeout (`timeout_seconds`) in the supervisor, plus coordinator deadline (`timeout + 30 s`) | whole tree SIGKILLed; attempt reported "timed out" | |
| `RLIMIT_AS = memory_mb` on the command (inherited by descendants) | allocations beyond the address-space budget fail | not an RSS/cgroup quota; each process has its own limit; the Bean's `--memory-mb` budget is accounting only |
| `sched_setaffinity` to `cpus` distinct CPU ids owned by the claim | task confined to its CPU slots | not a CPU-time quota; slots are accounting + pinning, not physical pooling |
| `RLIMIT_FSIZE = --max-artifact-bytes`, `RLIMIT_CORE=0`, `RLIMIT_NOFILE=1024` | one file ≤ limit (SIGXFSZ otherwise); no cores | |
| **Aggregate workspace cap** `coffee join --max-workspace-bytes N` (default 1 GiB, 0 rejected): declared inputs are checked before download; about every 100 ms the supervisor measures the workspace and kills the tree when it exceeds N; it **measures again after the whole tree is dead before success is reported**. A measurement (`include/coffee/workspace.hpp`) is the set of *distinct* regular files (de-duplicated by device+inode across hard links, several fds, several processes and both views) made of (a) every file below the workspace found by a no-follow directory walk and (b) every regular file held open by the command **and its descendants** whose `/proc/<pid>/fd` link resolves below the workspace, **including files already unlinked while open** (`… (deleted)`, `O_TMPFILE`); size = `max(st_size, allocated)` | attempt fails non-retryably with `workspace limit exceeded: … (measured M bytes, limit N bytes per attempt)`; a fast writer that finishes between samples is still rejected if its files remain; unreadable directories or processes (a process that is merely inside `execve()` answers `EACCES` for a moment and is retried for up to about 50 ms before failing closed), depth > 128, > 200 000 entries, > 200 000 fds or > 4096 processes fail closed; vanished processes/fds and ordinary create-unlink temp files below the cap never cause failure | userspace sampling, **not a filesystem quota or sandbox**: a writer can overshoot by about (write rate × (100 ms interval + scan time + scheduling delay)) before it is killed; an unlinked file that is filled and released between two samples is not observed (its space is freed at that moment); descriptors pointing outside the workspace, files renamed out while open, `/tmp` and other absolute-path writes, memfd/anonymous memory (bounded by `RLIMIT_AS` instead) are not counted; the limit is **per attempt** (a Bean running k attempts can use k × N; no cross-Bean disk accounting, no workload-level disk resource) |
| stdout/stderr through pipes into bounded files (1 MiB each, rest discarded, flagged truncated) | bounded log storage | |
| Cancellation: the Bean renews each lease every `--heartbeat-ms`; a refused renewal (cancelled/expired) or local lease expiry cancels the command | command stops within about one heartbeat of cancel | |
| Bean `SIGKILL`: the supervisor (child subreaper, `PR_SET_PDEATHSIG(SIGTERM)`, liveness pipe EOF) kills the command's process group **and every descendant found by parent-pid ancestry** | command and ordinary children stop (tested), including `setsid()`-detached descendants that were re-parented to the supervisor | if the *supervisor itself* is SIGKILLed, only the direct command stops (PDEATHSIG); its descendants may survive. Fork bombs, `ptrace`, and processes that change user/namespace are out of scope. No cgroup delegation is used |
| Command environment is rebuilt (`PATH`, `HOME`=workspace, `LANG`, `COFFEE_*` task ids); the Bean's own environment and the token are not passed | | the command runs as the Bean's user: this is resource enforcement for trusted owners, **not** a security sandbox |

Other Bean behaviour: staged inputs are created with `openat(O_NOFOLLOW|O_EXCL)` below the attempt workspace
(`attempts/<job>.<task>.<n>.<rand>/work`, mode 0700), digest- and size-verified; declared outputs must be regular files
(symlink components or leaf, directories, missing files, oversize files fail the attempt; deterministic problems are
non-retryable); workspaces are deleted after the attempt (`--keep-attempts` to keep). A Bean stopped with SIGTERM cancels its
commands and returns the attempts as retryable failures. A hard-linked file that the command could already read is still
readable by the command (trusted-owner scope).

## Unsupported / not done

HTTPS; per-Bean credentials; cgroup memory/CPU accounting; coordinator HA/failover; garbage collection of unreferenced blobs or
old jobs; `coffee prepare`; resumable partial uploads; multi-machine measurements; Windows/macOS.
