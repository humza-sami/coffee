# Coffee

## What if your computers could become one?

A laptop at home. Your dad's laptop. A server at the office.
You already own the machines. **Coffee's goal is to bring their CPU, memory and storage together as one virtual machine**,
even when they are in different places. Coffee would handle the resource sharing underneath.

**Today: an early alpha, not yet a unified VM.** Coffee can run tasks across participating machines, called **Beans**.
Each task must fit on one Bean. Memory is not pooled yet.

![Coffee's vision: Home Laptop, Dads Laptop and Office Server connect to one unified VM, with signals moving in both directions](docs/assets/coffee-terminal.gif)

*This illustration shows Coffee’s goal. Hardware specs are examples.*
[View the still image](docs/assets/coffee-terminal.png).

[Try the alpha](#quickstart) · [Release](https://github.com/humza-sami/coffee/releases/tag/v0.1.0-alpha.1) · [Build checks](https://github.com/humza-sami/coffee/actions/runs/44973542870) · [Apache-2.0](LICENSE)

## The idea

Use the computers you already have as one shared machine. A spare laptop could contribute memory. An office server could
contribute CPU and storage. You would use the combined resources without choosing which computer holds each piece.
That single-machine experience is the long-term vision; the current task runner is a first step.

Coffee is native C++20. Its runtime needs no AI service.

## What you can do today

The `v0.1.0-alpha.1` prerelease gives you a coordinator and Bean workers. You can:

- Send tasks to Beans with enough free capacity, and pass verified files between pipeline steps.
- Follow status and logs, understand why a task is waiting, and cancel work.
- Retry failed attempts and restart the coordinator using its saved state.

More Beans let you run more tasks at once. One task still uses one Bean. Pooling CPU, memory and storage into one VM
is future work. See the [current limits](docs/limitations.md).

<details>
<summary>Testing and performance</summary>

**Tested so far:** local Ubuntu 26.04 process tests, plus [hosted Ubuntu 24.04 / GCC 14 CI](https://github.com/humza-sami/coffee/actions/runs/44973542870)
with 11/11 tests passing in each of the normal and ASAN/UBSAN builds. Physical multi-device, WAN and real power-loss tests
have not been run. These source-build results do not establish binary compatibility with other distributions.

**Performance:** in the recorded one-host benchmark, Coffee took 30–53% longer than direct pinned processes at the same
CPU budget for eight short tasks. That measures overhead, not multi-device performance or the future shared VM.
[Read the benchmark and its limits](docs/benchmarking.md).

</details>

## Quickstart

Build from source and try two Beans on one machine.

Needs Ubuntu/Linux x86-64, `g++` with C++20, `python3` (with `venv`), `curl`, `tar`, `sha256sum` and network access once, to
fetch pinned dependencies into `./.tools` (no sudo, nothing global; details in [docs/native-build.md](docs/native-build.md)).

Clone or download this repository, enter its directory, then run:

```sh
scripts/bootstrap.sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure      # optional, about 2 minutes
```

<details>
<summary>Run a two-Bean demo</summary>

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

**Demo output**

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

</details>

<details>
<summary>When a task does not fit</summary>

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

</details>

<details>
<summary>Install or remove Coffee</summary>

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

</details>

## Use more than one machine

Use an SSH tunnel or an encrypted network such as WireGuard or Tailscale to connect your machines. Coffee has no built-in
TLS yet. An SSH tunnel can use `ssh -N -L 7077:127.0.0.1:7077 user@coordinator`. The coordinator stays on loopback unless you
pass an explicit plaintext override, which adds **no** encryption. Complete recipes, token handling, drain/restart/recovery: [docs/operations.md](docs/operations.md).

## Find your way around

- [Build and workload format](docs/native-build.md) · [CLI and protocol](docs/native-protocol.md)
- [Operations and recovery](docs/operations.md) · [Current limitations](docs/limitations.md)
- [Release notes](docs/releases/v0.1.0-alpha.1.md) · [Release verification](docs/verification/release-verification.md)

## Help build the shared machine

Coffee is early. Try the local demo, tell us where it breaks, or help turn the shared-machine vision into small, testable
steps. Useful contributions include clear bug reports, reproducible workloads, tests and improvements to the docs.
[Start with the contribution guide](CONTRIBUTING.md).

Please [report security issues privately](SECURITY.md). Coffee currently assumes trusted machines and a trusted owner;
commands run as the Bean's user, without a sandbox. Remote links need an SSH tunnel or encrypted overlay.

Coffee is licensed under [Apache-2.0](LICENSE). See [dependencies](DEPENDENCIES.md), [third-party notices](NOTICE) and
[artwork provenance](docs/assets/README.md).
