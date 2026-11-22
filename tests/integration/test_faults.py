"""Fault-injection and resource-bound regressions (correction round 1): durability failures, slow publication,
damaged content-addressed blobs and the aggregate per-attempt workspace cap. Real processes, loopback, temp state.
The LD_PRELOAD fixture (fixtures/fault_fsync.c) is compiled on the fly and only loaded into the coordinator."""
import os
import threading
import time
import unittest

import test_protocol as tp
from harness import COFFEE, CoffeeTestCase, Proc, build_fault_preload, preload_value, wait_until

FAST_SLOW = """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: contend}
spec:
  tasks:
    - id: fast
      command: ["sh", "-c", "sleep 0.7; echo fast > out"]
      outputs: [out]
      max_attempts: 1
    - id: ongoing
      command: ["sh", "-c", "sleep 8; echo long > out"]
      outputs: [out]
      max_attempts: 1
"""


class FaultMixin:
    def fault_env(self):
        so = build_fault_preload(self.tmp)
        self.fault_file = os.path.join(self.tmp, "fault.trigger")
        return {"LD_PRELOAD": preload_value(so), "COFFEE_FAULT_FILE": self.fault_file}

    def set_fault(self, mode):
        with open(self.fault_file, "w") as f:
            f.write(mode)

    def clear_fault(self):
        try:
            os.unlink(self.fault_file)
        except FileNotFoundError:
            pass

    def injections(self, marker="FAULT-INJECTED"):
        return self.read_log("serve.log").count(marker)


ONE = """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: one}
spec:
  tasks:
    - id: t
      command: ["sh", "-c", "printf %s > out"]
      outputs: [out]
      max_attempts: 1
"""


class DurabilityFaults(CoffeeTestCase, FaultMixin):
    LEASE_MS = 2000
    HEARTBEAT_TIMEOUT_MS = 3000

    def test_directory_sync_failure_prevents_acceptance_and_keeps_old_results(self):
        self.start_coordinator(env=self.fault_env())
        self.start_worker("bean-a")
        good = self.brew("good", "first")
        self.assertEqual(self.cli(["brew", good, "--wait", "--job-id", "ok1"]).returncode, 0)
        old_sha = self.task("ok1", "t")["result"][0]["sha256"]
        self.set_fault("eio")
        r = self.cli(["brew", self.brew("bad", "second"), "--wait", "--timeout", "40", "--job-id", "bad1"], timeout=60)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertGreater(self.injections(), 0)
        t = self.task("bad1", "t")
        self.assertNotEqual(t["state"], "succeeded")
        self.assertIsNone(t["result"])  # no accepted metadata referencing a blob whose durability failed
        self.assertEqual(self.accepted_output("ok1", "t", "out"), b"first")  # earlier accepted result untouched
        self.assertEqual(self.task("ok1", "t")["result"][0]["sha256"], old_sha)
        # the fault is transient: the same cluster accepts new work afterwards
        self.clear_fault()
        self.assertEqual(self.cli(["brew", self.brew("good2", "third"), "--wait", "--job-id", "ok2"]).returncode, 0)
        self.assertEqual(self.accepted_output("ok2", "t", "out"), b"third")

    def brew(self, name, text):
        return self.write_workload(name, ONE.replace("%s", text))

    def test_input_reuse_and_job_submission_need_a_successful_input_sync(self):
        import hashlib
        self.start_coordinator(env=self.fault_env())
        self.start_worker("bean-a")
        self.assertEqual(self.cli(["brew", self.brew("old", "kept"), "--wait", "--job-id", "old1"]).returncode, 0)  # earlier accepted result
        data = b"fix1 input bytes"
        sha = hashlib.sha256(data).hexdigest()
        path = "/v1/inputs/" + sha
        self.set_fault("eio-inputs")
        first = self.api.put(path, data)
        self.assertEqual(first[0], 500, first)
        self.assertGreater(self.injections(), 0)
        for _ in range(2):  # a blob that is merely present and valid is not proof that it was ever durable
            st, body = self.api.put(path, data)
            self.assertEqual(st, 500, body)
        injected = self.injections()
        self.assertGreaterEqual(injected, 3)  # every attempt really ran the required sync
        # Rejected publication/reuse must also reclaim the unique input scopes.
        self.assertFalse(any(name.startswith("inputs-") for name in os.listdir(os.path.join(self.state, "artifacts", "tmp"))))
        # a caller that skips the PUT result and submits the manifest directly must not get an accepted job
        wlj = tp.wl([{"id": "task", "command": ["sh", "-c", "test -s in && cp in out"], "inputs": [{"source": "in", "target": "in"}], "outputs": ["out"]}])
        body = {"job_id": "direct", "workload": wlj, "files": {"in": {"sha256": sha, "size": len(data)}}}
        st, resp = self.api.post("/v1/jobs", body)
        self.assertNotIn(st, (200, 201), resp)
        self.assertEqual(self.api.get("/v1/jobs/direct")[0], 404)  # nothing was recorded
        # the fault clears: the same upload + job now succeed, the digest is unchanged and old results are intact
        self.clear_fault()
        st, resp = self.api.put(path, data)
        self.assertIn(st, (200, 201), resp)
        st, resp = self.api.post("/v1/jobs", body)
        self.assertIn(st, (200, 201), resp)
        self.assertEqual(self.wait_job("direct")["state"], "succeeded")
        self.assertEqual(self.accepted_output("direct", "task", "out"), data)
        self.assertEqual(self.blob_files("inputs"), [sha])
        self.assertEqual(self.accepted_output("old1", "t", "out"), b"kept")

    def test_unscoped_directory_sync_failure_stops_the_coordinator_loudly(self):
        env = self.fault_env()
        self.set_fault("eio-all")
        # The ready-file write is an atomic-write path: the failed parent-directory sync must make `serve` exit non-zero
        # (the file may already be visible after the rename, so wait for the exit rather than for the file).
        p = Proc([COFFEE, "serve", "--state", self.state, "--listen", "127.0.0.1:%d" % self.port, "--ready-file", os.path.join(self.tmp, "r.json")],
                 os.path.join(self.tmp, "serve.log"), env=env)
        self.procs.append(p)
        self.assertIsNotNone(wait_until(lambda: p.p.poll() is not None, 20, what="coordinator to exit on the sync failure") or True)
        self.assertNotEqual(p.p.returncode, 0)
        self.assertIn("fsync directory", self.read_log("serve.log"))


