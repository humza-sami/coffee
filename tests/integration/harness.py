"""Real-process test harness for Coffee: starts `coffee serve` / `coffee join` as child processes on
loopback ephemeral ports with temporary state, talks to the coordinator over real HTTP, and guarantees
cleanup of every process it started (and any stray descendants it can find)."""
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

COFFEE = os.environ.get("COFFEE_BIN") or os.path.join(os.path.dirname(__file__), "..", "..", "build", "coffee")
COFFEE = os.path.abspath(COFFEE)
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:  # zombies count as stopped
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def wait_until(pred, timeout=15.0, interval=0.05, what="condition"):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(interval)
    raise AssertionError("timed out waiting for " + what)


def read_pid_file(path, timeout=10.0):
    def r():
        try:
            with open(path) as f:
                t = f.read().strip()
            return int(t) if t else None
        except (OSError, ValueError):
            return None
    return wait_until(r, timeout, what="pid file " + path)


class Api:
    """Minimal raw HTTP client (no retries) so tests control exact bytes and timing."""

    def __init__(self, port, token):
        self.port, self.token = port, token

    def request(self, method, path, body=None, token="default", headers=None, raw=False):
        h = dict(headers or {})
        tok = self.token if token == "default" else token
        if tok is not None:
            h["Authorization"] = "Bearer " + tok
        data = None
        if body is not None:
            if isinstance(body, (bytes, bytearray)):
                data = bytes(body)
                h.setdefault("Content-Type", "application/octet-stream")
            elif isinstance(body, str):
                data = body.encode()
                h.setdefault("Content-Type", "application/json")
            else:
                data = json.dumps(body).encode()
                h["Content-Type"] = "application/json"
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            c.request(method, path, body=data, headers=h)
            r = c.getresponse()
            payload = r.read()
            if raw:
                return r.status, payload, dict(r.getheaders())
            try:
                return r.status, json.loads(payload) if payload else None
            except ValueError:
                return r.status, payload
        finally:
            c.close()

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body if body is not None else {}, **kw)

    def put(self, path, body, **kw):
        return self.request("PUT", path, body, **kw)


class Proc:
    def __init__(self, args, log_path, env=None):
        self.args = args
        self.log = open(log_path, "ab")
        e = dict(os.environ)
        e.update(env or {})
        self.p = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=e, start_new_session=True)

    @property
    def pid(self):
        return self.p.pid

    def kill9(self):
        if self.p.poll() is None:
            os.kill(self.p.pid, signal.SIGKILL)
        self.p.wait()

    def stop(self, timeout=8):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.log.close()

    def alive(self):
        return self.p.poll() is None


