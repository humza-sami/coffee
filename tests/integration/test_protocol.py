"""Protocol-level integration tests: a real `coffee serve` process driven over real HTTP by a scripted
"worker" so stale, cancelled, expired and interrupted publications are deterministic."""
import hashlib
import http.client
import json
import os
import socket
import subprocess
import time
import unittest

from harness import COFFEE, CoffeeTestCase, free_port, wait_until

API_VERSION = "coffee/v1alpha1"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def wl(tasks, name="w"):
    return {"apiVersion": API_VERSION, "kind": "Workload", "metadata": {"name": name}, "spec": {"tasks": tasks}}


def producer_consumer():
    return wl([
        {"id": "produce", "command": ["true"], "inputs": [{"source": "in.txt", "target": "in.txt"}], "outputs": ["up.txt"]},
        {"id": "consume", "command": ["true"], "depends_on": ["produce"],
         "inputs": [{"from_task": "produce", "output": "up.txt", "target": "in/up.txt"}], "outputs": ["n.txt"]},
    ])


class ProtocolBase(CoffeeTestCase):
    def coordinator_env(self):
        return None

    def setUp(self):
        super().setUp()
        self.start_coordinator(env=self.coordinator_env())

    def register(self, bean, cpus=2, mem=2048):
        st, body = self.api.post("/v1/beans", {"id": bean, "cpus": cpus, "memory_mb": mem})
        self.assertEqual(st, 200, body)
        return body

    def put_input(self, data):
        st, body = self.api.put("/v1/inputs/" + sha(data), data)
        self.assertIn(st, (200, 201), body)

    def submit(self, workload, files=None, job_id="job1"):
        files = files or {}
        meta = {}
        for src, data in files.items():
            self.put_input(data)
            meta[src] = {"sha256": sha(data), "size": len(data)}
        st, body = self.api.post("/v1/jobs", {"job_id": job_id, "workload": workload, "files": meta})
        self.assertIn(st, (200, 201), body)
        return body

    def claim(self, bean):
        st, body = self.api.post("/v1/beans/%s/claim" % bean)
        self.assertEqual(st, 200, body)
        return body["claim"]

    def upload(self, lease, bean, data):
        return self.api.put("/v1/leases/%s/uploads/%s" % (lease, sha(data)), data, headers={"X-Coffee-Bean": bean})

    def complete(self, lease, bean, outputs, success=True, **extra):
        body = {"bean_id": bean, "success": success, "exit_code": 0 if success else 1, "outputs": outputs}
        body.update(extra)
        return self.api.post("/v1/leases/%s/complete" % lease, body)

    def out_entry(self, path, data):
        return {"path": path, "size": len(data), "sha256": sha(data)}


