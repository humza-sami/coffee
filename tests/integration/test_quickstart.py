"""Run the README quickstart block literally (bash -e) in a temporary directory and assert the real outputs.
Only substitution: the documented default port 7077 is replaced by a free ephemeral port so the test cannot collide with
a coordinator already running on the developer's machine. `coffee` is resolved from COFFEE_BIN's directory."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from harness import COFFEE, ROOT, free_port


def quickstart_block():
    text = open(os.path.join(ROOT, "README.md")).read()
    m = re.search(r"<!-- quickstart:start -->\s*```sh\n(.*?)```\s*<!-- quickstart:end -->", text, re.S)
    assert m, "README quickstart markers not found"
    return m.group(1)


class Quickstart(unittest.TestCase):
    def test_documented_quickstart_runs_and_produces_the_documented_output(self):
        tmp = tempfile.mkdtemp(prefix="coffee-test-quickstart-")
        self.addCleanup(shutil.rmtree, tmp, True)
        os.symlink(os.path.join(ROOT, "examples"), os.path.join(tmp, "examples"))
        os.makedirs(os.path.join(tmp, "build"))
        os.symlink(COFFEE, os.path.join(tmp, "build", "coffee"))  # the block does: export PATH="$PWD/build:$PATH"
        script = quickstart_block().replace("7077", str(free_port()))
        env = {k: v for k, v in os.environ.items() if not k.startswith("COFFEE_")}
        r = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", "set -m\n" + script], cwd=tmp, env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout
        self.assertEqual(len(re.findall(r"^bean-[ab]\s+online", out, re.M)), 2, out)
        self.assertIn("job demo (pipeline-demo): succeeded", out)
        self.assertIn("outputs written to ./results", out)
        self.assertEqual(open(os.path.join(tmp, "results", "stats", "size.txt")).read(), "34\nHELLO COFFEE\nSECOND LINE OF INPUT\n")
        self.assertEqual(open(os.path.join(tmp, "results", "upper", "up.txt")).read(), "HELLO COFFEE\nSECOND LINE OF INPUT\n")
        self.assertEqual(len(re.findall(r"#1 on bean-[ab]: succeeded", out)), 4, out)  # `brew --wait` and `status` each print both attempts
        left = subprocess.run(["pgrep", "-fa", "coffee (serve|join)"], capture_output=True, text=True).stdout
        self.assertNotIn(tmp, left)


if __name__ == "__main__":
    unittest.main(verbosity=2)
