"""Migrate bundled genuine schema-v1 synthetic state; no Git history is required."""
import hashlib
from pathlib import Path
import tempfile
import json
import os
import shutil
import sqlite3
import tarfile
import unittest

from harness import CoffeeTestCase

FIX = Path(os.environ.get("COFFEE_V1_FIXTURE_DIR") or Path(__file__).parent / "fixtures" / "v1")
PAYLOADS = {"v1-state.tar", "v1-snapshot.json", "v1-version.txt"}


def verify_fixture(directory):
    """Require a complete checksum-verified fixture before using any legacy state."""
    directory = Path(directory)
    try:
        entries = {}
        for line in (directory / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split()
            if name not in PAYLOADS or name in entries or len(digest) != 64:
                raise ValueError("invalid checksum manifest")
            entries[name] = digest
        if set(entries) != PAYLOADS:
            raise ValueError("incomplete checksum manifest")
        for name, expected in entries.items():
            if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
                raise ValueError("checksum mismatch: " + name)
    except (OSError, ValueError) as exc:
        raise AssertionError(f"invalid schema-v1 fixture at {directory}: {exc}") from exc


class FixtureIntegrity(unittest.TestCase):
    def test_missing_fixture_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(AssertionError, "invalid schema-v1 fixture"):
                verify_fixture(tmp)

    def test_corrupt_or_missing_payload_is_an_error(self):
        verify_fixture(FIX)
        for name in sorted(PAYLOADS):
            for damage in ("missing", "corrupt"):
                with self.subTest(name=name, damage=damage), tempfile.TemporaryDirectory() as tmp:
                    shutil.copytree(FIX, tmp, dirs_exist_ok=True)
                    target = Path(tmp) / name
                    if damage == "missing":
                        target.unlink()
                    else:
                        target.write_bytes(b"corrupt")
                    with self.assertRaisesRegex(AssertionError, "invalid schema-v1 fixture"):
                        verify_fixture(tmp)



def user_version(db):
    con = sqlite3.connect(db)
    try:
        return con.execute("PRAGMA user_version").fetchone()[0]
    finally:
        con.close()


def read(p):
    with open(p) as f:
        return f.read()


class SchemaUpgrade(CoffeeTestCase):
    def setUp(self):
        verify_fixture(FIX)
        super().setUp()
        shutil.rmtree(self.state)  # replace the freshly initialised state with the old-version one
        with tarfile.open(os.path.join(FIX, "v1-state.tar")) as t:
            t.extractall(self.tmp, filter="data")
        os.rename(os.path.join(self.tmp, "state"), self.state)
        self.token = read(self.token_file).strip()
        self.api.token = self.token

    def test_v1_state_upgrades_and_keeps_identity_history_and_stays_usable(self):
        db = os.path.join(self.state, "coffee.db")
        snap = json.loads(read(os.path.join(FIX, "v1-snapshot.json")))
        cfg_before, token_before = read(os.path.join(self.state, "config.json")), read(self.token_file)
        self.assertEqual(user_version(db), 1)
        self.start_coordinator()
        self.assertEqual(user_version(db), 2)
        info = self.api.get("/v1/version")[1]
        self.assertEqual(info["cluster_id"], snap["cluster_id"])
        for jid in ("legacy-job", "legacy-cancelled"):
            now, old = self.job(jid), snap[jid]
            self.assertEqual(now["state"], old["state"], jid)
            self.assertEqual([(t["id"], t["state"]) for t in now["tasks"]], [(t["id"], t["state"]) for t in old["tasks"]])
            self.assertEqual([[(a["attempt"], a["bean_id"], a["state"]) for a in t.get("attempts", [])] for t in now["tasks"]],
                             [[(a["attempt"], a["bean_id"], a["state"]) for a in t.get("attempts", [])] for t in old["tasks"]])
        self.assertIn("legacy-bean", [b["id"] for b in self.api.get("/v1/beans")[1]["beans"]])
        self.assertEqual(read(os.path.join(self.state, "config.json")), cfg_before)
        self.assertEqual(read(self.token_file), token_before)
        # the upgraded state runs new work end to end and survives a restart
        self.start_worker("bean-new", cpus=1)
        wl = self.write_workload("up", """apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: up}
spec:
  tasks:
    - id: t
      command: ["sh", "-c", "echo upgraded > out.txt"]
      outputs: [out.txt]
""")
        r = self.cli(["brew", wl, "--job-id", "after-upgrade", "--wait", "--timeout", "60"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.accepted_output("after-upgrade", "t", "out.txt"), b"upgraded\n")
        self.coordinator.stop()
        self.start_coordinator()
        self.assertEqual(self.job("after-upgrade")["state"], "succeeded")
        # Legacy jobs never had their input files uploaded, so a Bean cannot run their remaining tasks: that must be a clear,
        # non-retryable failure (not a hang), and the earlier accepted result and its attempt history must be untouched.
        legacy = self.wait_job("legacy-job", timeout=30)
        tasks = {t["id"]: t for t in legacy["tasks"]}
        self.assertEqual(legacy["state"], "failed")
        self.assertEqual(tasks["count-1"]["state"], "succeeded")
        self.assertEqual(tasks["count-1"]["result"], snap["legacy-job"]["tasks"][0]["result"])
        self.assertIn("has no recorded digest", tasks["count-2"]["failure_reason"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
