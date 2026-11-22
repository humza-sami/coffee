# What if your computers could become one?

Running an app across separate computers means choosing where it runs, moving files and managing each machine.
**Coffee is being built to make those computers work as one virtual machine**, even across different locations.
The goal is to combine their CPU, memory and storage, with Coffee handling the sharing underneath.

Coffee is native C++20. Its runtime needs no AI service.

## Example

Imagine using the resources of your home laptop, your dad's laptop and an office server through one machine.

![Home Laptop, Dads Laptop and Office Server connected to one unified VM, with signals travelling both ways](docs/assets/coffee-terminal.gif)

## Terminology

| Name | Meaning |
| --- | --- |
| Bean | A worker process on a machine, with a set CPU and memory budget. |
| Coordinator | The process that keeps the job queue and sends tasks to Beans. |
| Workload | A `coffee.yaml` file describing tasks, their needs and their dependencies. |
| Task | One command to run, with inputs, outputs and resource needs. |
| Job | One submitted run of a workload, including its tasks and attempts. |

## Commands

Client commands use `COFFEE_SERVER` and `COFFEE_TOKEN_FILE`, as shown in the demo below.
Replace `JOB`, `TASK`, `BEAN` and paths with your own values.

| Command | What it does |
| --- | --- |
| `coffee init --state ./.coffee` | Create or check the coordinator's state directory. |
| `coffee validate coffee.yaml` | Check a workload file before running it. |
| `coffee plan coffee.yaml` | Show task order and dependencies. Add `--server URL` to check Bean capacity. |
| `coffee serve --state ./.coffee` | Start the coordinator. |
| `coffee join --work-dir ./.coffee-bean` | Start a Bean. |
| `coffee beans` | List Beans and their available resources. |
| `coffee drain BEAN` | Stop sending new tasks to a Bean. Use `--undo` to resume. |
| `coffee brew coffee.yaml --wait --out ./results` | Submit work, wait for it and download results. |
| `coffee status` / `coffee status JOB` | List jobs or inspect one job. |
| `coffee logs JOB TASK` | Read a task attempt's output. Use `--stderr` for errors. |
| `coffee explain JOB TASK` | Explain why a task is waiting or cannot fit. |
| `coffee cancel JOB` | Cancel a job. Add `--task TASK` to cancel a task and its dependent tasks. |
| `coffee --help` / `coffee COMMAND --help` | Show command options. |
| `coffee --version` | Show the installed version. |

<details>
<summary>Build Coffee and run a two-Bean demo</summary>

Needs Ubuntu/Linux x86-64, a C++20 compiler, Python 3 with `venv`, `curl`, `tar` and `sha256sum`.
The bootstrap downloads pinned tools into this project; nothing is installed globally.
[Build details](docs/native-build.md).

```sh
scripts/bootstrap.sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure      # optional, about 2 minutes
```

Run a coordinator and two Beans on this machine, then a two-stage pipeline:

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

See [the demo output, task limits and installation steps](docs/getting-started.md).
For remote machines, use an SSH tunnel or an encrypted network; Coffee has no built-in TLS yet.
[Operations guide](docs/operations.md) · [CLI and protocol](docs/native-protocol.md).

</details>

## Roadmap

- [x] Task foundation: execution, file pipelines, status, logs, retries and saved coordinator state.
- [ ] Test on real devices and real networks.
- [ ] Add built-in encrypted connections.
- [ ] Develop pooled storage.
- [ ] Research remote memory and shared CPU execution toward one unified VM.
- [ ] Improve recovery and add coordinator failover.
- [ ] Reach stable v1.0 when the shared-machine behaviour and its limits are ready.

Releases will use numbered betas—Beta 1, Beta 2 and onward—while this work develops.
[Beta 1 notes](docs/releases/v0.1.0-beta.1.md) · [Contribute](CONTRIBUTING.md).

## Support Me

Help me build and test Coffee. You can lend a Linux VPS, help fund development, or contribute code and tests.
[Offer a server or discuss support](https://github.com/humza-sami/coffee/issues/new?template=support.md) ·
[Start contributing](CONTRIBUTING.md).

*Available in Beta 1: task execution, pipelines, status, logs and retries. Each task runs on one Bean; a combined VM and
pooled memory/storage are future work. [Limitations](docs/limitations.md) · [Security](SECURITY.md) · [Apache-2.0](LICENSE).*
