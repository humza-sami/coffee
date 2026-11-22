# Local benchmark: direct processes vs one and two Coffee Beans

> **This is a local-process simulation on one machine.** Two Beans on one host share one CPU package, memory system and page cache;
> they are not two devices. No network, WAN, second machine, pooled memory, transparent threading or durable pooled disk is
> measured. Absolute numbers apply to this host and workload only. No C++ vs Rust (or other language) comparison is made or implied.

## Question

How much does Coffee cost, and what does a second Bean buy, for a deterministic CPU + file workload when the *total CPU budget is
held equal*? Runs that change the total CPU budget are labelled "scaling" and are never presented as overhead comparisons.

## Setup (all values from `docs/verification/benchmark-2026-11-22/environment.json`)

| Item | Value |
| --- | --- |
| Measurement source | Archival internal revision `6424c5b8b0d9057a3b14c34ebfb2f5ba914ef353`, clean worktree (`git_dirty_files: 0`); not a revision in the public repository. [Source checksums](verification/benchmark-2026-11-22/source-sha256.json) identify recorded source contents; no contemporaneous binary digest was recorded. Harness phase-timing fields were added afterwards. |
| Host | Ubuntu 26.04.1, kernel 7.0.0-43, i5-12450H (12 logical CPUs), 31 GiB RAM, `powersave` governor, ~13 GiB free disk (not stressed) |
| Build | separate directory `build-release`: `-DCMAKE_BUILD_TYPE=Release` = `-O3 -DNDEBUG`, g++ 15.2.0, C++20, no `-march=native`, no sanitizers |
| Workload | 8 independent tasks; each reads one 4 MiB deterministic input (seeded `random.Random`), runs 280 integer-mixing rounds (`bench/work.cpp`, about 0.95 s on one E-core), writes a 64 KiB output; every output SHA-256 must equal the reference or the run aborts |
| Task CPUs | logical CPUs 8–11 (the four single-thread E-cores: homogeneous, no SMT siblings). Disjoint masks: `taskset -c` per Bean (Bean 0 → CPU 8[,9], Bean 1 → CPU 10[,11]); the Bean pins each running slot to a distinct allowed CPU |
| Outside every budget | coordinator, `coffee brew`, and the harness are pinned to CPU 4 (a P-core), so coordination does not steal task CPUs; Bean and supervisor processes *do* run on the task CPUs and are counted as Coffee overhead |
| Limits | each task `memory_mb: 128` (`RLIMIT_AS`), `timeout_seconds: 120`, `max_attempts: 1`; Beans advertise 2048 MiB (accounting only) |
| Polling | Bean `--poll-ms 50`, `coffee brew --poll-ms 50`, heartbeat 1 s (the defaults, 500 ms Bean poll, add up to 0.5 s of claim latency per task and would inflate Coffee's numbers) |
| Cgroup | process cgroup is the user's Chromium `app-*.scope`; no `cpu.max`/`memory.max` limit anywhere on the ancestry (`max`); `cpuset.cpus.effective` 0-11 |
| Protocol | 1 warm-up repetition (recorded, excluded) + 5 measured repetitions of every configuration; the order of the 7 configurations is shuffled per repetition (seed recorded in `raw.jsonl` `order_in_rep`); each Coffee run starts a fresh state, coordinator and Beans, waits for registration, then times `coffee brew --wait --out` (upload of 32 MiB, submit, run, verified download) |
| Caveat | background load was present (`loadavg` 3.2 at start, 4.5 at end; a Chromium session was running on the same machine) and the task CPUs were not isolated from other processes |

Configurations (CPU budget = number of task CPUs):

| Name | Budget | What |
| --- | --- | --- |
| `direct-1cpu` / `direct-2cpu` / `direct-4cpu` | 1 / 2 / 4 | strongest simple local baseline: a pool of *N* slots, each a `taskset`-pinned process on its own CPU, tasks fed dynamically; no coordinator, no transfer |
| `coffee-1bean-x1slot` | 1 | one Bean with one slot |
| `coffee-1bean-x2slot` | 2 | one Bean with two slots |
| `coffee-2bean-x1slot` | 2 | two Beans with one slot each |
| `coffee-2bean-x2slot` | 4 | two Beans with two slots each (**scaling**: budget doubled again) |

## Results (wall seconds, 5 measured repetitions, raw values in `raw.jsonl`)

| Config | Budget | Median | Mean ± sd | Min – max | Raw (repetition order) |
| --- | --- | --- | --- | --- | --- |
| direct-1cpu | 1 | **7.721** | 7.802 ± 0.240 | 7.582 – 8.110 | 7.599, 8.110, 7.999, 7.721, 7.582 |
| coffee-1bean-x1slot | 1 | **10.040** | 9.988 ± 0.407 | 9.430 – 10.381 | 9.430, 10.351, 10.381, 9.737, 10.040 |
| direct-2cpu | 2 | **4.011** | 4.076 ± 0.133 | 3.964 – 4.296 | 4.105, 4.296, 3.964, 4.006, 4.011 |
| coffee-1bean-x2slot | 2 | **5.584** | 5.590 ± 0.217 | 5.310 – 5.917 | 5.310, 5.917, 5.544, 5.597, 5.584 |
| coffee-2bean-x1slot | 2 | **5.731** | 5.731 ± 0.166 | 5.562 – 5.994 | 5.731, 5.994, 5.749, 5.562, 5.618 |
| direct-4cpu | 4 | **2.284** | 2.274 ± 0.074 | 2.161 – 2.341 | 2.284, 2.341, 2.250, 2.335, 2.161 |
| coffee-2bean-x2slot | 4 | **3.492** | 3.530 ± 0.441 | 3.179 – 4.266 | 3.517, 4.266, 3.492, 3.194, 3.179 |

Every one of the 42 runs (including warm-ups) produced outputs identical to the reference digests, 8 successful attempts and no retries;
with two Beans the attempts split 4/4 every time.

**Equal-budget comparison (overhead):**
* Budget 1: Coffee costs about **+2.3 s (+30 %)** over the direct pinned process on this 8 × ~1 s workload.
* Budget 2: Coffee costs about **+1.6 s (+39 %)** (one Bean × 2 slots) / **+1.7 s (+43 %)** (two Beans × 1 slot). The two Coffee layouts differ by
  0.15 s with run-to-run spread of ±0.2 s, so this experiment does **not** show a difference between "one Bean, two slots" and "two Beans, one slot each".
* Budget 4 (two Beans × 2 slots): about +1.2 s (+53 %) over `direct-4cpu`, with the widest spread (one repetition at 4.27 s).

**Scaling (budget changes, not an overhead result):** from the 1-CPU run to the 2-CPU, two-Bean run the median falls 10.04 → 5.73 s (1.75×); to the
4-CPU, two-Bean run 3.49 s (2.9×). The direct baseline scales 7.72 → 4.01 (1.93×) → 2.28 s (3.4×). Two Beans scale because they were given more CPUs,
not because of anything specific to Beans.

**Honest reading:** with short (~1 s) tasks and 4 MiB inputs, Coffee is *slower* than plain pinned processes. A job-level phase probe (single run,
`phase-probe-raw.jsonl`, not part of the five-repetition table) attributes roughly 0.4–0.46 s to hashing/uploading/fsyncing the 32 MiB of inputs and submitting,
about 0.03–0.06 s to the final wait/download, and the rest to execution in which each attempt took about 1.13 s on the 1×1 layout versus about 0.96 s
of pure compute (input download + verification, supervision, output upload and publication). Coffee's value here is placement, retries, verified transfer and
failure handling across machines, not speed on one machine. Larger tasks amortize the fixed costs; that was **not** measured and is not claimed.

## Idle and peak resources (best-effort `/proc` readings of the Coffee processes themselves)

| Process | Idle RSS (after registration) | Peak RSS (`VmHWM`) after the job | Idle CPU over 3 s | CPU during the job |
| --- | --- | --- | --- | --- |
| coordinator | 8.0–8.1 MiB, 11 threads | 8.9–9.0 MiB | 0.03–0.07 s | 0.48–0.62 s |
| each Bean | 5.8–5.9 MiB, 2 threads | 6.1–6.5 MiB | 0.02–0.03 s | 0.12–0.27 s own; 4.6–9.1 s in reaped children (tasks + supervisors) |

Per-task RSS inside Coffee is **not recorded** (unavailable); the direct baseline's largest child resident set was 35.0 MiB (`ru_maxrss`, taskset + task). Network and disk
throughput were not measured. The hostile-limit behaviour (memory hog, file flood, 1-CPU affinity) is covered by the correctness suites, not here.

## Reproduce

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOFFEE_BUILD_TESTS=OFF && cmake --build build-release
python3 bench/run_bench.py --coffee build-release/coffee --work build-release/coffee_bench_work --out /tmp/coffee-bench-out
```

Defaults: tasks 8, rounds 280, 4 MiB inputs, 1 warm-up + 5 measured repetitions, task CPUs 8,9,10,11, aux CPU 4 (**adjust `--cpus/--aux-cpu` for your
topology**; the script refuses nothing, so check for SMT siblings and P/E-core mixes yourself). It takes about 5 minutes, uses a few hundred MiB of temp
space in `/tmp` and cleans up its processes and directories. Do not benchmark with a sanitizer build.

Harness notes: the first attempt of this benchmark was aborted by a real defect (a spurious scratch-sampler failure while a command was inside `execve`;
see [limitations](limitations.md)); it was fixed with a regression test first, the release binary rebuilt and the benchmark rerun from scratch, and
the failed attempt was retained in the private measurement archive. After the benchmark run the harness gained the per-phase timing fields used by the single phase probe. Subsequent executable fixes include temporary transfer cleanup. These are archived measurements of the recorded historical source, not measurements of the final release executable; no post-cleanup-fix benchmark was run.

The public environment record generalizes the user ID and browser scope identifier in cgroup paths; resource-limit values, timings, output digests and CPU/governor/background-load facts are retained. The historical source ID is archival and cannot be checked out from public history.

## What would be needed for real evidence

Two or more physical machines on a real network (office server, spare laptop, everyday laptop), repeated runs with network RTT/bandwidth recorded, tasks of
realistic duration, and identical total hardware budgets for the comparator. None of that exists yet.
