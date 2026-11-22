"""End-to-end tests with real `coffee serve` and `coffee join` processes (local-process simulation on one machine;
this is NOT a multi-machine measurement)."""
import json
import os
import signal
import time
import unittest

from harness import CoffeeTestCase, pid_alive, read_pid_file, wait_until

PART1 = "alpha beta gamma\ndelta\n"
PART2 = "one two\n"
PART3 = "x y z w v u\n"


def count_workload(sleep=0.6):
    return """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: count}
spec:
  tasks:
""" + "".join("""    - id: count-%d
      command: ["sh", "-c", "sleep %s; wc -w < part.txt > count.txt; hostname > host.txt; echo $COFFEE_TASK_ID-ran"]
      inputs: [{source: data/part-%d.txt, target: part.txt}]
      outputs: [count.txt]
      resources: {cpus: 1, memory_mb: 64}
""" % (i, sleep, i) for i in (1, 2, 3, 4)).replace("outputs: [count.txt]", "outputs: [count.txt, host.txt]")


class RealWorkers(CoffeeTestCase):
    def setUp(self):
        super().setUp()
        self.start_coordinator()

    def brew(self, path, *extra, timeout=90):
        return self.cli(["brew", path] + list(extra), timeout=timeout)

    def test_two_workers_process_files_with_verified_transfer(self):
        files = {"data/part-1.txt": PART1, "data/part-2.txt": PART2, "data/part-3.txt": PART3, "data/part-4.txt": "single\n"}
        wl = self.write_workload("count", count_workload(), files)
        self.start_worker("bean-a")
        self.start_worker("bean-b")
        out = os.path.join(self.tmp, "results")
        r = self.brew(wl, "--wait", "--out", out, "--job-id", "count1", "--json")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        job = self.job("count1")
        self.assertEqual(job["state"], "succeeded")
        expected = {1: 4, 2: 2, 3: 6, 4: 1}
        beans = set()
        for i, words in expected.items():
            t = self.task("count1", "count-%d" % i)
            self.assertEqual(t["state"], "succeeded")
            self.assertEqual(len(t["attempts"]), 1)
            beans.add(t["attempts"][0]["bean_id"])
            with open(os.path.join(out, "count-%d" % i, "count.txt")) as f:
                self.assertEqual(int(f.read().strip()), words)
            self.assertEqual([o["path"] for o in t["result"]], ["count.txt", "host.txt"])
            for o in t["result"]:  # reported digests match the bytes delivered to the owner
                with open(os.path.join(out, "count-%d" % i, o["path"]), "rb") as f:
                    import hashlib
                    self.assertEqual(hashlib.sha256(f.read()).hexdigest(), o["sha256"])
        self.assertEqual(beans, {"bean-a", "bean-b"}, "tasks should have been spread over both workers")
        # logs are retrievable per attempt
        r = self.cli(["logs", "count1", "count-1"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("count-1-ran", r.stdout)
        # status shows distinct identities in history
        r = self.cli(["status", "count1", "--json"])
        self.assertEqual(r.returncode, 0)
        seen = {a["bean_id"] for t in json.loads(r.stdout)["tasks"] for a in t["attempts"]}
        self.assertEqual(seen, {"bean-a", "bean-b"})

    def two_step(self):
        return self.write_workload("pipe", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: pipe}
spec:
  tasks:
    - id: upper
      command: ["sh", "-c", "tr a-z A-Z < in.txt > up.txt"]
      inputs: [{source: data/in.txt, target: in.txt}]
      outputs: [up.txt]
    - id: stats
      command: ["sh", "-c", "wc -c < data/up.txt > size.txt; cat data/up.txt >> size.txt"]
      depends_on: [upper]
      inputs: [{from_task: upper, output: up.txt, target: data/up.txt}]
      outputs: [size.txt]
""", {"data/in.txt": "hello coffee\n"})

    def test_dependency_outputs_flow_through_verified_artifacts(self):
        self.start_worker("bean-a")
        self.start_worker("bean-b")
        out = os.path.join(self.tmp, "res")
        r = self.brew(self.two_step(), "--wait", "--out", out, "--job-id", "pipe1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(os.path.join(out, "stats", "size.txt")) as f:
            self.assertEqual(f.read(), "13\nHELLO COFFEE\n")
        # dependent never ran before the producer was accepted
        up, st = self.task("pipe1", "upper"), self.task("pipe1", "stats")
        self.assertLessEqual(up["attempts"][0]["finished_at_ms"], st["attempts"][0]["claimed_at_ms"])

    def test_failed_producer_fails_consumer_visibly(self):
        wl = self.write_workload("badprod", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: bad}
spec:
  tasks:
    - id: upper
      command: ["sh", "-c", "echo boom >&2; exit 3"]
      outputs: [up.txt]
      max_attempts: 2
    - id: stats
      command: ["true"]
      depends_on: [upper]
      inputs: [{from_task: upper, output: up.txt, target: up.txt}]
""")
        self.start_worker("bean-a")
        r = self.brew(wl, "--wait", "--job-id", "bad1")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        up = self.task("bad1", "upper")
        self.assertEqual((up["state"], len(up["attempts"])), ("failed", 2))
        self.assertIn("max attempts exhausted", up["failure_reason"])
        self.assertEqual([a["exit_code"] for a in up["attempts"]], [3, 3])
        st = self.task("bad1", "stats")
        self.assertEqual(st["state"], "failed")
        self.assertIn("dependency_failed: upper", st["failure_reason"])
        self.assertEqual(st["attempts"], [])
        r = self.cli(["logs", "bad1", "upper", "--stderr", "--attempt", "2"])
        self.assertIn("boom", r.stdout)

    def test_missing_declared_output_fails_the_attempt_not_the_data(self):
        wl = self.write_workload("noout", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: noout}
spec:
  tasks:
    - {id: t, command: ["true"], outputs: [never.txt], max_attempts: 3}
""")
        self.start_worker("bean-a")
        self.assertEqual(self.brew(wl, "--wait", "--job-id", "noout1").returncode, 1)
        t = self.task("noout1", "t")
        self.assertEqual(t["state"], "failed")
        self.assertEqual(len(t["attempts"]), 1)  # deterministic failure is not retried
        self.assertIn("never.txt", t["failure_reason"])
        self.assertEqual(self.blob_files("outputs"), [])

    def test_unsatisfied_resources_are_explained_and_stay_pending(self):
        wl = self.write_workload("big", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: big}
spec:
  tasks:
    - {id: huge, command: ["true"], resources: {cpus: 64, memory_mb: 999999}}
""")
        self.start_worker("bean-a", cpus=2, memory_mb=512)
        r = self.brew(wl, "--job-id", "big1")
        self.assertEqual(r.returncode, 0, r.stderr)
        time.sleep(1.0)
        self.assertEqual(self.task("big1", "huge")["state"], "pending")
        r = self.cli(["explain", "big1", "huge", "--json"])
        self.assertEqual(r.returncode, 0, r.stderr)
        e = json.loads(r.stdout)
        self.assertTrue(e["unsatisfiable"])
        self.assertIn("capacity", " ".join(e["placement"][0]["reasons"]))
        r = self.cli(["plan", wl])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("huge", r.stdout)
        self.assertIn("cannot be placed", r.stdout)

    def test_plan_prints_dependency_layers_offline(self):
        r = self.run_cli(["plan", self.two_step()])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertLess(r.stdout.index("upper"), r.stdout.index("stats"))
        j = json.loads(self.run_cli(["plan", self.two_step(), "--json"]).stdout)
        self.assertEqual([[t["id"] for t in layer] for layer in j["layers"]], [["upper"], ["stats"]])

    def test_killing_the_running_worker_retries_on_a_survivor_and_stops_children(self):
        pidfile = os.path.join(self.tmp, "sleeper.pid")
        shellpid = os.path.join(self.tmp, "shell.pid")
        wl = self.write_workload("kill", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: kill}
