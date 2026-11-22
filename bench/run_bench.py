#!/usr/bin/env python3
"""Bounded local-process benchmark: direct pinned processes vs Coffee with one or two Beans on ONE host.

This is a local-process simulation. Two Beans on one machine are not two devices; nothing here measures a network, a
second machine, pooled memory or any transparent threading. See docs/benchmarking.md for the protocol and its limits.

  python3 bench/run_bench.py --coffee build-release/coffee --work build-release/coffee_bench_work --out DIR
"""
import argparse
import hashlib
import json
import os
import platform
import random
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def proc_status(pid, key):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith(key + ":"):
                    return int(line.split()[1])  # kB
    except OSError:
        pass
    return None


def proc_ticks(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            parts = f.read().rsplit(")", 1)[1].split()
        return {"utime": int(parts[11]), "stime": int(parts[12]), "cutime": int(parts[13]), "cstime": int(parts[14])}
    except (OSError, IndexError, ValueError):
        return None


def read_text(p):
    try:
        with open(p) as f:
            return f.read().strip()
    except OSError:
        return None


def cgroup_chain():
    """Limits found along the process's real cgroup ancestry (not assuming the root means unlimited)."""
    line = (read_text("/proc/self/cgroup") or "").splitlines()
    rel = line[0].split("::", 1)[1] if line and "::" in line[0] else ""
    chain, cur = [], rel
    while True:
        d = "/sys/fs/cgroup" + cur
        chain.append({"path": cur or "/", **{k: read_text(os.path.join(d, k)) for k in ("cpu.max", "memory.max", "memory.high", "cpuset.cpus.effective", "pids.max")}})
        if not cur or cur == "/":
            break
        cur = os.path.dirname(cur)
    return chain


def environment(args):
    cm = {}
    cache = os.path.join(os.path.dirname(os.path.abspath(args.coffee)), "CMakeCache.txt")
    for line in (read_text(cache) or "").splitlines():
        for k in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_CXX_COMPILER", "CMAKE_C_FLAGS_RELEASE"):
            if line.startswith(k + ":"):
                cm[k] = line.split("=", 1)[1]
    cpu = ""
    for line in (read_text("/proc/cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip()
            break
    gov = {c: read_text("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor" % c) for c in args.cpus}
    return {
        "git_head": subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip(),
        "git_dirty_files": subprocess.run(["git", "-C", ROOT, "status", "--porcelain"], capture_output=True, text=True).stdout.count("\n"),
        "coffee_version": subprocess.run([args.coffee, "--version"], capture_output=True, text=True).stdout.strip(),
        "cmake_cache": cm,
        "compiler": subprocess.run(["g++", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
        "kernel": platform.release(), "machine": platform.machine(), "python": sys.version.split()[0],
        "cpu_model": cpu, "logical_cpus": os.cpu_count(), "initial_affinity": args.initial_affinity,
        "mem_total_kb": proc_meminfo("MemTotal"), "mem_available_kb_at_start": proc_meminfo("MemAvailable"),
        "task_cpus": args.cpus, "aux_cpu": args.aux_cpu,
        "cpu_topology": {c: read_text("/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list" % c) for c in args.cpus + [args.aux_cpu]},
        "governors": gov, "cgroup_chain": cgroup_chain(), "loadavg_at_start": read_text("/proc/loadavg"),
        "workload": {"tasks": args.tasks, "rounds": args.rounds, "input_bytes_each": args.input_mib << 20, "seed": args.seed},
        "limits": {"memory_mb_per_task": 128, "bean_poll_ms": args.poll_ms, "brew_poll_ms": args.poll_ms},
    }


def proc_meminfo(key):
    for line in (read_text("/proc/meminfo") or "").splitlines():
        if line.startswith(key + ":"):
            return int(line.split()[1])
    return None


class Direct:
    """Strongest simple local baseline: a pool of N slots, each pinned (taskset) to one distinct CPU, tasks fed dynamically."""

    def __init__(self, ncpu):
        self.n = ncpu
        self.name = "direct-%dcpu" % ncpu
        self.kind, self.beans, self.slots, self.budget = "direct", 0, ncpu, ncpu

    def run(self, ctx):
        wd = tempfile.mkdtemp(prefix="coffee-bench-direct-", dir=ctx.tmp)
        cpus = ctx.cpus[: self.n]
        queue = list(range(ctx.args.tasks))
        running = {}
        outs = {}
        t0 = time.perf_counter()
        while queue or running:
            for cpu in cpus:
                if cpu not in running and queue:
                    i = queue.pop(0)
                    d = os.path.join(wd, "t%d" % i)
                    os.makedirs(d)
                    outs[i] = os.path.join(d, "out.bin")
                    running[cpu] = (i, subprocess.Popen(["taskset", "-c", str(cpu), ctx.work, ctx.inputs[i], outs[i], str(ctx.args.rounds)]))
            for cpu, (i, p) in list(running.items()):
                if p.poll() is not None:
                    if p.returncode != 0:
                        raise RuntimeError("direct task %d exit %d" % (i, p.returncode))
                    del running[cpu]
            time.sleep(0.002)
        wall = time.perf_counter() - t0
        digests = {"t%d" % i: sha256_file(p) for i, p in outs.items()}
        import resource
        res = {"wall_s": wall, "digests": digests, "child_max_rss_kb": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
               "resources": {"coordinator": "n/a (no coordinator)", "beans": "n/a"}}
        shutil.rmtree(wd, ignore_errors=True)
        return res


class CoffeeCfg:
    def __init__(self, beans, slots):
        self.beans, self.slots = beans, slots
        self.name = "coffee-%dbean-x%dslot" % (beans, slots)
        self.kind, self.budget = "coffee", beans * slots

    def run(self, ctx):
        wd = tempfile.mkdtemp(prefix="coffee-bench-cluster-", dir=ctx.tmp)
        procs = []
        env = dict(os.environ)
        env.pop("COFFEE_TOKEN_FILE", None)
        try:
            state = os.path.join(wd, "state")
            subprocess.run([ctx.coffee, "init", "--state", state], check=True, capture_output=True)
            ready = os.path.join(wd, "ready.json")
            logs = []
            def spawn(argv, name):
                lf = open(os.path.join(wd, name + ".log"), "wb")
                logs.append(lf)
                p = subprocess.Popen(argv, stdout=lf, stderr=subprocess.STDOUT, start_new_session=True, env=env)
                procs.append(p)
                return p
            coord = spawn([ctx.coffee, "serve", "--state", state, "--listen", "127.0.0.1:0", "--ready-file", ready], "coordinator")
            deadline = time.time() + 20
            while not os.path.exists(ready) and time.time() < deadline:
                time.sleep(0.02)
            info = json.loads(read_text(ready))
            server = "http://127.0.0.1:%d" % info["port"] if "port" in info else info.get("url")
            conn = ["--server", server, "--token-file", os.path.join(state, "token")]
            beans = []
            for k in range(self.beans):
                mine = ctx.cpus[k * self.slots:(k + 1) * self.slots]
                beans.append(spawn(["taskset", "-c", ",".join(map(str, mine)), ctx.coffee, "join", *conn, "--work-dir", os.path.join(wd, "bean%d" % k), "--id", "bean-%d" % k,
                                    "--cpus", str(self.slots), "--memory-mb", "2048", "--poll-ms", str(ctx.args.poll_ms), "--heartbeat-ms", "1000"], "bean%d" % k))
            deadline = time.time() + 20
            while time.time() < deadline:
                r = subprocess.run([ctx.coffee, "beans", *conn], capture_output=True, text=True)
                if r.stdout.count("online") >= self.beans:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Beans did not register")
            pids = {"coordinator": coord.pid, **{"bean-%d" % k: b.pid for k, b in enumerate(beans)}}
            # idle window: resources of the idle runtime itself
            t_idle0 = {n: proc_ticks(p) for n, p in pids.items()}
            time.sleep(ctx.args.idle_s)
            idle = {n: {"rss_kb": proc_status(p, "VmRSS"), "ticks_in_window": delta_ticks(t_idle0[n], proc_ticks(p)), "threads": proc_status(p, "Threads")} for n, p in pids.items()}
            before = {n: proc_ticks(p) for n, p in pids.items()}
            out = os.path.join(wd, "results")
            t0_epoch_ms = int(time.time() * 1000)
            t0 = time.perf_counter()
            r = subprocess.run(["taskset", "-c", str(ctx.args.aux_cpu), ctx.coffee, "brew", ctx.workload, *conn, "--job-id", "bench", "--wait", "--timeout", "120", "--out", out,
                                "--poll-ms", str(ctx.args.poll_ms)], capture_output=True, text=True, timeout=180)
            wall = time.perf_counter() - t0
            if r.returncode != 0:
                raise RuntimeError("brew failed rc=%d: %s %s" % (r.returncode, r.stdout[-400:], r.stderr[-400:]))
            peak = {n: {"vm_hwm_kb": proc_status(p, "VmHWM"), "ticks_during_job": delta_ticks(before[n], proc_ticks(p))} for n, p in pids.items()}
            digests = {}
            for i in range(ctx.args.tasks):
                digests["t%d" % i] = sha256_file(os.path.join(out, "t%d" % i, "out.bin"))
            st = json.loads(subprocess.run([ctx.coffee, "status", "bench", "--json", *conn], capture_output=True, text=True).stdout)
            tasks = st.get("tasks", [])
            attempts = sum(len(t.get("attempts", [])) for t in tasks)
            per_bean = {}
            for t in tasks:
                for a in t.get("attempts", []):
                    per_bean[a.get("bean_id")] = per_bean.get(a.get("bean_id"), 0) + 1
            phases = None
            try:  # coordinator-side timestamps (same host clock): upload+submit, scheduled execution, final download/exit
                created, finished = st["created_at_ms"], st["finished_at_ms"]
                att = [a for t in tasks for a in t.get("attempts", [])]
                phases = {"upload_and_submit_s": (created - t0_epoch_ms) / 1000, "job_created_to_finished_s": (finished - created) / 1000,
                          "finished_to_brew_exit_s": (t0_epoch_ms + wall * 1000 - finished) / 1000,
                          "first_claim_after_created_s": (min(a["claimed_at_ms"] for a in att) - created) / 1000,
                          "sum_task_attempt_s": sum(a["finished_at_ms"] - a["claimed_at_ms"] for a in att) / 1000}
            except (KeyError, ValueError, TypeError):
                pass
            return {"wall_s": wall, "phases": phases, "digests": digests, "attempts_total": attempts, "attempts_per_bean": per_bean,
                    "idle": idle, "peak_and_cpu": peak, "child_max_rss_kb": "unavailable (task commands run under the supervisor; per-task RSS is not recorded by Coffee)",
                    "job_state": st.get("state") or st.get("job", {}).get("state")}
        finally:
            for p in procs:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
            for p in procs:
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(p.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    p.wait()
            for lf in logs:
                lf.close()
            shutil.rmtree(wd, ignore_errors=True)


def delta_ticks(a, b):
    if not a or not b:
        return None
    return {k: b[k] - a[k] for k in a}


class Ctx:
    pass


def summarize(vals):
    return {"n": len(vals), "median": statistics.median(vals), "mean": statistics.fmean(vals), "min": min(vals), "max": max(vals),
            "stdev": statistics.stdev(vals) if len(vals) > 1 else 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--coffee", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tasks", type=int, default=8)
    ap.add_argument("--rounds", type=int, default=280)
    ap.add_argument("--input-mib", type=int, default=4)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--cpus", default="8,9,10,11", help="task CPUs (disjoint masks are carved from this list)")
    ap.add_argument("--aux-cpu", type=int, default=4, help="CPU for the coordinator, CLI and harness (outside every task budget)")
    ap.add_argument("--poll-ms", type=int, default=50)
    ap.add_argument("--idle-s", type=float, default=3.0)
    ap.add_argument("--seed", type=int, default=20261122)
    ap.add_argument("--configs", default="direct-1cpu,coffee-1bean-x1slot,direct-2cpu,coffee-1bean-x2slot,coffee-2bean-x1slot,direct-4cpu,coffee-2bean-x2slot")
    args = ap.parse_args()
    args.cpus = [int(c) for c in args.cpus.split(",")]
    args.coffee, args.work, args.out = map(os.path.abspath, (args.coffee, args.work, args.out))
    os.makedirs(args.out, exist_ok=True)
    args.initial_affinity = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {args.aux_cpu})  # harness + coordinator + CLI stay off the task CPUs; children inherit unless pinned

    cfgs = []
    for n in args.configs.split(","):
        if n.startswith("direct-"):
            cfgs.append(Direct(int(n[7:].replace("cpu", ""))))
        else:
            _, b, s = n.split("-")
            cfgs.append(CoffeeCfg(int(b.replace("bean", "")), int(s.replace("x", "").replace("slot", ""))))

    ctx = Ctx()
    ctx.args, ctx.coffee, ctx.work, ctx.cpus = args, args.coffee, args.work, args.cpus
    ctx.tmp = tempfile.mkdtemp(prefix="coffee-bench-")
    try:
        data = os.path.join(ctx.tmp, "data")
        os.makedirs(data)
        ctx.inputs = []
        for i in range(args.tasks):
            p = os.path.join(data, "in-%d.bin" % i)
            with open(p, "wb") as f:
                f.write(random.Random(args.seed + i).randbytes(args.input_mib << 20))
            ctx.inputs.append(p)
        tasks = "".join(
            "    - id: t%d\n      command: [\"%s\", in.bin, out.bin, \"%d\"]\n      inputs:\n        - {source: data/in-%d.bin, target: in.bin}\n      outputs: [out.bin]\n"
            "      resources: {cpus: 1, memory_mb: 128}\n      timeout_seconds: 120\n      max_attempts: 1\n" % (i, args.work, args.rounds, i) for i in range(args.tasks))
        ctx.workload = os.path.join(ctx.tmp, "coffee.yaml")
        with open(ctx.workload, "w") as f:
            f.write("apiVersion: coffee/v1alpha1\nkind: Workload\nmetadata:\n  name: bench\nspec:\n  tasks:\n" + tasks)

        env = environment(args)
        env["configs"] = [c.name for c in cfgs]
        env["start_time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        env["input_sha256"] = [sha256_file(p) for p in ctx.inputs]
        with open(os.path.join(args.out, "environment.json"), "w") as f:
            json.dump(env, f, indent=2)

        reference = None
        raw = open(os.path.join(args.out, "raw.jsonl"), "w")
        results = {c.name: [] for c in cfgs}
        for rep in range(args.warmup + args.reps):
            order = cfgs[:]
            random.Random(args.seed * 31 + rep).shuffle(order)  # randomized order per repetition, recorded below
            for c in order:
                load0 = read_text("/proc/loadavg")
                r = c.run(ctx)
                r.update({"config": c.name, "kind": c.kind, "beans": c.beans, "slots": c.slots, "cpu_budget": c.budget, "rep": rep,
                          "phase": "warmup" if rep < args.warmup else "measured", "loadavg_before": load0, "loadavg_after": read_text("/proc/loadavg"),
                          "order_in_rep": [o.name for o in order]})
                if reference is None:
                    reference = r["digests"]
                    env["reference_output_sha256"] = reference
                r["outputs_identical_to_reference"] = (r["digests"] == reference)
                raw.write(json.dumps(r) + "\n")
                raw.flush()
                print("%s rep=%d %-8s %-22s wall=%.3fs identical=%s" % (time.strftime("%H:%M:%S"), rep, r["phase"], c.name, r["wall_s"], r["outputs_identical_to_reference"]), flush=True)
                if not r["outputs_identical_to_reference"]:
                    raise SystemExit("OUTPUT MISMATCH in %s rep %d" % (c.name, rep))
                if r["phase"] == "measured":
                    results[c.name].append(r)
        raw.close()
        summary = {n: {"cpu_budget": rs[0]["cpu_budget"], "beans": rs[0]["beans"], "slots": rs[0]["slots"], "wall_s": summarize([x["wall_s"] for x in rs]),
                       "walls": [x["wall_s"] for x in rs]} for n, rs in results.items() if rs}
        env["end_time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        env["loadavg_at_end"] = read_text("/proc/loadavg")
        with open(os.path.join(args.out, "environment.json"), "w") as f:
            json.dump(env, f, indent=2)
        with open(os.path.join(args.out, "summary.json"), "w") as f:
            json.dump(summary, f, indent=2)
        print(json.dumps({k: {"median_s": round(v["wall_s"]["median"], 3), "min": round(v["wall_s"]["min"], 3), "max": round(v["wall_s"]["max"], 3)} for k, v in summary.items()}, indent=1))
    finally:
        shutil.rmtree(ctx.tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
