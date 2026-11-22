<p align="center">
  <img src="docs/assets/coffee-banner-v1.png" alt="COFFEE: three small coffee-bean shaped compute nodes joined by glowing paths into one larger bean constellation on a dark graphite background" width="100%">
</p>

# Coffee

**Owner-controlled task scheduling across a few Linux machines you already own.** One coordinator, any number of *Beans*
(worker processes on your office server, spare laptop or everyday machine), a durable job queue, verified file transfer and
honest failure handling. Native C++20, no AI service and no cloud dependency at runtime.

> **Status: `v0.1.0-alpha.1` (prerelease).** Everything below was exercised as *local processes on one Linux host*. No
> multi-device, WAN or real power-loss measurements exist yet. See [what is not supported](docs/limitations.md).

> ### Read this first: Coffee distributes tasks, it does not pool memory
> Every task is an ordinary process that must **fit inside one Bean's budget**. Adding Beans adds *scheduling capacity*
> (more tasks at once); it does **not** create a larger address space, a bigger "virtual machine", transparently
> distributed threads or a pooled disk. A task that needs 40 GB of RAM cannot run on three 8 GB Beans, however much memory they
> hold together; `coffee plan` and `coffee explain` say so instead of starting it (see [Placement failures](#when-a-task-does-not-fit)).

## What it does today

| Works (tested with real processes on one host) | Does **not** exist |
| --- | --- |
| `coffee init/serve/join/beans/plan/brew/status/logs/explain/cancel/drain` | pooled RAM across machines, transparent threads, durable pooled disk |
| Bearer-token API on loopback by default; SSH-tunnel / WireGuard / Tailscale recipes | built-in TLS/HTTPS (the token and data are plain HTTP on the wire) |
| Durable SQLite state, leases, retries, cancel, coordinator restart | automatic coordinator failover / HA |
| Digest-verified inputs/outputs, two-stage pipelines (`from_task`) | multi-tenant isolation or a sandbox: commands run as the Bean's user |
| Timeouts, address-space limit, CPU pinning, per-attempt scratch cap, cleanup after a Bean is `SIGKILL`ed | hard memory/CPU/disk quotas (no cgroups) |

**Performance, honestly:** on one host, with eight ~1 s tasks and equal CPU budgets, Coffee was 30–53 % *slower* than plain pinned
processes (fixed costs: hashing/fsyncing inputs, supervision, verified transfer); a second Bean helped only because it was given more CPUs. This is a
local-process simulation, not a multi-device measurement: [docs/benchmarking.md](docs/benchmarking.md).

## Quickstart (build from source, run a two-Bean demo)

Needs Ubuntu/Linux x86-64, `g++` with C++20, `python3` (with `venv`), `curl`, `tar`, `sha256sum` and network access once, to
fetch pinned dependencies into `./.tools` (no sudo, nothing global; details in [docs/native-build.md](docs/native-build.md)).

Clone or download this repository, enter its directory, then run:

```sh
scripts/bootstrap.sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure      # optional, about 2 minutes
```

Run a coordinator and two Beans on this machine, then a two-stage pipeline (`upper` → `stats`):

<!-- quickstart:start -->
```sh
export PATH="$PWD/build:$PATH"
coffee init --state ./.coffee
coffee serve --state ./.coffee --listen 127.0.0.1:7077 --ready-file ./.coffee/ready.json > ./.coffee/serve.log 2>&1 &
until [ -s ./.coffee/ready.json ]; do sleep 0.1; done
export COFFEE_SERVER=http://127.0.0.1:7077 COFFEE_TOKEN_FILE="$PWD/.coffee/token"
coffee join --work-dir ./.coffee-bean-a --id bean-a --cpus 2 --memory-mb 2048 > ./.coffee/bean-a.log 2>&1 &
coffee join --work-dir ./.coffee-bean-b --id bean-b --cpus 2 --memory-mb 2048 > ./.coffee/bean-b.log 2>&1 &
until [ "$(coffee beans | grep -c online)" -ge 2 ]; do sleep 0.2; done
coffee beans
coffee brew examples/pipeline/coffee.yaml --job-id demo --wait --out ./results
cat ./results/stats/size.txt
coffee status demo
kill %1 %2 %3
```
<!-- quickstart:end -->

Real output of the commands above (Ubuntu 26.04, one machine; columns are tab-separated in the terminal, Bean names and order may vary; `status` prints the job summary a second time):

```text
ID      STATE   CPUS  MEMORY_MB  NAME
bean-b  online  0/2   0/2048     bean-b
bean-a  online  0/2   0/2048     bean-a
job demo (pipeline-demo): succeeded
  upper  succeeded  attempts 1/3
      #1 on bean-b: succeeded
  stats  succeeded  attempts 1/3
      #1 on bean-a: succeeded
outputs written to ./results
34
HELLO COFFEE
SECOND LINE OF INPUT
```

`stats` consumed `upper`'s *accepted, digest-verified* output (`from_task`), possibly on a different Bean. The token stays in
`./.coffee/token` (mode 0600) and is never printed. State is in `./.coffee`; delete it to start over.
`scripts/demo-local.sh` runs a larger self-checking demonstration (two Beans, retries, `kill -9` of a Bean mid-task,
401/400 errors) and prints `DEMO PASSED` only if every assertion held.

## When a task does not fit

```sh
coffee plan big.yaml          # a task with  resources: {cpus: 1, memory_mb: 40960}  on 2 GB Beans
# huge  cpus=1 memory_mb=40960 timeout=300s max_attempts=3  [cannot be placed: no registered Bean has this capacity]
coffee brew big.yaml --job-id big      # the job is accepted but stays pending; it never runs
coffee explain big huge
# big/huge: no registered Bean has enough capacity for this task's resources
#   bean-b: does not fit; needs memory_mb 40960 but Bean capacity is 2048
```

Other failures are explained the same way: `coffee status JOB` shows every attempt and its reason, `coffee logs JOB TASK`
the captured stdout/stderr, `coffee explain` unmet dependencies or capacity. Exit codes: `0` ok, `1` job failed, `2` usage,
`3` not implemented, `4` unreachable/unauthorized, `5` `--wait` timed out.

## Install to a prefix of your choice

```sh
cmake --install build --prefix "$HOME/.local"      # or: scripts/install.sh --prefix "$HOME/.local"
"$HOME/.local/share/coffee/uninstall.sh" --prefix "$HOME/.local"
```

* No default prefix, no sudo, no system service. If `PREFIX/bin/coffee` (or any other target file) already exists and was not
  installed by Coffee, installation **refuses before writing anything** and leaves it untouched.
* A manifest with SHA-256 digests (`share/coffee/install-manifest.txt`) drives removal; edited files are kept, and your state
  directories, Bean work directories and results are never touched. Empty directories are kept. `cpack -G TGZ` (in `build/`) makes a tarball.
* Examples and `systemd --user` unit *templates* land in `share/coffee/`; Coffee never enables them for you.

## Use more than one machine

Coffee has no TLS yet, so encrypt the link yourself: an SSH tunnel (`ssh -N -L 7077:127.0.0.1:7077 user@coordinator`) or a
WireGuard/Tailscale overlay. The coordinator stays on loopback unless you pass an explicit plaintext override, which adds **no**
encryption. Complete recipes, token handling, drain/restart/recovery: [docs/operations.md](docs/operations.md).

## Documentation

| Topic | Where |
| --- | --- |
| Build, state directory, workload schema | [docs/native-build.md](docs/native-build.md) |
| HTTP routes, CLI flags, enforced limits | [docs/native-protocol.md](docs/native-protocol.md) |
| Running, remote setup, install/remove, recovery | [docs/operations.md](docs/operations.md) |
| What is and is not supported | [docs/limitations.md](docs/limitations.md) |
| Local benchmark protocol and results | [docs/benchmarking.md](docs/benchmarking.md) |
| Release notes | [docs/releases/v0.1.0-alpha.1.md](docs/releases/v0.1.0-alpha.1.md) |
| Release verification | [docs/verification/release-verification.md](docs/verification/release-verification.md) |
| Dependencies and licenses | [DEPENDENCIES.md](DEPENDENCIES.md), [NOTICE](NOTICE) |

## Contributing, security, license

See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md) (please report vulnerabilities privately). Coffee is
licensed under [Apache-2.0](LICENSE); third-party components keep their own licenses ([NOTICE](NOTICE)). The banner was generated
for this project ([provenance](docs/assets/README.md)).