spec:
  tasks:
    - id: first
      command: ["sh", "-c", "echo first > out.txt"]
      outputs: [out.txt]
    - id: victim
      command: ["sh", "-c", "echo $$ > %s; sleep 300 & echo $! > %s; wait; echo done > res.txt"]
      depends_on: [first]
      outputs: [res.txt]
      max_attempts: 3
      timeout_seconds: 120
""" % (shellpid, pidfile))
        a = self.start_worker("bean-a")
        self.assertEqual(self.brew(wl, "--job-id", "kill1").returncode, 0)
        self.wait_task("kill1", "victim", ("running",))
        sh, sleeper = read_pid_file(shellpid), read_pid_file(pidfile)
        self.extra_pids += [sh, sleeper]
        self.assertTrue(pid_alive(sh) and pid_alive(sleeper))
        a.kill9()  # SIGKILL the Bean: no cleanup code in the Bean can run
        wait_until(lambda: not pid_alive(sh) and not pid_alive(sleeper), 5, what="command and its ordinary child stop after Bean SIGKILL")
        os.unlink(pidfile)
        os.unlink(shellpid)
        self.start_worker("bean-b")
        # the retry runs on the survivor after the dead attempt's lease expires; make the retry finish fast
        wait_until(lambda: os.path.exists(pidfile), 20, what="attempt 2 started")
        survivor_sleeper = read_pid_file(pidfile)
        self.extra_pids.append(survivor_sleeper)
        os.kill(survivor_sleeper, signal.SIGTERM)  # lets `wait` return so the attempt can complete
        j = self.wait_job("kill1", timeout=30)
        self.assertEqual(j["state"], "succeeded")
        t = self.task("kill1", "victim")
        self.assertEqual([(x["bean_id"], x["state"]) for x in t["attempts"]], [("bean-a", "expired"), ("bean-b", "succeeded")])
        first = self.task("kill1", "first")  # earlier accepted results are untouched
        self.assertEqual((first["state"], first["accepted_attempt"], len(first["attempts"])), ("succeeded", 1, 1))
        self.assertEqual(self.accepted_output("kill1", "first", "out.txt"), b"first\n")

    def test_graceful_bean_stop_returns_the_attempt_for_retry(self):
        pidfile = os.path.join(self.tmp, "grace.pid")
        wl = self.write_workload("grace", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: grace}
spec:
  tasks:
    - {id: long, command: ["sh", "-c", "sleep 300 & echo $! > %s; wait"], max_attempts: 2}
""" % pidfile)
        a = self.start_worker("bean-a")
        self.brew(wl, "--job-id", "g1")
        self.wait_task("g1", "long", ("running",))
        child = read_pid_file(pidfile)
        a.p.terminate()  # SIGTERM: clean shutdown
        a.p.wait(10)
        wait_until(lambda: not pid_alive(child), 5, what="command stopped on graceful Bean stop")
        t = self.wait_task("g1", "long", ("pending",), timeout=10)  # re-queued at once, no lease wait
        self.assertEqual(t["attempts"][0]["state"], "failed")
        self.assertIn("worker stopped", t["attempts"][0]["reason"])

    def test_command_failure_timeout_and_cancellation(self):
        pidfile = os.path.join(self.tmp, "cancel.pid")
        wl = self.write_workload("limits", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: limits}
