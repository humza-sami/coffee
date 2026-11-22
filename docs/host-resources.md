# Host resources: `coffee resources`

`coffee resources` reports what the current process can see of this machine's CPU, RAM and disk, and checks as a **dry run**
whether a contribution of a given size fits. It does not join a cluster, reserve, allocate, enforce or pool anything, needs no
network or token, and writes nothing. It is available when building from source and is not part of the tagged
`beta-v0.1` binaries; it is planned for the next release.

```sh
coffee resources                          # human report for the filesystem of .
coffee resources --path /srv/coffee --json
coffee resources --cpus 4 --memory-mb 8192 --storage-mb 50000 --reserve-memory-mb 2048
```

| Option | Meaning |
| --- | --- |
| `--path DIR` | Directory whose filesystem is measured (default `.`). It must already exist and be readable and searchable. A symlink is resolved and the capacity is that of the target's filesystem. |
| `--json` | One JSON object on stdout (schema below). |
| `--cpus N` | Requested whole CPU slots. Positive integer. |
| `--memory-mb N`, `--storage-mb N` | Requested MiB. Positive integers. |
| `--reserve-memory-mb N`, `--reserve-storage-mb N` | MiB kept back for the machine's owner. Default 1024 each; `0` is allowed. |

Numbers are strict decimal: no sign, space, prefix or trailing text; a value whose MiB-to-byte conversion would overflow 64 bits
is rejected before it is multiplied. `--help` (or `-h`) anywhere on the line prints the help and exits 0.

## Exit status and errors

| Status | When |
| --- | --- |
| 0 | Observation succeeded and any requested quantity fits. Unknown fields alone do not fail a command that requested nothing. |
| 1 | A requested quantity exceeds the maximum **or its maximum is unknown**; or `--path` is missing, not a directory, not readable and searchable, or its filesystem cannot be stat'ed. |
| 2 | Unknown option, missing option value, unexpected positional argument, empty `--path`, or a bad/negative/zero/overflowing number. |

With `--json`, stdout always holds exactly one object. A path or argument error is
`{"schema_version":1,"ok":false,"error":{"code":"io_error"|"invalid_argument","message":"..."}}` (status 1 and 2 respectively). A rejected
dry-run request uses the full schema with `ok:false`, `request.valid:false` and one entry in `request.errors` per failing quantity (status 1).
Human mode writes diagnostics to stderr.

## Non-UTF-8 paths and arguments

Linux file names and command-line arguments are bytes, not necessarily UTF-8. The filesystem is always accessed with the **original
bytes**, so a readable directory whose name is not valid UTF-8 is observed normally (exit 0). Only the *display* is lossy: in JSON
output (and in the `message` of JSON error objects) every invalid UTF-8 sequence is shown as U+FFFD, and valid UTF-8 is left
unchanged. When `storage.path` needed such a replacement the report adds a warning saying so; the displayed path **cannot be used to
recover the original file name**. No schema field is added. Invalid bytes in an option, value or positional argument are still a
usage error (status 2, `invalid_argument`), and a missing or inaccessible byte path is still an `io_error` (status 1) that keeps the
underlying cause (for example "No such file or directory"). Human mode writes diagnostics to stderr without conversion.

## JSON schema (version 1)

```json
{
  "schema_version": 1, "ok": true,
  "cpu": {"affinity_logical_cpus": 12, "physical_cores": 6, "quota_cores": null, "quota_status": "unlimited", "effective_cores": 12.0},
  "memory": {"host_total_bytes": 33554432000, "host_available_bytes": 17179869184, "cgroup_limit_bytes": null,
             "cgroup_headroom_bytes": null, "cgroup_status": "unlimited", "effective_available_bytes": 17179869184},
  "storage": {"path": "/srv/coffee", "total_bytes": 107374182400, "available_bytes": 10737418240},
  "budget": {"reserve_memory_bytes": 1073741824, "reserve_storage_bytes": 1073741824,
             "max_cpu_slots": 12, "max_memory_bytes": 16106127360, "max_storage_bytes": 9663676416},
  "request": {"cpus": null, "memory_bytes": null, "storage_bytes": null, "valid": true, "errors": []},
  "warnings": []
}
```

All byte counts are unsigned 64-bit integers. `quota_status` and `cgroup_status` are `limited`, `unlimited` or `unknown`.
**Every observation-derived number is `null` when it could not be observed. `null` never means 0 and never means unlimited**; a real
observed zero stays `0`. The output contains no hostname, machine id, IP address or token, and the command does not identify the
physical host.

