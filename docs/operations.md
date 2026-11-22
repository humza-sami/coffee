# Operating Coffee (Beta 1)

Everything here describes behaviour that exists in this build. Where something was **not** exercised it says so. All
verification so far used processes on one Linux host over loopback; the remote recipes below follow the documented flags and
have not been run across real machines.

Roles: one **coordinator** (`coffee serve`, owns the durable state), any number of **Beans** (`coffee join`, run tasks), and the
`coffee` CLI as a client. A task is an ordinary process and must fit inside **one** Bean's CPU/memory budget; Beans do not pool
memory or disk ([limitations](limitations.md)).

## Dependencies

Runtime: a Linux x86-64 kernel with `/proc`, a C++ standard library matching the build host (the binary is dynamically linked
against `libstdc++`, `libgcc_s`, `libc`, `libm`; it is **not** shown to run on older distributions than the one it was built on),
and whatever your workload commands need (`sh`, `tr`, ...). Build time: g++ with C++20, `python3` with `venv`, `curl`, `tar`,
`sha256sum`; `scripts/bootstrap.sh` fetches pinned, checksummed dependencies into `./.tools` ([DEPENDENCIES.md](../DEPENDENCIES.md)).
No sudo, no system packages beyond the compiler, no AI service or API key at runtime.

## Install and remove

```sh
cmake --install build --prefix "$HOME/.local"
"$HOME/.local/share/coffee/uninstall.sh" --prefix "$HOME/.local" --dry-run     # preview
"$HOME/.local/share/coffee/uninstall.sh" --prefix "$HOME/.local"
```

* There is deliberately no default prefix and nothing is installed system-wide.
* **Collision handling:** before copying anything, the installer checks every destination. A path that exists and is not an
  *unmodified* file recorded in this prefix's own manifest (an existing `coffee` of another origin, a symlink, a locally edited
  binary) makes the install fail with `refusing to install … nothing was written` and a list of the offending paths. Pick another
  prefix or move the file yourself. An existing manifest must contain recognized Coffee paths and matching installed binary and
  uninstaller records; stale or incomplete ownership evidence is refused. Inspect the prefix before moving a stale manifest aside.
  Re-installing an unmodified Coffee over itself (upgrade) is allowed.
* **Removal** follows `share/coffee/install-manifest.txt` (SHA-256 per file). Files whose digest changed are kept and reported;
  symlinks and unlisted files are never removed; manifests with absolute or `..` paths are rejected before anything is deleted;
  all directories are kept, including empty folders, because the manifest records only file ownership. **State directories** (`coffee init --state`),
  Bean work directories, `results/` and artifacts are never touched; delete them yourself if you want them gone.
* `cpack -G TGZ` (run inside the build directory) produces `coffee-<version>-Linux-x86_64.tar.gz` with the same file set.
  Unpacking over an existing install bypasses the collision check, so unpack into a fresh prefix.
* Optional examples live in `share/coffee/examples`; `share/coffee/systemd-user/*.service` are *templates*: copy and edit them
  yourself (`systemctl --user`); Coffee never installs or enables a service.

## Start, stop, drain, recover

```sh
coffee init --state ~/.local/state/coffee            # once; creates config.json, token (0600), coffee.db, artifacts/
coffee serve --state ~/.local/state/coffee           # 127.0.0.1:7077; SIGTERM/SIGINT stop it cleanly
coffee join --work-dir ~/.local/state/coffee-bean --cpus 4 --memory-mb 8192 \
            --server http://127.0.0.1:7077 --token-file ~/.config/coffee/token
coffee beans                                         # online / unreachable / draining, with used vs total slots
coffee drain BEAN            # no new claims; running attempts finish.  coffee drain BEAN --undo  resumes
```

