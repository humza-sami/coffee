# Getting started with Coffee

Start with the build and marked two-Bean demo in the [README](../README.md#commands).
The output below was captured on one Ubuntu 26.04 machine; it is not a multi-device test.

**Demo output**

Real output of the README demo commands (Ubuntu 26.04, one machine; columns are tab-separated in the terminal, Bean names and order may vary; `status` prints the job summary a second time):

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

See [operations](operations.md) for remote links and [limitations](limitations.md) for supported behaviour.