## How each number is obtained

**CPU.** `affinity_logical_cpus` is the size of this process's `sched_getaffinity` set (not the machine's thread count), so `taskset`
and cpusets are respected. `physical_cores` counts distinct `(physical_package_id, core_id)` pairs of the *allowed* CPUs from sysfs
and is `null` if any topology file is missing or malformed; it is descriptive, not a scheduling slot count. `quota_cores` is the
tightest finite `cpu.max` quota/period on the cgroup v2 path (fractions allowed). `effective_cores = min(affinity, quota)` and
`max_cpu_slots = floor(effective_cores)`, so a 0.5 CPU quota supports no whole slot. A parent's quota is **not** spare CPU time
for you: sibling groups compete for it and that contention is not predictable.

**RAM.** `MemTotal` and `MemAvailable` come from `/proc/meminfo` (kB converted exactly to bytes; missing, duplicate, malformed or
overflowing values give `null`; an available figure above the total is capped at the total with a warning). For every visible cgroup v2 group
with a finite `memory.max` the headroom is `max(0, memory.max - memory.current)` using *that group's* `memory.current`, which includes
its siblings' usage; `cgroup_limit_bytes` is the smallest finite limit and `cgroup_headroom_bytes` the smallest headroom (it is not a
difference of limits). `effective_available_bytes = min(host available, every finite headroom)`. A group already over its limit
saturates at 0. `max_memory_bytes = max(0, effective_available - reserve)`.

**Disk.** `statvfs` of the resolved directory: `total = f_blocks * f_frsize`, `available = f_bavail * f_frsize` (blocks reserved for
root are not counted), with overflow-checked products; a zero `f_frsize`, or more free than total blocks, fails the command rather
than inventing a budget. `max_storage_bytes = max(0, available - reserve)`. Several paths on one filesystem are one disk, not
more capacity; the command reports one filesystem.

## cgroup v2 resolution and what stays unknown

One resolver serves CPU and RAM. It takes the unified `0::` entry of `/proc/self/cgroup`, finds the `cgroup2` mount in
`/proc/self/mountinfo` whose mount *root* is the longest path-component prefix of that entry (so `/foo` never matches `/foobar`; octal escapes
such as `\040` are decoded; relative, non-normalized, `..` or NUL paths are rejected), maps the remainder below the mount point
and walks every group from the process's group up to the visible mount root, inclusive. Directories and files are opened one
component at a time without following symbolic links, so nothing outside the mount point is read.

The result is `unknown` (and everything derived from it is `null`, with a warning) when:

* the hierarchy is not resolvable: cgroup v1 only (not supported), no unified entry or `cgroup2` mount, malformed or duplicated records,
  equally specific mounts, or a membership that no visible mount covers;
* a relevant file is missing or unreadable on a **non-root** group, for example because the `cpu` or `memory` controller is not enabled
  for it (a missing controller is not treated as unlimited);
* a value is malformed: signs, trailing text, overflow, a zero period (even beside `max`), non-numeric `memory.current`.

The one deliberate exception: in the real hierarchy root (mount root `/`, and no `cgroup.events` file, which every non-root group has)
missing `cpu.max` / `memory.max` is expected and means no cap there; the groups below it are still read. A cgroup namespace
whose visible root is not the real root is **not** treated that way: its absent files give `unknown`.

Limits: the command cannot see ancestors hidden by a cgroup namespace or a container's mount view, so values cover **the visible
hierarchy only**; a stricter limit above the visible root is invisible to it. Many desktop and CI scopes do not enable the `cpu` controller on a leaf group, in
which case `quota_status` is `unknown`, `max_cpu_slots` is `null`, and a `--cpus` request is rejected; run the process in a
scope or service that enables `cpu` (for example `systemd-run --user --scope -p CPUQuota=...`) to get a definite answer. Counters such as
`MemAvailable`, `memory.current` and free blocks change continuously: the report is a snapshot, not a reservation or a promise that
the amount will still be free when work starts.

## Dry-run requests

`--cpus`, `--memory-mb` and `--storage-mb` are compared with `budget.max_cpu_slots`, `max_memory_bytes` and `max_storage_bytes`. A
request exactly equal to a maximum passes; one more unit fails; a request against an unknown or zero maximum fails. Every failing
quantity is reported with its own reason. Passing means "this fit when observed", not that anything is held for you.

This command is the local observation step only. It does not change what `coffee join` advertises, and it does not provide pooled
memory, shared disk or one unified machine; see [limitations](limitations.md).