class CoffeeTestCase(unittest.TestCase):
    LEASE_MS = 3000
    HEARTBEAT_TIMEOUT_MS = 3000
    SERVE_EXTRA = ()

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coffee-it-")
        self.procs = []
        self.extra_pids = []
        self.addCleanup(self.cleanup)  # runs even if the rest of setUp fails
        self.state = os.path.join(self.tmp, "state")
        self.run_cli(["init", "--state", self.state]).check_returncode()
        self.token_file = os.path.join(self.state, "token")
        with open(self.token_file) as f:
            self.token = f.read().strip()
        self.port = free_port()
        self.url = "http://127.0.0.1:%d" % self.port
        self.api = Api(self.port, self.token)
        self.coordinator = None

    def cleanup(self):
        for p in reversed(self.procs):
            try:
                p.stop()
            except Exception:
                pass
        for pid in self.extra_pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- process helpers
    def run_cli(self, args, timeout=60, env=None, check_server=True):
        e = dict(os.environ)
        e.update(env or {})
        return subprocess.run([COFFEE] + args, capture_output=True, text=True, timeout=timeout, env=e, stdin=subprocess.DEVNULL)

    def cli(self, args, **kw):
        """CLI call against this test's coordinator."""
        return self.run_cli(args + ["--server", self.url, "--token-file", self.token_file], **kw)

    def start_coordinator(self, extra=(), env=None):
        self.ready_seq = getattr(self, "ready_seq", 0) + 1
        ready = os.path.join(self.tmp, "ready-%d.json" % self.ready_seq)
        args = [COFFEE, "serve", "--state", self.state, "--listen", "127.0.0.1:%d" % self.port, "--lease-ms", str(self.LEASE_MS),
                "--heartbeat-timeout-ms", str(self.HEARTBEAT_TIMEOUT_MS), "--ready-file", ready] + list(self.SERVE_EXTRA) + list(extra)
        p = Proc(args, os.path.join(self.tmp, "serve.log"), env=env)
        self.procs.append(p)
        self.coordinator = p

        def up():
            if not p.alive():
                raise AssertionError("coordinator exited early (rc=%s): %s" % (p.p.returncode, self.read_log("serve.log")))
            return os.path.exists(ready)
        wait_until(up, 20, what="coordinator ready file")
        return p

    def start_worker(self, name, cpus=1, memory_mb=1024, extra=()):
        wd = os.path.join(self.tmp, "work-" + name)
        args = [COFFEE, "join", "--server", self.url, "--token-file", self.token_file, "--work-dir", wd, "--id", name, "--cpus", str(cpus),
                "--memory-mb", str(memory_mb), "--heartbeat-ms", "300", "--poll-ms", "100"] + list(extra)
        p = Proc(args, os.path.join(self.tmp, "worker-%s.log" % name))
        self.procs.append(p)
        wait_until(lambda: any(b["id"] == name for b in (self.api.get("/v1/beans")[1] or {}).get("beans", [])), 15, what="bean %s registered" % name)
        return p

    def read_log(self, name):
        try:
            with open(os.path.join(self.tmp, name), errors="replace") as f:
                return f.read()[-4000:]
        except OSError:
            return ""

    # ---- workload helpers
    def write_workload(self, name, yaml_text, files=None):
        d = os.path.join(self.tmp, "wl-" + name)
        os.makedirs(d, exist_ok=True)
        for rel, data in (files or {}).items():
            fp = os.path.join(d, rel)
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode())
        path = os.path.join(d, "coffee.yaml")
        with open(path, "w") as f:
            f.write(yaml_text)
        return path

    def job(self, job_id):
        st, body = self.api.get("/v1/jobs/" + job_id)
        self.assertEqual(st, 200, body)
        return body

    def task(self, job_id, task_id):
        for t in self.job(job_id)["tasks"]:
            if t["id"] == task_id:
                return t
        raise AssertionError("no task " + task_id)

    def wait_job(self, job_id, states=("succeeded", "failed", "cancelled"), timeout=40):
        def done():
            j = self.job(job_id)
            return j if j["state"] in states else None
        return wait_until(done, timeout, 0.1, what="job %s in %s" % (job_id, states))

    def wait_task(self, job_id, task_id, states, timeout=30):
        def done():
            t = self.task(job_id, task_id)
            return t if t["state"] in states else None
        return wait_until(done, timeout, 0.1, what="task %s/%s in %s" % (job_id, task_id, states))

    def accepted_output(self, job_id, task_id, path):
        st, data, hdr = self.api.get("/v1/jobs/%s/tasks/%s/outputs?path=%s" % (job_id, task_id, path), raw=True)
        self.assertEqual(st, 200, data)
        return data

    def blob_files(self, kind):
        d = os.path.join(self.state, "artifacts", kind)
        return sorted(os.listdir(d)) if os.path.isdir(d) else []


def build_fault_preload(dest_dir):
    """Compile tests/integration/fixtures/fault_fsync.c into dest_dir and return the .so path."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "fault_fsync.c")
    out = os.path.join(dest_dir, "fault_fsync.so")
    subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-o", out, src], check=True)
    return out


def preload_value(so_path):
    """LD_PRELOAD value for the fault fixture. An ASan-linked coffee needs the ASan runtime first in the list."""
    try:
        linked = subprocess.run(["ldd", COFFEE], capture_output=True, text=True).stdout
    except OSError:
        linked = ""
    if "libasan" in linked:
        runtime = subprocess.run(["cc", "-print-file-name=libasan.so"], capture_output=True, text=True).stdout.strip()
        return runtime + ":" + so_path
    return so_path