spec:
  tasks:
    - {id: slow, command: ["sh", "-c", "sleep 300 & echo $! > %s; wait"], timeout_seconds: 1, max_attempts: 1}
    - {id: fails, command: ["sh", "-c", "exit 5"], max_attempts: 2}
""" % pidfile)
        self.start_worker("bean-a", cpus=2)
        self.assertEqual(self.brew(wl, "--wait", "--job-id", "lim1").returncode, 1)
        slow = self.task("lim1", "slow")
        self.assertEqual(slow["state"], "failed")
        self.assertIn("timed out", slow["failure_reason"])
        child = read_pid_file(pidfile)
        wait_until(lambda: not pid_alive(child), 5, what="timed-out child stopped")
        fails = self.task("lim1", "fails")
        self.assertEqual((fails["state"], len(fails["attempts"]), fails["attempts"][0]["exit_code"]), ("failed", 2, 5))

    def test_cancel_stops_running_command_and_blocks_publication(self):
        pidfile = os.path.join(self.tmp, "c2.pid")
        wl = self.write_workload("cancel", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: cancel}
spec:
  tasks:
    - id: long
      command: ["sh", "-c", "sleep 300 & echo $! > %s; wait; echo late > out.txt"]
      outputs: [out.txt]
""" % pidfile)
        self.start_worker("bean-a")
        self.brew(wl, "--job-id", "c1")
        self.wait_task("c1", "long", ("running",))
        child = read_pid_file(pidfile)
        self.assertEqual(self.cli(["cancel", "c1"]).returncode, 0)
        wait_until(lambda: not pid_alive(child), 6, what="cancelled command stopped")
        self.assertEqual(self.job("c1")["state"], "cancelled")
        self.assertEqual(self.blob_files("outputs"), [])

    def test_hostile_to_limits_workloads(self):
        wl = self.write_workload("hostile", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: hostile}