class AuthAndLimits(ProtocolBase):
    def test_health_is_open_everything_else_needs_the_token(self):
        self.assertEqual(self.api.get("/v1/health", token=None)[0], 200)
        for method, path in [("GET", "/v1/version"), ("GET", "/v1/beans"), ("GET", "/v1/jobs"), ("POST", "/v1/beans"), ("PUT", "/v1/inputs/" + "0" * 64),
                             ("POST", "/v1/beans/x/claim"), ("POST", "/v1/leases/x/complete"), ("GET", "/v1/jobs/x")]:
            for tok in (None, "wrong-token", "x" * 64):
                st, body = self.api.request(method, path, {} if method != "GET" else None, token=tok)
                self.assertEqual(st, 401, (method, path, tok, body))
                self.assertEqual(body["error"]["code"], "unauthorized")
        # token is never echoed back
        st, body = self.api.get("/v1/version", token="wrong-token")
        self.assertNotIn("wrong-token", json.dumps(body))
        self.assertEqual(self.api.get("/v1/version")[0], 200)

    def test_token_never_appears_in_logs_or_status_documents(self):
        self.api.get("/v1/version", token="wrong-token-123")
        self.api.get("/v1/jobs")
        time.sleep(0.2)
        self.assertNotIn(self.token, self.read_log("serve.log"))
        self.assertNotIn("wrong-token-123", self.read_log("serve.log"))
        st, version = self.api.get("/v1/version")
        self.assertNotIn(self.token, json.dumps(version))

    def test_malformed_and_invalid_requests_are_controlled_errors(self):
        for body in ["{not json", "", "[1,2", '{"id": }', "\x00\x01"]:
            st, resp = self.api.request("POST", "/v1/beans", body)
            self.assertEqual(st, 400, (body, resp))
            self.assertEqual(resp["error"]["code"], "invalid_json")
        st, resp = self.api.post("/v1/beans", {"cpus": "many", "memory_mb": 10})
        self.assertEqual((st, resp["error"]["code"]), (400, "invalid_argument"))
        st, resp = self.api.post("/v1/beans", {"id": "../../etc", "cpus": 1, "memory_mb": 10})
        self.assertEqual((st, resp["error"]["code"]), (400, "invalid_argument"))
        st, resp = self.api.post("/v1/jobs", {"workload": {"apiVersion": "nope"}})
        self.assertEqual((st, resp["error"]["code"]), (400, "invalid_workload"))
        self.assertIn("field", resp["error"])
        st, resp = self.api.post("/v1/jobs", [1, 2, 3])
        self.assertEqual(st, 400)
        self.assertEqual(self.api.get("/v1/nope")[0], 404)
        self.assertEqual(self.api.get("/v1/nope")[1]["error"]["code"], "not_found")
        self.assertEqual(self.api.post("/v1/leases/lease-doesnotexist/renew", {"bean_id": "b"})[0], 404)
        self.assertEqual(self.api.post("/v1/beans/ghost/claim")[0], 404)
        self.assertEqual(self.api.get("/v1/jobs/ghost")[0], 404)
        self.assertEqual(self.api.get("/v1/jobs/..%2f..%2fetc/tasks/x/explain")[0], 404)
        # the process survived all of it
        self.assertTrue(self.coordinator.alive())
        self.assertEqual(self.api.get("/v1/health", token=None)[0], 200)

    def test_oversized_json_bodies_are_rejected(self):
        try:
            st, resp = self.api.request("POST", "/v1/beans", b"{" + b" " * (6 << 20) + b"}", headers={"Content-Type": "application/json"})
            self.assertEqual(st, 413)
        except (ConnectionError, OSError):
            pass  # the server may close the socket while the client is still sending the rejected body
        self.assertTrue(self.coordinator.alive())

    def test_input_upload_verifies_digest_and_size(self):
        data = b"hello input\n"
        self.assertEqual(self.api.put("/v1/inputs/" + sha(data), data)[0], 201)
        self.assertEqual(self.api.put("/v1/inputs/" + sha(data), data)[0], 200)  # idempotent
        st, resp = self.api.put("/v1/inputs/" + sha(b"other"), data)
        self.assertEqual((st, resp["error"]["code"]), (400, "digest_mismatch"))
        self.assertEqual(self.api.put("/v1/inputs/not-a-digest", data)[0], 400)
        self.assertEqual(self.api.put("/v1/inputs/" + "A" * 64, data)[0], 400)
        self.assertEqual(self.blob_files("inputs"), [sha(data)])

    def test_unauthorized_upload_body_is_not_stored(self):
        data = b"x" * 100000
        st, _ = self.api.put("/v1/inputs/" + sha(data), data, token="bad")
        self.assertEqual(st, 401)
        self.assertEqual(self.blob_files("inputs"), [])

    def test_nonloopback_plaintext_listen_needs_an_explicit_override(self):
        r = self.run_cli(["serve", "--state", self.state, "--listen", "0.0.0.0:%d" % free_port()], timeout=15)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("plaintext", r.stderr)
        self.assertIn("--allow-plaintext-nonloopback", r.stderr)
        r = self.run_cli(["join", "--server", "http://192.0.2.1:7077", "--token-file", self.token_file, "--work-dir", os.path.join(self.tmp, "w")], timeout=15)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("--allow-plaintext-remote", r.stderr)
        r = self.run_cli(["serve", "--state", self.state, "--listen", "127.0.0.1:%d" % free_port(), "--bogus"], timeout=15)
        self.assertEqual(r.returncode, 2)