class SlowPublication(CoffeeTestCase, FaultMixin):
    LEASE_MS = 2500
    HEARTBEAT_TIMEOUT_MS = 5000

    def test_slow_output_sync_does_not_expire_unrelated_or_publishing_work(self):
        self.start_coordinator(env=self.fault_env())
        self.set_fault("slow:4000")
        self.start_worker("bean-a")
        self.start_worker("bean-b")
        r = self.cli(["brew", self.write_workload("contend", FAST_SLOW), "--wait", "--timeout", "40", "--job-id", "contend"], timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertGreater(self.injections(), 0)
        for tid in ("fast", "ongoing"):
            t = self.task("contend", tid)
            self.assertEqual((t["state"], [a["state"] for a in t["attempts"]]), ("succeeded", ["succeeded"]), tid)  # no expiry, no retry
        self.assertEqual(self.accepted_output("contend", "fast", "out"), b"fast\n")


class PreparationFence(tp.ProtocolBase, FaultMixin):
    LEASE_MS = 2500
    HEARTBEAT_TIMEOUT_MS = 5000

    def coordinator_env(self):
        return self.fault_env()

    def test_lease_expiring_during_slow_preparation_is_never_accepted_and_renewals_stay_live(self):
        self.register("fw1", 1, 512)
        self.register("fw2", 1, 512)
        self.submit(tp.wl([{"id": "slow", "command": ["true"], "outputs": ["o.txt"], "max_attempts": 1},
                           {"id": "other", "command": ["true"], "max_attempts": 1}]), {}, "j")
        c1, c2 = self.claim("fw1"), self.claim("fw2")
        slow, other = (c1, c2) if c1["task_id"] == "slow" else (c2, c1)
        sb, ob = ("fw1", "fw2") if c1["task_id"] == "slow" else ("fw2", "fw1")
        out = b"slow-output"
        self.assertEqual(self.upload(slow["lease_id"], sb, out)[0], 201)
        self.set_fault("slow:5000")
        result = {}
        th = threading.Thread(target=lambda: result.update(r=self.complete(slow["lease_id"], sb, [self.out_entry("o.txt", out)])))
        t0 = time.time()
        th.start()
        wait_until(lambda: self.injections() > 0, 5, what="slow publication started")
        worst = 0.0
        while th.is_alive():  # the unrelated Bean keeps renewing while the other publication is stalled
            s = time.time()
            st, body = self.api.post("/v1/leases/%s/renew" % other["lease_id"], {"bean_id": ob})
            worst = max(worst, time.time() - s)
            self.assertEqual(st, 200, body)
            # Even after expiry, reaping must leave the publisher's staged files pinned
            # until its slow filesystem operation and final fenced commit have finished.
            if time.time() - t0 > 3.0:
                self.assertTrue(os.path.isdir(os.path.join(self.state, "artifacts", "tmp", slow["lease_id"])))
            time.sleep(0.4)
        th.join()
        self.assertGreater(time.time() - t0, 4.5)
        self.assertLess(worst, 1.0, "renewals were serialized behind artifact publication")
        st, body = result["r"]  # the publishing attempt never renewed: its lease lapsed during preparation
        self.assertEqual((st, body["error"]["code"]), (409, "expired"), body)
        t = self.task("j", "slow")
        self.assertIsNone(t["result"])
        self.assertEqual(t["state"], "failed")  # max_attempts 1
        self.assertEqual(self.task("j", "other")["state"], "running")  # unrelated work was not expired by it
        self.assertEqual(self.complete(other["lease_id"], ob, [])[0], 200)
        wait_until(lambda: not os.path.exists(os.path.join(self.state, "artifacts", "tmp", slow["lease_id"])), 6)

    def test_cancellation_during_slow_completion_defers_cleanup_and_fences_acceptance(self):
        self.register("fw1", 1, 512)
        self.submit(tp.wl([{"id": "t", "command": ["true"], "outputs": ["o.txt"]}]), {}, "j")
        claim = self.claim("fw1"); lease = claim["lease_id"]; out = b"cancelled-output"
        self.assertEqual(self.upload(lease, "fw1", out)[0], 201)
        self.set_fault("slow:3000")
        result = {}
        thread = threading.Thread(target=lambda: result.update(r=self.complete(lease, "fw1", [self.out_entry("o.txt", out)])))
        thread.start()
        wait_until(lambda: self.injections() > 0, 5, what="slow publication started")
        self.assertEqual(self.api.post("/v1/jobs/j/cancel")[0], 200)
        time.sleep(0.7)
        scope = os.path.join(self.state, "artifacts", "tmp", lease)
        self.assertTrue(os.path.isdir(scope))
        thread.join(10)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result["r"][0], 409, result)
        self.assertIsNone(self.task("j", "t")["result"])
        wait_until(lambda: not os.path.exists(scope), 6)



