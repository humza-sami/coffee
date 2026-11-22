# Building Coffee (native MVP)

Prerequisites on Ubuntu x86-64: `g++` with C++20 (verified with GCC 15.2.0), `make`, `curl`, `tar`, `sha256sum`,
`python3` with `venv` (only to obtain pinned CMake/Ninja wheels), and network access to pypi.org, github.com and
sqlite.org for the one-time bootstrap. No `sudo`, no system development headers and no global installs are needed.

```sh
scripts/bootstrap.sh                      # CMake 4.3.4, Ninja 1.13.2 and checksummed sources into ./.tools
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/coffee --version
```

The binary is `build/coffee`; remove everything with `rm -rf build .tools`. Nothing is installed system-wide, so
no existing `coffee` command is overwritten. To install into a prefix you choose, see "Install" below and
[operations.md](operations.md). Dependencies, versions, licenses and checksums are listed in
[DEPENDENCIES.md](../DEPENDENCIES.md). Builds are portable (no `-march=native`).

## Install, package, sanitizers, benchmarks

```sh
cmake --install build --prefix "$HOME/.local"     # refuses to overwrite files Coffee did not install; writes share/coffee/install-manifest.txt
"$HOME/.local/share/coffee/uninstall.sh" --prefix "$HOME/.local"
(cd build && cpack -G TGZ)                         # coffee-0.1.0-beta.1-Linux-x86_64.tar.gz
# correctness under sanitizers: a SEPARATE build directory, never used for timing
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
# optimized build for measurements (-O3 -DNDEBUG), see benchmarking.md
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOFFEE_BUILD_TESTS=OFF && cmake --build build-release
```

Sanitizer notes: the fault-injection fixture is an `LD_PRELOAD` library; in an ASan build the harness preloads `libasan` first
(otherwise ASan aborts at startup, see `tests/integration/harness.py`). Sanitizer builds are slower and are not used for any timing.
The default build type is `RelWithDebInfo` (`-O2 -g`, no `-march=native`).

## State directory and CLI

```sh
./build/coffee init --state ./.coffee            # idempotent; prints no secrets
./build/coffee validate examples/coffee.yaml [--json]
```

Exit codes: 0 success, 1 operation failed, 2 usage error, 3 recognised command not implemented in this build,
4 coordinator unreachable/unauthorized, 5 `brew --wait` timed out.

## Running a local cluster

```sh
./build/coffee init --state ./.coffee
./build/coffee serve --state ./.coffee --ready-file ./.coffee/ready.json &          # 127.0.0.1:7077
export COFFEE_SERVER=http://127.0.0.1:7077 COFFEE_TOKEN_FILE=$PWD/.coffee/token
./build/coffee join --work-dir ./.coffee-bean-a --id bean-a &
./build/coffee join --work-dir ./.coffee-bean-b --id bean-b &
./build/coffee beans
./build/coffee brew examples/pipeline/coffee.yaml --wait --out ./results
./build/coffee status
# stop: kill %1 %2 %3 (SIGTERM); remove: rm -rf .coffee .coffee-bean-* results build .tools
```

`scripts/demo-local.sh` runs a complete, self-cleaning local demonstration (ephemeral port, temp state) with assertions: it exits non-zero with `DEMO FAILED: …` if any required step or check fails and prints `DEMO PASSED` only at the very end. Protocol, flags, status
codes and the resource limits that are really enforced: [native-protocol.md](native-protocol.md).
Tests: `ctest --test-dir build` runs 11 suites: unit, CLI smoke, protocol, cluster, faults, demo, hardening, quickstart, upgrade, install and install safety. Integration tests start real processes on loopback ephemeral ports and clean up temporary state. The [genuine schema-v1 fixture](../tests/integration/fixtures/v1/README.md) is bundled: migration runs by default in source archives and shallow checkouts, and missing/corrupt fixture data fails. A complete alternate fixture may be selected with `-DCOFFEE_V1_FIXTURE_DIR=/path/to/fixture`.

## Workload schema `coffee/v1alpha1`

See [examples/coffee.yaml](../examples/coffee.yaml). Required: `apiVersion`, `kind: Workload`, `metadata.name`,
`spec.tasks`. Task fields: `id` (unique, `[A-Za-z0-9][A-Za-z0-9._-]{0,62}`), `command` (non-empty argv list, no
implicit shell), `depends_on`, `inputs` (`source` relative to the workload file, or `from_task`+`output` naming a declared output of an explicit `depends_on`
producer; `target` relative to the attempt directory), `outputs` (relative paths), `resources.cpus` (default 1, 1..1024), `resources.memory_mb` (default 256),
`timeout_seconds` (default 300, up to 604800), `max_attempts` (default 3, up to 100). Unknown fields, duplicate IDs,
unknown dependencies, cycles, absolute/`..`/symlink-escaping/duplicate paths, input sources that are not regular
files below the workload directory, non-integer resources, multi-document or >1 MiB YAML are rejected. Plain YAML
integers are integers; every other plain scalar is a string (quote or not, `[sleep, 1]` yields `"1"` for argv).
`coffee validate --json` prints the normalized form with defaults filled in.

## Durable store contract (public header `include/coffee/store.hpp`)

* Jobs/tasks: pending → running → succeeded | failed | cancelled; attempt history is kept (leases are never exposed).
* Each claim reserves the task's cpus/memory on the Bean inside one SQLite `BEGIN IMMEDIATE` transaction and creates
  an attempt with a random 192-bit lease id. Free resources are derived from running attempts, so they cannot drift.
* `renew`/`complete` succeed only for the current, running, unexpired lease of the stated Bean on a non-cancelled
  task whose deadline (`timeout_seconds` + `deadline_grace_ms`) has not passed. Expired/old/cancelled leases get a
  distinct `LeaseStatus` and never replace accepted output.
* Failed, expired, timed-out or abandoned attempts retry while `attempts_used < max_attempts`; dependents of a failed
  task fail with `dependency_failed: <id>`, dependents of a cancelled task are cancelled.
* Restart: state is committed before success is reported. Opening or reopening a Store (coordinator restart or an
  additional connection) never changes lease timestamps and there is no restart grace: a still-valid lease keeps its
  original expiry, a lease that expired (including during an outage) stays expired, and the next serialized call reaps
  it (reservation released, retry state recovered). Workers must renew in time or lose the attempt.
* `before_commit` may be slow; the clock, lease expiry and deadline are re-checked after it returns. If the attempt
  expired or timed out meanwhile, completion is rejected (`Expired`/`DeadlineExceeded`), nothing accepted is recorded,
  and anything the hook published stays unreferenced.
* Resources are accounting slots; enforcement is the Bean's job (see native-protocol.md). Timestamps rely on wall-clock time.
* Schema-v2 input transfer: `submit(workload, job_id, input_files)` records per-job input digests (table `job_files`, schema version 2,
  created idempotently); `Claim.inputs` carries the resolved input manifest (dependency outputs come only from the producer's
  accepted result); `lease_info` and the read-only `lease_status` serve the network layer. Lease semantics are unchanged.