* **Budgets are declarations, not quotas.** `--cpus N` advertises N slots; each running slot is pinned to a distinct CPU id from
  the process's allowed CPUs. `--memory-mb` is accounting for placement; each task's `memory_mb` becomes its `RLIMIT_AS`.
  Choose them below what the machine needs for its owner ([enforcement table](native-protocol.md#beans-supervision-and-the-resources-actually-enforced)).
* **Stopping a Bean** (SIGTERM) cancels its commands and returns the attempts for retry; `kill -9` of a Bean is also cleaned up
  (the supervisor kills the command's process group and descendants) and the lease later expires so another Bean retries.
  If the *supervisor itself* is SIGKILLed, only the direct command is guaranteed to stop.
* **Coordinator restart:** state is durable SQLite; accepted results and attempt history survive. A restart never extends a
  lease: Beans that renew in time keep their attempt, otherwise the attempt expires and is retried while attempts remain. Jobs
  keep running if the coordinator is restarted within the lease (default 10 s, `--lease-ms`).
  There is **no automatic coordinator failover**; run one coordinator and keep its state directory.
* **Schema upgrade:** opening a state created by the accepted pre-network build (schema v1) upgrades it to v2 and preserves the
  cluster id, Beans and job history (verified with a genuine v1 state, see [verification record](verification/release-verification.md)).
  Jobs from that old build never uploaded their input files, so their unfinished tasks fail with `input '…' has no recorded
  digest`.
* **Cancel:** `coffee cancel JOB [--task T]`; a late result from a cancelled/expired attempt is rejected, never accepted.
* **Disk housekeeping:** unreferenced blobs and old staged inputs are not garbage-collected yet; monitor the state directory.

## Remote machines: encrypted connectivity

Coffee 0.1.0-beta.1 has **no TLS**: the bearer token and artifact bytes are plain HTTP. The coordinator therefore binds
`127.0.0.1` and refuses any other address unless you add `--allow-plaintext-nonloopback` (clients: `--allow-plaintext-remote`).
**Those flags add no encryption**; they only record that you know the link is protected by something else.

### Option A: SSH tunnel (recommended; nothing but `sshd` needed on the coordinator host)

On the coordinator host (office server): `coffee init --state S && coffee serve --state S` (loopback only).
On each Bean host (laptop):

```sh
mkdir -p ~/.config/coffee && umask 077
scp office-server:S/token ~/.config/coffee/token && chmod 600 ~/.config/coffee/token     # copy the token out of band, once
ssh -N -L 7077:127.0.0.1:7077 user@office-server &                                       # encrypted tunnel to the loopback listener
coffee join --server http://127.0.0.1:7077 --token-file ~/.config/coffee/token --work-dir ~/.local/state/coffee-bean --id laptop --cpus 4 --memory-mb 8192
```

From a client machine use the same tunnel plus `coffee brew … --server http://127.0.0.1:7077 --token-file ~/.config/coffee/token`
(or `export COFFEE_SERVER=… COFFEE_TOKEN_FILE=…`). Use `autossh`/`systemd --user` if you want the tunnel restarted; if the tunnel
drops longer than a lease, running attempts expire and retry elsewhere. *Not exercised across real hosts in this release.*

### Option B: WireGuard or Tailscale overlay

Bind the coordinator to the overlay address and acknowledge plaintext-inside-the-overlay explicitly:

```sh
coffee serve --state S --listen 100.64.0.10:7077 --allow-plaintext-nonloopback
coffee join --server http://100.64.0.10:7077 --allow-plaintext-remote --token-file ~/.config/coffee/token --work-dir …
```

Only do this when every peer on the overlay is trusted: any peer that can reach the port and knows the token can submit work.
Use the overlay's ACLs/firewall to limit who can connect. *Not exercised across real hosts in this release.*

### Token and state handling

* The token is generated by `coffee init` (256 random bits, `STATE/token`, mode 0600) and read **only from a file** (`--token-file`,
  `$COFFEE_TOKEN_FILE`, or `--state`), never from argv. It is never printed or logged; do not paste it into tickets or chat. Copy it
  with `scp`/a password manager, not through a command line that other users can see.
* The token is one shared cluster credential: any holder may submit, cancel and read everything. There is no per-user or per-Bean
  credential, no revocation other than re-initialising the state, no brute-force throttling.
* **Trust model:** a trusted owner controlling every Bean and client. Commands run as the Bean's user; the resource limits stop
  accidents, not malicious workloads. Do not expose the port to the internet or run untrusted workloads.

## Troubleshooting

| Symptom | Likely cause | Check |
| --- | --- | --- |
| `coffee: refusing non-loopback …` (exit 2) | plaintext guard | use a tunnel, or the explicit override on an already-encrypted network |
| exit 4 `unauthorized` | wrong/missing token file | token must be the file from the coordinator's `coffee init` |
| exit 4 `unreachable` | coordinator down / tunnel not up | `curl http://127.0.0.1:7077/v1/health` |
| job stays `pending` | no Bean can ever fit, or Beans busy/draining | `coffee explain JOB TASK`, `coffee beans` |
| `workspace limit exceeded …` | attempt scratch above `--max-workspace-bytes` | raise it on the Bean or write less |
| install `refusing to install` | an existing file would be overwritten | choose another `--prefix` |