class WorkspaceCap(CoffeeTestCase):
    CAP = ["--max-artifact-bytes", "1000000", "--max-workspace-bytes", "2000000"]

    def setUp(self):
        super().setUp()
        self.start_coordinator()
        self.start_worker("bean-a", extra=self.CAP)

    def run_one(self, name, command, outputs="[out]", extra_task="", files=None):
        wl = self.write_workload(name, """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: %s}
spec:
  tasks:
    - {id: t, command: %s, outputs: %s, max_attempts: 3%s}
""" % (name, command, outputs, extra_task), files)
        r = self.cli(["brew", wl, "--wait", "--timeout", "40", "--job-id", name], timeout=60)
        return r, self.task(name, "t")

    def assert_cap_failure(self, t):
        self.assertEqual(t["state"], "failed")
        self.assertEqual(len(t["attempts"]), 1)  # deterministic: not retried
        self.assertIn("workspace limit exceeded", t["failure_reason"])
        self.assertIsNone(t["result"])
        self.assertEqual(self.blob_files("outputs"), [])

    def test_multiple_files_each_under_the_per_file_limit_exceed_the_aggregate_cap(self):
        r, t = self.run_one("multi", '["sh", "-c", "for i in 1 2 3 4; do head -c 900000 /dev/zero > file$i; done; echo done > out"]')
        self.assertNotEqual(r.returncode, 0)
        self.assert_cap_failure(t)

    def test_ordinary_child_growth_is_stopped_and_cleaned_up(self):
        pidfile = os.path.join(self.tmp, "writer.pid")
        r, t = self.run_one("child", '["sh", "-c", "(echo $$ > %s; for i in $(seq 1 40); do head -c 900000 /dev/zero > c$i; sleep 0.1; done) & wait; echo done > out"]' % pidfile)
        self.assertNotEqual(r.returncode, 0)
        self.assert_cap_failure(t)
        from harness import pid_alive, read_pid_file
        pid = read_pid_file(pidfile)
        wait_until(lambda: not pid_alive(pid), 5, what="ordinary child writer killed")
        att = os.path.join(self.tmp, "work-bean-a", "attempts")
        self.assertEqual(os.listdir(att), [])  # workspace removed afterwards

    def test_declared_inputs_over_the_cap_are_refused_before_staging(self):
        r, t = self.run_one("bigin", '["true"]', outputs="[]", extra_task=", inputs: [{source: big.bin, target: big.bin}]", files={"big.bin": b"x" * 2_500_000})
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(t["state"], "failed")
        self.assertIn("declared inputs", t["failure_reason"])
        self.assertEqual(len(t["attempts"]), 1)

    def test_unlinked_open_files_count_and_the_tree_is_stopped(self):
        pidfile = os.path.join(self.tmp, "u.pid")
        perl = ('open(P,">",%s); print P $$; close P; for $i (1..4) { my $f; open($f,">","u$i") or die; unlink "u$i"; '
                'syswrite($f,"x" x 900000); push @f,$f } sleep 3; open(O,">","out"); print O "ok"; close O;' % ('"%s"' % pidfile))
        import json as _json
        t0 = time.time()
        r, t = self.run_one("unl", _json.dumps(["perl", "-e", perl]))
        self.assertNotEqual(r.returncode, 0)
        self.assert_cap_failure(t)
        self.assertLess(time.time() - t0, 2.9 + 3, "must be stopped by the sampler, not after the 3 s hold")
        from harness import pid_alive, read_pid_file
        wait_until(lambda: not pid_alive(read_pid_file(pidfile)), 5, what="writer reaped")

    def test_normal_unlinked_tempfile_pattern_and_directory_churn_succeed(self):
        r, t = self.run_one("tmpfiles", '["perl", "-e", "for $i (1..30) { open($f,q(>),qq(t$i)); unlink qq(t$i); syswrite($f,q(x) x 100000); close $f; mkdir qq(d$i); open(G,q(>),qq(d$i/f)); print G q(y) x 50000; close G; } open(O,q(>),q(out)); print O q(ok); close O;"]')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(t["state"], "succeeded")

    def test_normal_bounded_workload_still_succeeds(self):
        r, t = self.run_one("fine", '["sh", "-c", "head -c 600000 /dev/zero > a; head -c 600000 /dev/zero > b; echo ok > out"]')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(t["state"], "succeeded")

    def test_zero_cap_is_a_usage_error_not_a_silent_unlimited(self):
        r = self.run_cli(["join", "--server", self.url, "--token-file", self.token_file, "--work-dir", os.path.join(self.tmp, "w0"), "--max-workspace-bytes", "0"])
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("--max-workspace-bytes", r.stderr)


class ExistingBlobRepair(CoffeeTestCase):
    def test_corrupt_existing_output_blob_is_repaired_before_the_new_reference_is_accepted(self):
        import hashlib
        self.start_coordinator()
        self.start_worker("bean-a")
        wl = self.write_workload("repeat", ONE.replace("%s", "abc"))
        self.assertEqual(self.cli(["brew", wl, "--wait", "--job-id", "first"]).returncode, 0)
        blob = os.path.join(self.state, "artifacts", "outputs", hashlib.sha256(b"abc").hexdigest())
        os.chmod(blob, 0o600)
        with open(blob, "wb") as f:
            f.write(b"xyz")  # same length, wrong content
        r = self.cli(["brew", wl, "--wait", "--job-id", "second"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.accepted_output("second", "t", "out"), b"abc")  # not HTTP 500 artifact_corrupt
        self.assertEqual(self.accepted_output("first", "t", "out"), b"abc")
        self.assertEqual(self.blob_files("outputs"), [hashlib.sha256(b"abc").hexdigest()])


if __name__ == "__main__":
    unittest.main(verbosity=2)
