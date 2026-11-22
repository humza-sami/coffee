"""Install / collision / uninstall tests: real `cmake --install` into temporary prefixes (never a system path)."""
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.abspath(os.environ.get("COFFEE_BUILD_DIR") or os.path.join(ROOT, "build"))
CMAKE = os.environ.get("COFFEE_CMAKE") or shutil.which("cmake") or os.path.join(ROOT, ".tools", "venv", "bin", "cmake")
CPACK = os.path.join(os.path.dirname(CMAKE), "cpack")
VERSION = "0.1.0-beta.1"
UNINSTALL = os.path.join(ROOT, "scripts", "uninstall.sh")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def tree(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root)] = os.readlink(p) if os.path.islink(p) else (None if os.path.isdir(p) else sha256(p))
    return out


def install(prefix, check=False):
    r = subprocess.run([CMAKE, "--install", BUILD, "--prefix", prefix], capture_output=True, text=True, timeout=120)
    if check and r.returncode != 0:
        raise AssertionError("install failed: " + r.stdout + r.stderr)
    return r


def read_manifest(prefix):
    entries = {}
    with open(os.path.join(prefix, "share", "coffee", "install-manifest.txt")) as f:
        lines = f.read().splitlines()
    assert lines[0] == "# coffee install manifest v1", lines[0]
    for line in lines:
        if line and not line.startswith("#"):
            digest, rel = line.split("  ", 1)
            entries[rel] = digest
    return entries, lines


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coffee-test-install-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.prefix = os.path.join(self.tmp, "prefix")

    def test_version_is_the_documented_prerelease(self):
        out = subprocess.run([os.path.join(BUILD, "coffee"), "--version"], capture_output=True, text=True).stdout
        self.assertEqual(out.split()[1], VERSION, out)
        self.assertTrue(os.path.exists(os.path.join(ROOT, "docs", "releases", "v%s.md" % VERSION)))

    def test_install_into_temporary_prefix_runs_and_records_manifest(self):
        install(self.prefix, check=True)
        exe = os.path.join(self.prefix, "bin", "coffee")
        self.assertTrue(os.access(exe, os.X_OK))
        out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
        self.assertIn("coffee " + VERSION, out)
        entries, lines = read_manifest(self.prefix)
        for needed in ("bin/coffee", "share/coffee/uninstall.sh", "share/doc/coffee/LICENSE", "share/doc/coffee/NOTICE", "share/doc/coffee/README.md",
                       "share/doc/coffee/licenses/cpp-httplib.LICENSE", "share/coffee/examples/pipeline/coffee.yaml",
                       "share/coffee/systemd-user/coffee-coordinator.service"):
            self.assertIn(needed, entries)
        for rel, digest in entries.items():
            self.assertEqual(sha256(os.path.join(self.prefix, rel)), digest, rel)
        # nothing outside the manifest except the manifest itself
        actual = {k for k, v in tree(self.prefix).items() if v is not None}
        self.assertEqual(actual, set(entries) | {"share/coffee/install-manifest.txt"})
        # installed examples validate with the installed binary
        r = subprocess.run([exe, "validate", os.path.join(self.prefix, "share/coffee/examples/pipeline/coffee.yaml")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_existing_foreign_coffee_is_never_overwritten(self):
        os.makedirs(os.path.join(self.prefix, "bin"))
        sentinel = os.path.join(self.prefix, "bin", "coffee")
        with open(sentinel, "w") as f:
            f.write("#!/bin/sh\necho SENTINEL-NOT-COFFEE\n")
        os.chmod(sentinel, 0o750)
        before, mode = tree(self.prefix), os.stat(sentinel).st_mode
        r = install(self.prefix)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refusing to install", r.stdout + r.stderr)
        self.assertIn("bin/coffee", r.stdout + r.stderr)
        self.assertEqual(tree(self.prefix), before)  # nothing written at all, sentinel byte-identical
        self.assertEqual(os.stat(sentinel).st_mode, mode)
        self.assertEqual(subprocess.run([sentinel], capture_output=True, text=True).stdout.strip(), "SENTINEL-NOT-COFFEE")

    def test_dangling_or_foreign_symlink_is_a_collision_too(self):
        os.makedirs(os.path.join(self.prefix, "bin"))
        target = os.path.join(self.tmp, "elsewhere")
        with open(target, "w") as f:
            f.write("keep")
        os.symlink(target, os.path.join(self.prefix, "bin", "coffee"))
        r = install(self.prefix)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(read(target), "keep")
        self.assertTrue(os.path.islink(os.path.join(self.prefix, "bin", "coffee")))

    def test_foreign_doc_file_collision_also_refuses_before_writing(self):
        os.makedirs(os.path.join(self.prefix, "share", "doc", "coffee"))
        with open(os.path.join(self.prefix, "share", "doc", "coffee", "LICENSE"), "w") as f:
            f.write("not ours")
        before = tree(self.prefix)
        r = install(self.prefix)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(tree(self.prefix), before)

    def test_reinstall_of_unmodified_install_succeeds_but_modified_binary_is_protected(self):
        install(self.prefix, check=True)
        install(self.prefix, check=True)  # idempotent upgrade of our own unmodified files
        exe = os.path.join(self.prefix, "bin", "coffee")
        os.chmod(exe, 0o755)
        with open(exe, "ab") as f:
            f.write(b"local-edit")
        r = install(self.prefix)
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue(read(exe, "rb").endswith(b"local-edit"))

    def test_uninstall_follows_manifest_and_preserves_state_and_user_files(self):
        install(self.prefix, check=True)
        exe = os.path.join(self.prefix, "bin", "coffee")
        state = os.path.join(self.tmp, "state")
        self.assertEqual(subprocess.run([exe, "init", "--state", state], capture_output=True).returncode, 0)
        results = os.path.join(self.tmp, "results")
        os.makedirs(results)
        with open(os.path.join(results, "out.txt"), "w") as f:
            f.write("precious artifact\n")
        note = os.path.join(self.prefix, "share", "coffee", "user-note.txt")
        with open(note, "w") as f:
            f.write("mine")
        edited = os.path.join(self.prefix, "share", "doc", "coffee", "NOTICE")
        os.chmod(edited, 0o644)
        with open(edited, "a") as f:
            f.write("\nuser edit\n")
        entries, _ = read_manifest(self.prefix)
        state_before, results_before = tree(state), tree(results)

        dry = subprocess.run(["sh", UNINSTALL, "--prefix", self.prefix, "--dry-run"], capture_output=True, text=True)
        self.assertEqual(dry.returncode, 0, dry.stderr)
        self.assertTrue(os.path.exists(exe))
        self.assertIn("bin/coffee", dry.stdout)

        r = subprocess.run(["sh", os.path.join(self.prefix, "share", "coffee", "uninstall.sh"), "--prefix", self.prefix], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(exe))
        self.assertFalse(os.path.exists(os.path.join(self.prefix, "share", "coffee", "install-manifest.txt")))
        remaining = {k for k, v in tree(self.prefix).items() if v is not None}
        self.assertEqual(remaining, {"share/coffee/user-note.txt", "share/doc/coffee/NOTICE"})
        self.assertIn("modified", r.stdout + r.stderr)
        self.assertEqual(tree(state), state_before)
        self.assertEqual(tree(results), results_before)
        self.assertEqual(read(note), "mine")
        self.assertIn("user edit", read(edited))
        self.assertTrue(len(entries) > 10)

    def test_uninstall_refuses_unsafe_manifest_and_missing_manifest(self):
        r = subprocess.run(["sh", UNINSTALL, "--prefix", self.prefix], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        victim = os.path.join(self.tmp, "victim.txt")
        with open(victim, "w") as f:
            f.write("outside")
        os.makedirs(os.path.join(self.prefix, "share", "coffee"))
        with open(os.path.join(self.prefix, "share", "coffee", "install-manifest.txt"), "w") as f:
            f.write("# coffee install manifest v1\n%s  ../victim.txt\n%s  /etc/hostname\n" % (sha256(victim), "0" * 64))
        r = subprocess.run(["sh", UNINSTALL, "--prefix", self.prefix], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(read(victim), "outside")
        self.assertTrue(os.path.exists("/etc/hostname"))

    def test_cpack_tarball_contains_the_install_set(self):
        out = os.path.join(self.tmp, "pack")
        r = subprocess.run([CPACK, "-G", "TGZ", "-B", out, "--config", os.path.join(BUILD, "CPackConfig.cmake")], capture_output=True, text=True, cwd=BUILD, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        tarballs = [n for n in os.listdir(out) if n.endswith(".tar.gz")]
        self.assertEqual(len(tarballs), 1, os.listdir(out))
        self.assertIn(VERSION, tarballs[0])
        names = subprocess.run(["tar", "tzf", os.path.join(out, tarballs[0])], capture_output=True, text=True).stdout
        self.assertIn("bin/coffee", names)
        self.assertIn("share/doc/coffee/LICENSE", names)


if __name__ == "__main__":
    unittest.main(verbosity=2)
