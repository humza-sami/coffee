"""Live smoke tests for `coffee resources` against the real kernel: CPU affinity under taskset and cgroup v2 limits under a
transient `systemd-run --user --scope` (memory/CPU limits). The expected values are read straight from the cgroup files by this
test's own small walker, independent of the C++ resolver. Cases the host cannot provide (no user systemd manager, controller not
delegated) are reported as SKIPPED with the reason, never as passed. No host setting is changed: the scope is transient."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from harness import COFFEE

CG_ROOT = "/sys/fs/cgroup"

# Runs inside the scope: independently walk the real cgroup tree, then run coffee and print both views as JSON.
INSIDE = r'''
import json, os, subprocess, sys
def read(p):
    try:
        return open(p).read().strip()
    except OSError:
        return None
rel = [l.split(":", 2)[2] for l in open("/proc/self/cgroup").read().splitlines() if l.startswith("0::")][0]
groups = []
cur = rel
while True:
    groups.append(cur)
    if cur in ("/", ""):
        break
    cur = os.path.dirname(cur) or "/"
limits, quotas, missing = [], [], []
for g in groups:
    d = os.path.join("/sys/fs/cgroup", g.lstrip("/"))
    mm, cm = read(d + "/memory.max"), read(d + "/cpu.max")
    if g != "/":
        if mm is None or cm is None:
            missing.append(g)
    if mm not in (None, "max"):
        limits.append(int(mm))
    if cm is not None and not cm.startswith("max"):
        q, p = cm.split(); quotas.append(int(q) / int(p))
out = subprocess.run([sys.argv[1], "resources", "--json", "--path", "/tmp"], capture_output=True, text=True)
print(json.dumps({"rc": out.returncode, "coffee": json.loads(out.stdout), "expect": {"limit": min(limits) if limits else None,
      "quota": min(quotas) if quotas else None, "missing": missing}}))
'''


def resources(*args, prefix=()):
    r = subprocess.run(list(prefix) + [COFFEE, "resources", "--json", *args], capture_output=True, text=True, timeout=60)
    return r.returncode, json.loads(r.stdout), r.stderr


class Affinity(unittest.TestCase):
    def test_taskset_restricts_logical_cpus_and_budget_never_counts_machine_threads(self):
        allowed = sorted(os.sched_getaffinity(0))
        if len(allowed) < 2 or shutil.which("taskset") is None:
            self.skipTest("need >= 2 allowed CPUs and taskset")
        for n in (1, 2):
            cpus = ",".join(str(c) for c in allowed[:n])
            rc, j, err = resources(prefix=["taskset", "-c", cpus])
            self.assertEqual(rc, 0, err)
            self.assertEqual(j["cpu"]["affinity_logical_cpus"], n)
            if j["cpu"]["effective_cores"] is not None:
                self.assertLessEqual(j["cpu"]["effective_cores"], n)
                self.assertLessEqual(j["budget"]["max_cpu_slots"], n)
            self.assertLessEqual(j["cpu"]["physical_cores"] or 0, n)  # physical cores of the allowed CPUs only
        # a request for more slots than the affinity set is rejected even if the machine has more threads
        rc, j, _ = resources("--cpus", "2", prefix=["taskset", "-c", str(allowed[0])])
        self.assertEqual(rc, 1)
        self.assertFalse(j["ok"])
        self.assertFalse(j["request"]["valid"])


class Cgroup(unittest.TestCase):
    def run_in_scope(self, *props):
        if shutil.which("systemd-run") is None or not os.path.isdir(CG_ROOT) or not os.path.exists(CG_ROOT + "/cgroup.controllers"):
            self.skipTest("no systemd-run or cgroup v2 mount")
        cmd = ["systemd-run", "--user", "--scope", "--quiet", "--collect"]
        for p in props:
            cmd += ["-p", p]
        cmd += [sys.executable, "-I", "-c", INSIDE, COFFEE]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if r.returncode != 0 or not r.stdout.strip():
            self.skipTest("systemd --user scope unavailable (exit %d): %s" % (r.returncode, r.stderr.strip()[:300]))
        return json.loads(r.stdout.strip().splitlines()[-1])

    def test_memory_limit_matches_files_and_live_headroom_bounds_hold(self):
        d = self.run_in_scope("MemoryMax=512M", "MemorySwapMax=0")
        exp, j = d["expect"], d["coffee"]
        if exp["limit"] is None or exp["limit"] > 512 * 1024 * 1024:
            self.skipTest("memory controller not delegated to the user scope (no limit visible in cgroup files)")
        self.assertEqual(d["rc"], 0)
        self.assertEqual(j["memory"]["cgroup_status"], "limited")
        self.assertEqual(j["memory"]["cgroup_limit_bytes"], exp["limit"])
        self.assertLessEqual(exp["limit"], 512 * 1024 * 1024)
        # memory.current changes during Coffee startup (especially under ASAN), so a
        # pre-spawn sample cannot be an equality oracle, even with a fixed margin.
        # The scope's live Python process consumes memory throughout the observation:
        # headroom must be below its finite limit, but a full ancestor may leave zero.
        # Exact min(max-current) ancestor arithmetic is covered by deterministic unit fixtures.
        headroom = j["memory"]["cgroup_headroom_bytes"]
        self.assertIs(type(headroom), int)
        self.assertGreaterEqual(headroom, 0)
        self.assertLess(headroom, exp["limit"])
        self.assertEqual(j["memory"]["effective_available_bytes"], min(headroom, j["memory"]["host_available_bytes"]))
        self.assertEqual(j["budget"]["max_memory_bytes"], max(0, j["memory"]["effective_available_bytes"] - j["budget"]["reserve_memory_bytes"]))

    def test_cpu_quota_matches_the_real_cgroup_files(self):
        d = self.run_in_scope("CPUQuota=150%", "MemoryMax=512M")
        exp, j = d["expect"], d["coffee"]
        if exp["quota"] is None or abs(exp["quota"] - 1.5) > 1e-9:
            self.skipTest("cpu controller not delegated to the user scope (cpu.max shows no 150%% quota; found %r)" % exp["quota"])
        if exp["missing"]:
            self.skipTest("limit files missing on non-root groups %r, coffee reports unknown by design" % exp["missing"])
        self.assertEqual(j["cpu"]["quota_status"], "limited")
        self.assertAlmostEqual(j["cpu"]["quota_cores"], 1.5)
        self.assertAlmostEqual(j["cpu"]["effective_cores"], min(1.5, j["cpu"]["affinity_logical_cpus"]))
        self.assertEqual(j["budget"]["max_cpu_slots"], 1)

    def test_unlimited_scope_reports_what_the_files_show(self):
        d = self.run_in_scope()
        exp, j = d["expect"], d["coffee"]
        if exp["missing"]:
            # a controller file is absent on a non-root group: unknown by design, never silently unlimited
            self.assertTrue(j["cpu"]["quota_status"] == "unknown" or j["memory"]["cgroup_status"] == "unknown", j)
            self.assertTrue(j["warnings"])
            self.assertIsNone(j["cpu"]["effective_cores"] if j["cpu"]["quota_status"] == "unknown" else None)
        else:
            self.assertEqual(j["cpu"]["quota_status"], "limited" if exp["quota"] is not None else "unlimited")
            self.assertEqual(j["memory"]["cgroup_status"], "limited" if exp["limit"] is not None else "unlimited")


class NonUtf8(unittest.TestCase):
    """Real processes with raw byte argv: Linux file names and arguments need not be UTF-8."""

    def run_bytes(self, *args):
        r = subprocess.run([os.fsencode(COFFEE), b"resources", *args], capture_output=True, timeout=60)
        return r.returncode, r.stdout, r.stderr

    def test_existing_directory_with_invalid_utf8_name_is_observed(self):
        tmp = os.fsencode(tempfile.mkdtemp(prefix="coffee-test-bytes-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        for name in (b"bad\xff", b"trunc\xe2\x82", b"mid\xe2\x82-x"):
            path = tmp + b"/" + name
            os.mkdir(path)
            with open(path + b"/sentinel", "wb") as f:
                f.write(b"keep")
            before = os.listdir(path)
            rc, out, err = self.run_bytes(b"--json", b"--path", path)
            self.assertEqual(rc, 0, (name, out, err))
            j = json.loads(out.decode("utf-8"))  # strict: stdout must itself be valid UTF-8 JSON
            self.assertEqual(out.count(b"\n"), 1)
            self.assertIn("\ufffd", j["storage"]["path"])
            self.assertEqual(j["storage"]["total_bytes"], os.statvfs(path).f_blocks * os.statvfs(path).f_frsize)
            self.assertTrue(any("storage.path" in w for w in j["warnings"]), j["warnings"])
            self.assertEqual(os.listdir(path), before)  # nothing created or removed

    def test_missing_byte_path_reports_the_underlying_io_error(self):
        rc, out, err = self.run_bytes(b"--json", b"--path", b"/nonexistent-\xff")
        self.assertEqual(rc, 1)
        j = json.loads(out.decode("utf-8"))
        self.assertEqual(j["error"]["code"], "io_error")
        self.assertIn("No such file", j["error"]["message"])
        self.assertNotIn("json.exception", j["error"]["message"])

    def test_invalid_byte_arguments_are_usage_errors_not_aborts(self):
        for argv in ([b"--json", b"--bad\xff"], [b"--json", b"--cpus", b"\xff"], [b"--bad\xff", b"--json"], [b"--json", b"stray\xe2\x82"],
                     [b"--json", b"--memory-mb", b"1\xff"]):
            rc, out, err = self.run_bytes(*argv)
            self.assertEqual(rc, 2, (argv, rc, out, err))  # -6 would be SIGABRT
            j = json.loads(out.decode("utf-8"))
            self.assertEqual(j["error"]["code"], "invalid_argument")
            self.assertEqual(j["ok"], False)
            self.assertEqual(err, b"")
        rc, out, err = self.run_bytes(b"--bad\xff")  # human mode
        self.assertEqual(rc, 2)
        self.assertEqual(out, b"")
        self.assertIn(b"unknown option", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
