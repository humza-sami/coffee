"""scripts/demo-local.sh must exit non-zero (and never print its success line) when a required step fails, and
succeed on the real binary. A tiny wrapper around `coffee` injects a failure into the first example brew."""
import os
import stat
import subprocess
import tempfile
import unittest

from harness import COFFEE, ROOT

DEMO = os.path.join(ROOT, "scripts", "demo-local.sh")


def run_demo(binary, timeout=180):
    return subprocess.run(["bash", DEMO, binary], capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)


def stray_demo_dirs():
    return {d for d in os.listdir("/tmp") if d.startswith("coffee-demo-")}


class Demo(unittest.TestCase):
    def test_injected_brew_failure_fails_the_demo(self):
        before = stray_demo_dirs()
        with tempfile.TemporaryDirectory() as d:
            wrapper = os.path.join(d, "coffee-wrapper")
            with open(wrapper, "w") as f:
                f.write('#!/bin/bash\nif [[ "$1" == brew && "$2" == */examples/coffee.yaml ]]; then echo "INJECTED BREW FAILURE" >&2; exit 1; fi\nexec "%s" "$@"\n' % COFFEE)
            os.chmod(wrapper, os.stat(wrapper).st_mode | stat.S_IXUSR)
            r = run_demo(wrapper)
        out = r.stdout + r.stderr
        self.assertNotEqual(r.returncode, 0, out)
        self.assertIn("INJECTED BREW FAILURE", out)
        self.assertIn("DEMO FAILED", r.stderr)
        self.assertNotIn("DEMO PASSED", out)
        self.assertNotIn("demo finished", out)
        self.assertEqual(stray_demo_dirs() - before, set())  # the cleanup trap still ran

    def test_wrong_output_fails_the_demo(self):
        # a coffee that "succeeds" without producing outputs must not pass the output assertions
        with tempfile.TemporaryDirectory() as d:
            wrapper = os.path.join(d, "coffee-wrapper")
            with open(wrapper, "w") as f:
                f.write('#!/bin/bash\nif [[ "$1" == brew && "$2" == */examples/coffee.yaml ]]; then mkdir -p "$(echo "$@" | sed -n "s/.*--out \\([^ ]*\\).*/\\1/p")"; exit 0; fi\nexec "%s" "$@"\n' % COFFEE)
            os.chmod(wrapper, os.stat(wrapper).st_mode | stat.S_IXUSR)
            r = run_demo(wrapper)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("DEMO PASSED", r.stdout + r.stderr)

    def test_normal_demo_passes_and_cleans_up(self):
        before = stray_demo_dirs()
        r = run_demo(COFFEE)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("DEMO PASSED", r.stdout)
        self.assertNotIn("DEMO FAILED", r.stderr)
        self.assertEqual(stray_demo_dirs() - before, set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