class FencedPublication(ProtocolBase):
    def run_to_claim(self, bean="fw1"):
        self.register(bean)
        self.submit(producer_consumer(), {"in.txt": b"abc def\n"})
        c = self.claim(bean)
        self.assertEqual((c["job_id"], c["task_id"], c["attempt"]), ("job1", "produce", 1))
        self.assertEqual(c["inputs"][0]["sha256"], sha(b"abc def\n"))
        return c

    def test_claim_input_download_is_lease_scoped_and_verified(self):
        c = self.run_to_claim()
        lease = c["lease_id"]
        st, data, hdr = self.api.get("/v1/leases/%s/inputs/0" % lease, headers={"X-Coffee-Bean": "fw1"}, raw=True)
        self.assertEqual((st, data), (200, b"abc def\n"))
        self.assertEqual(hdr["X-Coffee-Sha256"], sha(b"abc def\n"))
        self.assertEqual(self.api.get("/v1/leases/%s/inputs/5" % lease, headers={"X-Coffee-Bean": "fw1"})[0], 404)
        self.register("other")
        self.assertEqual(self.api.get("/v1/leases/%s/inputs/0" % lease, headers={"X-Coffee-Bean": "other"})[0], 403)
        self.assertEqual(self.api.get("/v1/leases/lease-bogus/inputs/0", headers={"X-Coffee-Bean": "fw1"})[0], 404)

    def test_successful_publication_is_accepted_and_served_back(self):
        c = self.run_to_claim()
        lease = c["lease_id"]
        out = b"ABC DEF\n"
        self.assertEqual(self.upload(lease, "fw1", out)[0], 201)
        # nothing is accepted yet: no immutable output blob before the fenced completion
        self.assertEqual(self.blob_files("outputs"), [])
        st, body = self.complete(lease, "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual((st, body["task_state"]), (200, "succeeded"), body)
        self.assertEqual(self.blob_files("outputs"), [sha(out)])
        self.assertEqual(self.accepted_output("job1", "produce", "up.txt"), out)
        # the dependent now claims the producer's accepted blob as a verified input
        c2 = self.claim("fw1")
        self.assertEqual(c2["task_id"], "consume")
        self.assertEqual(c2["inputs"][0]["sha256"], sha(out))
        st, data, _ = self.api.get("/v1/leases/%s/inputs/0" % c2["lease_id"], headers={"X-Coffee-Bean": "fw1"}, raw=True)
        self.assertEqual(data, out)

    def test_complete_validates_outputs_against_declared_and_staged(self):
        c = self.run_to_claim()
        lease, out = c["lease_id"], b"data"
        # nothing uploaded
        st, body = self.complete(lease, "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual((st, body["error"]["code"]), (409, "missing_upload"))
        self.upload(lease, "fw1", out)
        for bad in ([], [self.out_entry("other.txt", out)], [self.out_entry("../up.txt", out)], [self.out_entry("/etc/passwd", out)],
                    [self.out_entry("up.txt", out), self.out_entry("up.txt", out)],
                    [{"path": "up.txt", "size": 99, "sha256": sha(out)}], [{"path": "up.txt", "size": 4, "sha256": "zz"}]):
            st, body = self.complete(lease, "fw1", bad)
            self.assertIn(st, (400, 409), (bad, body))
            self.assertEqual(self.blob_files("outputs"), [], bad)
        self.assertEqual(self.task("job1", "produce")["state"], "running")
        self.assertEqual(self.complete(lease, "fw1", [self.out_entry("up.txt", out)])[0], 200)

    def test_cancel_during_transfer_prevents_acceptance(self):
        c = self.run_to_claim()
        lease, out = c["lease_id"], b"cancel-me" * 1000
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        conn.putrequest("PUT", "/v1/leases/%s/uploads/%s" % (lease, sha(out)))
        for k, v in {"Authorization": "Bearer " + self.token, "X-Coffee-Bean": "fw1", "Content-Length": str(len(out)),
                     "Content-Type": "application/octet-stream"}.items():
            conn.putheader(k, v)
        conn.endheaders()
        conn.send(out[:4000])  # half-way through the transfer ...
        time.sleep(0.2)
        st, _ = self.api.post("/v1/jobs/job1/cancel", {"reason": "owner cancel"})  # ... the owner cancels
        self.assertEqual(st, 200)
        conn.send(out[4000:])
        resp = conn.getresponse()
        resp.read()
        self.assertEqual(resp.status, 409)
        conn.close()
        st, body = self.complete(lease, "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual((st, body["error"]["code"]), (409, "cancelled"))
        self.assertEqual(self.blob_files("outputs"), [])
        self.assertEqual(self.task("job1", "produce")["state"], "cancelled")
        self.assertIsNone(self.task("job1", "produce")["result"])

    def test_interrupted_upload_never_becomes_a_result(self):
        c = self.run_to_claim()
        lease, out = c["lease_id"], b"partial-transfer" * 500
        s = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        s.sendall(("PUT /v1/leases/%s/uploads/%s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nX-Coffee-Bean: fw1\r\n"
                   "Content-Length: %d\r\n\r\n" % (lease, sha(out), self.token, len(out))).encode() + out[:1500])
        time.sleep(0.2)
        s.close()  # connection drops mid-transfer
        time.sleep(0.5)
        st, body = self.complete(lease, "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual((st, body["error"]["code"]), (409, "missing_upload"))
        self.assertEqual(self.blob_files("outputs"), [])
        # a clean retry of the same upload then succeeds
        self.assertEqual(self.upload(lease, "fw1", out)[0], 201)
        self.assertEqual(self.complete(lease, "fw1", [self.out_entry("up.txt", out)])[0], 200)

    def test_stale_attempt_cannot_replace_accepted_output(self):
        c1 = self.run_to_claim("fw1")
        self.register("fw2")
        time.sleep(self.LEASE_MS / 1000.0 + 0.8)  # fw1 never renews: its lease expires
        c2 = self.claim("fw2")
        self.assertEqual((c2["task_id"], c2["attempt"]), ("produce", 2))
        good, stale = b"GOOD\n", b"STALE\n"
        self.upload(c2["lease_id"], "fw2", good)
        self.assertEqual(self.complete(c2["lease_id"], "fw2", [self.out_entry("up.txt", good)])[0], 200)
        # the old attempt now tries to publish
        st, _ = self.upload(c1["lease_id"], "fw1", stale)
        self.assertEqual(st, 409)
        st, body = self.complete(c1["lease_id"], "fw1", [self.out_entry("up.txt", stale)])
        self.assertEqual(st, 409)
        self.assertIn(body["error"]["code"], ("expired", "not_running"))
        self.assertEqual(self.accepted_output("job1", "produce", "up.txt"), good)
        self.assertEqual(self.blob_files("outputs"), [sha(good)])
        t = self.task("job1", "produce")
        self.assertEqual([a["state"] for a in t["attempts"]], ["expired", "succeeded"])
        self.assertEqual([a["bean_id"] for a in t["attempts"]], ["fw1", "fw2"])
        # renew of a dead lease is also refused
        st, body = self.api.post("/v1/leases/%s/renew" % c1["lease_id"], {"bean_id": "fw1"})
        self.assertEqual(st, 409)

    def test_expiry_during_transfer_prevents_acceptance(self):
        c = self.run_to_claim()
        lease, out = c["lease_id"], b"slow"
        self.upload(lease, "fw1", out)
        time.sleep(self.LEASE_MS / 1000.0 + 0.8)  # staged but the lease lapses before completion
        st, body = self.complete(lease, "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual((st, body["error"]["code"]), (409, "expired"))
        self.assertEqual(self.blob_files("outputs"), [])
        self.assertIsNone(self.task("job1", "produce")["result"])

    def test_renew_keeps_a_lease_alive_and_wrong_bean_is_refused(self):
        c = self.run_to_claim()
        lease = c["lease_id"]
        for _ in range(4):
            time.sleep(self.LEASE_MS / 3000.0 + 0.2)
            st, body = self.api.post("/v1/leases/%s/renew" % lease, {"bean_id": "fw1"})
            self.assertEqual(st, 200, body)
        self.register("fw2")
        st, body = self.api.post("/v1/leases/%s/renew" % lease, {"bean_id": "fw2"})
        self.assertEqual((st, body["error"]["code"]), (403, "wrong_bean"))
        out = b"ok"
        self.upload(lease, "fw1", out)
        self.assertEqual(self.complete(lease, "fw2", [self.out_entry("up.txt", out)])[0], 403)
        self.assertEqual(self.complete(lease, "fw1", [self.out_entry("up.txt", out)])[0], 200)

    def test_corrupted_dependency_blob_is_never_served(self):
        c = self.run_to_claim()
        out = b"UPPER\n"
        self.upload(c["lease_id"], "fw1", out)
        self.complete(c["lease_id"], "fw1", [self.out_entry("up.txt", out)])
        c2 = self.claim("fw1")
        blob = os.path.join(self.state, "artifacts", "outputs", sha(out))
        os.chmod(blob, 0o600)
        with open(blob, "wb") as f:
            f.write(b"TAMPER\n")
        st, body, _ = self.api.get("/v1/leases/%s/inputs/0" % c2["lease_id"], headers={"X-Coffee-Bean": "fw1"}, raw=True)
        self.assertEqual(st, 500)
        self.assertIn(b"artifact_corrupt", body)
        self.assertNotIn(b"TAMPER", body)

    def test_damaged_existing_input_blob_is_repaired_by_a_valid_upload(self):
        data = b"input-bytes"
        self.assertEqual(self.api.put("/v1/inputs/" + sha(data), data)[0], 201)
        blob = os.path.join(self.state, "artifacts", "inputs", sha(data))
        os.chmod(blob, 0o600)
        with open(blob, "wb") as f:
            f.write(b"INPUT-BYTES")  # same length, different content
        st, body = self.api.put("/v1/inputs/" + sha(data), data)
        self.assertEqual((st, body.get("repaired")), (200, True), body)
        with open(blob, "rb") as f:
            self.assertEqual(f.read(), data)
        self.assertEqual(self.blob_files("inputs"), [sha(data)])  # no repair leftovers

    def test_submit_requires_uploaded_inputs_and_idempotent_resubmit(self):
        wlj = producer_consumer()
        st, body = self.api.post("/v1/jobs", {"job_id": "j", "workload": wlj, "files": {"in.txt": {"sha256": sha(b"zz"), "size": 2}}})
        self.assertEqual((st, body["error"]["code"]), (409, "missing_input"))
        st, body = self.api.post("/v1/jobs", {"job_id": "j", "workload": wlj, "files": {}})
        self.assertEqual((st, body["error"]["code"]), (400, "invalid_argument"))  # source without a digest entry
        self.assertEqual(self.api.get("/v1/jobs/j")[0], 404)  # failed submits left nothing behind
        self.assertEqual(self.submit(wlj, {"in.txt": b"zz"}, "j")["created"], True)
        self.assertEqual(self.api.post("/v1/jobs", {"job_id": "j", "workload": wlj, "files": {"in.txt": {"sha256": sha(b"zz"), "size": 2}}})[0], 200)
        st, body = self.api.post("/v1/jobs", {"job_id": "j", "workload": wl([{"id": "x", "command": ["true"]}])})
        self.assertEqual((st, body["error"]["code"]), (409, "conflict"))


class RestartDurability(ProtocolBase):
    def test_coordinator_restart_keeps_results_history_and_fences_stale_leases(self):
        self.register("fw1")
        self.submit(producer_consumer(), {"in.txt": b"one two\n"})
        c = self.claim("fw1")
        out = b"ONE TWO\n"
        self.upload(c["lease_id"], "fw1", out)
        self.assertEqual(self.complete(c["lease_id"], "fw1", [self.out_entry("up.txt", out)])[0], 200)
        c2 = self.claim("fw1")  # consumer running when the coordinator dies
        self.coordinator.kill9()
        self.start_coordinator()
        # accepted result and attempt history survived
        self.assertEqual(self.accepted_output("job1", "produce", "up.txt"), out)
        t = self.task("job1", "produce")
        self.assertEqual((t["state"], t["attempts"][0]["bean_id"], t["accepted_attempt"]), ("succeeded", "fw1", 1))
        # the running lease is still valid within its original expiry and the Bean may finish it
        n = b"8\n"
        self.assertEqual(self.api.post("/v1/leases/%s/renew" % c2["lease_id"], {"bean_id": "fw1"})[0], 200)
        self.upload(c2["lease_id"], "fw1", n)
        self.assertEqual(self.complete(c2["lease_id"], "fw1", [self.out_entry("n.txt", n)])[0], 200)
        self.assertEqual(self.wait_job("job1")["state"], "succeeded")
        # a completed attempt's lease cannot be replayed after another restart
        self.coordinator.kill9()
        self.start_coordinator()
        st, body = self.complete(c2["lease_id"], "fw1", [self.out_entry("n.txt", n)])
        self.assertEqual(st, 409)
        self.assertEqual(self.accepted_output("job1", "consume", "n.txt"), n)

    def test_outage_longer_than_the_lease_loses_the_lease_after_restart(self):
        self.register("fw1")
        self.submit(producer_consumer(), {"in.txt": b"x"})
        c = self.claim("fw1")
        self.coordinator.kill9()
        time.sleep(self.LEASE_MS / 1000.0 + 0.8)  # the outage outlasts the lease; restart grants no grace
        self.start_coordinator()
        out = b"X"
        self.upload(c["lease_id"], "fw1", out)  # may be refused
        st, body = self.complete(c["lease_id"], "fw1", [self.out_entry("up.txt", out)])
        self.assertEqual(st, 409, body)
        self.assertEqual(self.blob_files("outputs"), [])
        t = self.task("job1", "produce")
        self.assertEqual((t["state"], t["attempts"][0]["state"]), ("pending", "expired"))

    def test_cancellation_survives_restart_and_still_fences(self):
        self.register("fw1")
        self.submit(producer_consumer(), {"in.txt": b"x"})
        c = self.claim("fw1")
        self.assertEqual(self.api.post("/v1/jobs/job1/cancel", {})[0], 200)
        self.coordinator.kill9()
        self.start_coordinator()
        st, body = self.complete(c["lease_id"], "fw1", [self.out_entry("up.txt", b"x")])
        self.assertEqual(st, 409)
        self.assertEqual(self.job("job1")["state"], "cancelled")


class TemporaryLifecycle(ProtocolBase):
    run_to_claim = FencedPublication.run_to_claim
    def scope(self, lease):
        return os.path.join(self.state, "artifacts", "tmp", lease)

    def absent(self, path):
        wait_until(lambda: not os.path.exists(path), 6, what="temporary scope reclamation")

    def test_input_scopes_success_reuse_reject_and_disconnect(self):
        data = b"clean input"
        for digest, expected in [(sha(data), 201), (sha(data), 200), (sha(b"bad"), 400)]:
            self.assertEqual(self.api.put("/v1/inputs/" + digest, data)[0], expected)
            self.assertEqual(os.listdir(os.path.join(self.state, "artifacts", "tmp")), [])
        sock = self.partial_put("/v1/inputs/" + sha(data), data[:2], len(data))
        sock.close()
        wait_until(lambda: not os.listdir(os.path.join(self.state, "artifacts", "tmp")), 6)
        self.assertEqual(self.blob_files("inputs"), [sha(data)])

    def partial_put(self, path, prefix, size, bean=None):
        sock = socket.create_connection(("127.0.0.1", self.port))
        hdr = "PUT %s HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n" % (path, self.token, size)
        if bean:
            hdr += "X-Coffee-Bean: %s\r\n" % bean
        sock.sendall(hdr.encode() + b"\r\n" + prefix)
        return sock

    def test_cancelled_stage_reclaimed_without_stale_request(self):
        c = self.run_to_claim(); lease = c["lease_id"]; out = b"x" * 1000000
        self.assertEqual(self.upload(lease, "fw1", out)[0], 201)
        self.assertTrue(os.path.isdir(self.scope(lease)))
        self.assertEqual(self.api.post("/v1/jobs/job1/cancel")[0], 200)
        self.absent(self.scope(lease))
        self.coordinator.kill9(); self.start_coordinator()
        self.assertFalse(os.path.exists(self.scope(lease)))
        self.assertEqual(self.blob_files("outputs"), [])

    def test_expired_stage_reclaimed_without_stale_request(self):
        c = self.run_to_claim(); lease = c["lease_id"]
        self.assertEqual(self.upload(lease, "fw1", b"x" * 1000000)[0], 201)
        self.absent(self.scope(lease))
        c2 = self.claim("fw1")
        self.assertEqual(c2["attempt"], 2)
        self.assertEqual(self.blob_files("outputs"), [])

    def test_restart_keeps_valid_stage_and_removes_unknown_and_terminal(self):
        c = self.run_to_claim(); lease = c["lease_id"]; out = b"valid"
        self.assertEqual(self.upload(lease, "fw1", out)[0], 201)
        self.coordinator.kill9()
        unknown = self.scope("unknown")
        os.makedirs(unknown)
        with open(os.path.join(unknown, "orphan.staged"), "wb") as f: f.write(b"garbage")
        self.start_coordinator()
        self.absent(unknown)
        self.assertTrue(os.path.exists(self.scope(lease)))
        self.assertEqual(self.complete(lease, "fw1", [self.out_entry("up.txt", out)])[0], 200)
        self.absent(self.scope(lease))
        self.coordinator.kill9()
        # Simulate temporary debris left behind a terminal lease by an interrupted cleanup.
        os.makedirs(self.scope(lease))
        with open(os.path.join(self.scope(lease), "leftover.staged"), "wb") as f: f.write(out)
        self.start_coordinator()
        self.absent(self.scope(lease))
        self.assertEqual(self.accepted_output("job1", "produce", "up.txt"), out)

    def test_cancel_during_upload_does_not_delete_active_file_or_accept(self):
        c = self.run_to_claim(); lease = c["lease_id"]; data = b"inflight-output"
        sock = self.partial_put("/v1/leases/%s/uploads/%s" % (lease, sha(data)), data[:2], len(data), "fw1")
        self.addCleanup(sock.close)
        wait_until(lambda: os.path.isdir(self.scope(lease)), 2)
        self.assertEqual(self.api.post("/v1/jobs/job1/cancel")[0], 200)
        time.sleep(0.7)
        self.assertTrue(os.path.isdir(self.scope(lease)))
        sock.sendall(data[2:])
        self.assertIn(b"409", sock.recv(4096))
        sock.close()
        self.absent(self.scope(lease))
        self.assertEqual(self.blob_files("outputs"), [])


    def test_expiry_during_upload_skips_active_scope_then_reclaims(self):
        c = self.run_to_claim(); lease = c["lease_id"]; data = b"expiry-output"
        sock = self.partial_put("/v1/leases/%s/uploads/%s" % (lease, sha(data)), data[:2], len(data), "fw1")
        self.addCleanup(sock.close)
        wait_until(lambda: os.path.isdir(self.scope(lease)), 2)
        time.sleep(self.LEASE_MS / 1000.0 + 0.6)
        self.assertTrue(os.path.isdir(self.scope(lease)))
        sock.sendall(data[2:])
        self.assertIn(b"409", sock.recv(4096))
        sock.close()
        self.absent(self.scope(lease))
        self.assertEqual(self.blob_files("outputs"), [])

    def test_abandoned_stage_and_restart_expired_stage_are_reclaimed(self):
        c = self.run_to_claim(); lease = c["lease_id"]
        self.assertEqual(self.upload(lease, "fw1", b"abandoned")[0], 201)
        self.register("fw1")  # process identity reused: old attempt abandoned
        self.absent(self.scope(lease))
        c2 = self.claim("fw1"); lease2 = c2["lease_id"]
        self.assertEqual(self.upload(lease2, "fw1", b"expired while stopped")[0], 201)
        self.coordinator.kill9()
        time.sleep(self.LEASE_MS / 1000.0 + 0.2)
        self.start_coordinator()
        self.absent(self.scope(lease2))
        self.assertEqual(self.complete(lease2, "fw1", [self.out_entry("up.txt", b"expired while stopped")])[0], 409)
        self.assertEqual(self.blob_files("outputs"), [])

    def test_concurrent_inputs_leave_only_immutable_blobs(self):
        import concurrent.futures
        payloads = [b"parallel-" + str(i % 4).encode() for i in range(24)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda d: self.api.put("/v1/inputs/" + sha(d), d)[0], payloads))
        self.assertTrue(all(st in (200, 201) for st in results), results)
        self.assertEqual(os.listdir(os.path.join(self.state, "artifacts", "tmp")), [])
        self.assertEqual(self.blob_files("inputs"), sorted({sha(d) for d in payloads}))
        for d in payloads:
            path = os.path.join(self.state, "artifacts", "inputs", sha(d))
            with open(path, "rb") as f: self.assertEqual(f.read(), d)
            self.assertFalse(os.stat(path).st_mode & 0o222)


class SmallInputTemporaryLifecycle(ProtocolBase):
    SERVE_EXTRA = ("--max-artifact-bytes", "8")

    def test_oversize_input_removes_scope(self):
        st, body = self.api.put("/v1/inputs/" + sha(b"123456789"), b"123456789")
        self.assertEqual(st, 413, body)
        wait_until(lambda: not os.listdir(os.path.join(self.state, "artifacts", "tmp")), 6)
        self.assertEqual(self.blob_files("inputs"), [])


class CliBasics(ProtocolBase):
    def test_cli_failures_have_distinct_exit_codes(self):
        r = self.run_cli(["beans", "--server", "http://127.0.0.1:%d" % free_port(), "--token-file", self.token_file], timeout=30)
        self.assertEqual(r.returncode, 4, r.stderr)  # coordinator unreachable
        bad = os.path.join(self.tmp, "badtoken")
        with open(bad, "w") as f:
            f.write("0" * 64)
        r = self.run_cli(["beans", "--server", self.url, "--token-file", bad])
        self.assertEqual(r.returncode, 4, r.stderr)
        self.assertIn("unauthorized", r.stderr)
        self.assertNotIn("0" * 64, r.stderr)
        r = self.run_cli(["status", "nosuchjob", "--server", self.url, "--token-file", self.token_file])
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(self.run_cli(["beans", "--server", self.url]).returncode, 2)  # no credentials configured
        self.assertEqual(self.run_cli(["prepare"]).returncode, 3)  # still honestly unimplemented

    def test_beans_and_drain_commands(self):
        self.register("fw1", 2, 512)
        r = self.cli(["beans", "--json"])
        self.assertEqual(r.returncode, 0, r.stderr)
        beans = json.loads(r.stdout)["beans"]
        self.assertEqual((beans[0]["id"], beans[0]["cpus"], beans[0]["state"]), ("fw1", 2, "online"))
        self.assertIn("fw1", self.cli(["beans"]).stdout)
        self.assertEqual(self.cli(["drain", "fw1"]).returncode, 0)
        self.assertEqual(json.loads(self.cli(["beans", "--json"]).stdout)["beans"][0]["state"], "draining")
        self.assertEqual(self.cli(["drain", "ghost"]).returncode, 1)
        self.submit(wl([{"id": "t", "command": ["true"]}]))
        self.assertIsNone(self.claim("fw1"))  # drained Beans get nothing
        self.assertEqual(self.cli(["drain", "fw1", "--undo"]).returncode, 0)
        self.assertIsNotNone(self.claim("fw1"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