spec:
  tasks:
    - id: hog
      command: ["perl", "-e", "$x = 'a' x (400*1024*1024); print length($x)"]
      resources: {memory_mb: 64}
      max_attempts: 1
    - id: flood
      command: ["sh", "-c", "head -c 3000000 /dev/zero > big.bin"]
      outputs: [big.bin]
      max_attempts: 1
    - id: pinned
      command: ["sh", "-c", "grep Cpus_allowed_list /proc/self/status > cpus.txt"]
      outputs: [cpus.txt]
""")
        self.coordinator.stop()
        self.procs.remove(self.coordinator)
        self.start_coordinator(["--max-artifact-bytes", "1000000"])
        self.start_worker("bean-a", cpus=3, extra=["--max-artifact-bytes", "1000000"])
        self.assertEqual(self.brew(wl, "--wait", "--job-id", "h1").returncode, 1)
        hog, flood, pinned = (self.task("h1", t) for t in ("hog", "flood", "pinned"))
        self.assertEqual(hog["state"], "failed")
        self.assertNotEqual(hog["attempts"][0]["exit_code"], 0)
        self.assertEqual(flood["state"], "failed")
        self.assertEqual(pinned["state"], "succeeded")
        cpus = self.accepted_output("h1", "pinned", "cpus.txt").decode().split(":")[1].strip()
        self.assertNotIn("-", cpus.replace(",", ""))  # exactly one CPU allowed for a 1-cpu task
        self.assertEqual(len(cpus.split(",")), 1)
        self.assertEqual(self.blob_files("outputs") and len(self.blob_files("outputs")), 1)  # only the pinned result was published

    def test_path_and_symlink_escapes_are_refused(self):
        secret = os.path.join(self.tmp, "secret.txt")
        with open(secret, "w") as f:
            f.write("TOP-SECRET")
        outside = os.path.join(self.tmp, "outside")
        os.makedirs(outside)
        wl = self.write_workload("escape", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: escape}
spec:
  tasks:
    - {id: leaf, command: ["sh", "-c", "ln -s %s out.txt"], outputs: [out.txt], max_attempts: 1}
    - {id: dirlink, command: ["sh", "-c", "ln -s %s sub; echo x > %s/real.txt"], outputs: [sub/real.txt], max_attempts: 1}
    - {id: dirout, command: ["sh", "-c", "mkdir d"], outputs: [d], max_attempts: 1}
""" % (secret, outside, outside))
        self.start_worker("bean-a", cpus=3)
        self.assertEqual(self.brew(wl, "--wait", "--job-id", "esc1").returncode, 1)
        for t in ("leaf", "dirlink", "dirout"):
            task = self.task("esc1", t)
            self.assertEqual(task["state"], "failed", t)
            self.assertIn("unsafe", task["failure_reason"], t)
        self.assertEqual(self.blob_files("outputs"), [])
        for root, _, fs_ in os.walk(self.state):
            for fn in fs_:
                with open(os.path.join(root, fn), "rb") as f:
                    if os.path.basename(root) == "outputs":
                        self.assertNotIn(b"TOP-SECRET", f.read())

    def test_drain_stops_new_work_on_that_worker(self):
        self.start_worker("bean-a")
        self.start_worker("bean-b")
        self.assertEqual(self.cli(["drain", "bean-a"]).returncode, 0)
        files = {"data/part-%d.txt" % i: "a b c\n" for i in (1, 2, 3, 4)}
        wl = self.write_workload("drain", count_workload(0.2), files)
        self.assertEqual(self.brew(wl, "--wait", "--job-id", "d1").returncode, 0)
        beans = {a["bean_id"] for t in self.job("d1")["tasks"] for a in t["attempts"]}
        self.assertEqual(beans, {"bean-b"})

    def test_coordinator_restart_with_live_worker_resumes_work(self):
        self.start_worker("bean-a")
        files = {"data/part-%d.txt" % i: "a b c\n" for i in (1, 2, 3, 4)}
        wl = self.write_workload("restart", count_workload(0.8), files)
        self.assertEqual(self.brew(wl, "--job-id", "r1").returncode, 0)
        self.wait_task("r1", "count-1", ("running",))
        self.coordinator.kill9()
        time.sleep(0.4)
        self.start_coordinator()
        j = self.wait_job("r1", timeout=60)
        self.assertEqual(j["state"], "succeeded")
        self.assertEqual({a["bean_id"] for t in j["tasks"] for a in t["attempts"]}, {"bean-a"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
