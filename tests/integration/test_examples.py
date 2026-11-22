"""University algorithm examples (examples/algorithms): independent oracles for matrix multiplication, merge sort and exact
Held-Karp TSP, the command-line contract, and the three ready Recipes validated and brewed on a real local Manager and Bean.
Every oracle is separate from the code under test: closed-form sums for the matrices, sorted() and a hand-derived sequence for
the sort, exhaustive permutations and a ring whose optimum is proven by an edge-count argument for the TSP. Result files are
compared on their stable fields only; elapsed_seconds and peak_rss_bytes are checked for presence and sanity, never for value."""
import hashlib
import importlib.util
import itertools
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from array import array

from harness import COFFEE, ROOT, CoffeeTestCase, free_port

sys.dont_write_bytecode = True
ALGO = os.path.join(ROOT, "examples", "algorithms")
SCRIPTS = {"matrix-multiply": "matrix_multiply.py", "merge-sort": "merge_sort.py", "tsp-exact": "tsp_held_karp.py"}
SIZES = ("light", "medium", "heavy")
MEASURED = ("elapsed_seconds", "peak_rss_bytes", "peak_virtual_bytes")
ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")


def load(name):
    if ALGO not in sys.path:
        sys.path.insert(0, ALGO)
    spec = importlib.util.spec_from_file_location("ex_" + name, os.path.join(ALGO, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def slurp(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def load_json(path):
    with open(path) as f:
        return json.load(f)


def source_state():
    """Relative path -> SHA-256 of every file below examples/algorithms, caches included, so a test can detect anything it created or changed
    without requiring a pristine tree (a legitimate pre-existing __pycache__ is part of the baseline)."""
    out = {}
    for d, _, files in os.walk(ALGO):
        for n in files:
            with open(os.path.join(d, n), "rb") as f:
                out[os.path.relpath(os.path.join(d, n), ALGO)] = hashlib.sha256(f.read()).hexdigest()
    return out


_BASELINE = {}


def setUpModule():
    _BASELINE.update(source_state())


def tearDownModule():
    after = source_state()
    changed = sorted(k for k in set(after) | set(_BASELINE) if after.get(k) != _BASELINE.get(k))
    if changed:
        raise AssertionError("the tests changed the source tree under examples/algorithms: %s" % changed)


def stable(result):
    return {k: v for k, v in result.items() if k not in MEASURED}


def run_script(script, args, cwd, directory=ALGO):
    return subprocess.run([sys.executable, os.path.join(directory, script)] + args, cwd=cwd, capture_output=True, text=True, timeout=300, env=ENV, stdin=subprocess.DEVNULL)


# ---- independent oracles -------------------------------------------------------------------------------------------------
def matrix_oracle_digest(n):
    """C = A*B for A[i][j] = 3i+5j-7 and B[j][k] = 2j-4k+11 by Faulhaber sums (no multiplication loop over j)."""
    s0, s1, s2 = n, n * (n - 1) // 2, (n - 1) * n * (2 * n - 1) // 6
    h = hashlib.sha256()
    for i in range(n):
        for k in range(n):
            p, q = 3 * i - 7, 11 - 4 * k  # A = p + 5j, B = q + 2j
            h.update((p * q * s0 + (2 * p + 5 * q) * s1 + 10 * s2).to_bytes(8, "little", signed=True))
    return h.hexdigest()


def brute_force_tsp(dist):
    n = len(dist)
    if n == 1:
        return 0
    return min(sum(dist[a][b] for a, b in zip((0,) + p, p + (0,))) for p in itertools.permutations(range(1, n)))


def random_symmetric(n, rng, hi=50):
    d = [[0] * n for _ in range(n)]
    for i in range(n):
        for j in range(i + 1, n):
            d[i][j] = d[j][i] = rng.randint(1, hi)
    return d


class MatrixMultiply(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mm = load("matrix_multiply")

    def test_hand_computed_three_by_three_product(self):
        a, b = self.mm.generate(3)
        self.assertEqual([list(r) for r in a], [[-7, -2, 3], [-4, 1, 6], [-1, 4, 9]])
        self.assertEqual([list(r) for r in b], [[11, 7, 3], [13, 9, 5], [15, 11, 7]])
        self.assertEqual([list(r) for r in self.mm.multiply(a, b)], [[-58, -34, -10], [59, 47, 35], [176, 128, 80]])

    def test_multiply_matches_a_plain_triple_loop_on_non_structured_matrices(self):
        rng = random.Random(7)
        for n in (1, 2, 4, 7):
            a = [array("q", [rng.randint(-99, 99) for _ in range(n)]) for _ in range(n)]
            b = [array("q", [rng.randint(-99, 99) for _ in range(n)]) for _ in range(n)]
            want = [[sum(a[i][j] * b[j][k] for j in range(n)) for k in range(n)] for i in range(n)]
            self.assertEqual([list(r) for r in self.mm.multiply(a, b)], want)

    def test_closed_form_matches_every_element_of_the_product(self):
        for n in (1, 2, 3, 8, 21):
            a, b = self.mm.generate(n)
            c = self.mm.multiply(a, b)
            for i in range(n):
                self.assertTrue(self.mm.row_matches(n, i, c[i]))
                for k in range(n):
                    s0, s1, s2 = n, n * (n - 1) // 2, (n - 1) * n * (2 * n - 1) // 6
                    p, q = 3 * i - 7, 11 - 4 * k
                    self.assertEqual(c[i][k], p * q * s0 + (2 * p + 5 * q) * s1 + 10 * s2)
                    self.assertEqual(self.mm.expected_element(n, i, k), c[i][k])

    def test_a_single_wrong_element_is_detected(self):
        a, b = self.mm.generate(6)
        c = self.mm.multiply(a, b)
        c[4][2] += 1
        self.assertFalse(self.mm.row_matches(6, 4, c[4]))
        self.assertTrue(self.mm.row_matches(6, 3, c[3]))

    def test_solve_reports_the_independent_digest_and_exact_answers(self):
        for n in (1, 5, 16):
            body = self.mm.solve(n)
            self.assertEqual(body["result"]["c_sha256"], matrix_oracle_digest(n))
            self.assertEqual(body["input"]["n"], n)
            self.assertTrue(body["check"]["passed"])
            self.assertEqual(body["check"]["elements_checked"], n * n)
            self.assertEqual(body["result"]["c_first"], self.mm.expected_element(n, 0, 0))
            self.assertEqual(body["result"]["c_last"], self.mm.expected_element(n, n - 1, n - 1))

    def test_sizes_are_bounded_increasing_and_declared_exactly(self):
        sizes = self.mm.SIZES
        self.assertEqual(tuple(sizes), SIZES)
        self.assertTrue(sizes["light"] < sizes["medium"] < sizes["heavy"] <= self.mm.MAX_N)
        with self.assertRaises(ValueError):
            self.mm.generate(self.mm.MAX_N + 1)
        with self.assertRaises(ValueError):
            self.mm.generate(0)


class MergeSort(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ms = load("merge_sort")

    def test_matches_sorted_oracle_on_many_shapes_and_lengths(self):
        rng = random.Random(11)
        for n in list(range(0, 40)) + [63, 64, 65, 127, 1000, 4097]:
            cases = {
                "random": [rng.randint(-10**12, 10**12) for _ in range(n)],
                "few-distinct": [rng.randint(-3, 3) for _ in range(n)],
                "ascending": list(range(n)),
                "descending": list(range(n, 0, -1)),
                "equal": [5] * n,
                "extremes": [rng.choice((-2**63, 2**63 - 1, 0)) for _ in range(n)],
            }
            for name, values in cases.items():
                got = self.ms.merge_sort(array("q", values))
                self.assertEqual(list(got), sorted(values), (n, name))

    def test_generated_data_is_unsorted_has_duplicates_and_negative_values(self):
        data, _ = self.ms.generate(4096)
        values = list(data)
        self.assertNotEqual(values, sorted(values))
        self.assertGreater(len(values) - len(set(values)), 1000)
        self.assertTrue(min(values) < 0 < max(values))
        self.assertEqual(sorted(values), [(j // 4 - 4096 // 8) * 1_000_003 for j in range(4096)])  # derived by hand, not by the generator

    def test_generated_data_oracle_holds_for_lengths_not_divisible_by_four(self):
        for n in (1, 2, 3, 5, 10, 13, 100, 1001):
            data, _ = self.ms.generate(n)
            self.assertEqual(sorted(data), [(j // 4 - n // 8) * 1_000_003 for j in range(n)], n)

    def test_verify_accepts_the_sorted_result_and_rejects_order_and_multiset_faults(self):
        n = 1000
        data, fp = self.ms.generate(n)
        good = self.ms.merge_sort(data)
        self.ms.verify(good, n, fp)
        swapped = array("q", good)
        swapped[10], swapped[500] = swapped[500], swapped[10]
        with self.assertRaises(self.ms.VerificationError):
            self.ms.verify(swapped, n, fp)
        replaced = array("q", good)
        replaced[7] = replaced[8]  # still non-decreasing, one element changed: only the multiset check can catch it
        self.assertEqual(list(replaced), sorted(replaced))
        with self.assertRaises(self.ms.VerificationError):
            self.ms.verify(replaced, n, fp)
        with self.assertRaises(self.ms.VerificationError):
            self.ms.verify(good[:-1], n, fp)

    def test_solve_reports_exact_answers(self):
        body = self.ms.solve(1000)
        want = hashlib.sha256(b"".join(((j // 4 - 1000 // 8) * 1_000_003).to_bytes(8, "little", signed=True) for j in range(1000))).hexdigest()
        self.assertEqual(body["result"]["sorted_sha256"], want)
        self.assertEqual(body["input"]["elements"], 1000)
        self.assertEqual(body["input"]["buffer_bytes"], 2 * 8 * 1000)
        self.assertTrue(body["check"]["passed"])

    def test_sizes_are_bounded_and_memory_estimate_is_small_enough_for_heavy(self):
        sizes = self.ms.SIZES
        self.assertEqual(tuple(sizes), SIZES)
        self.assertTrue(sizes["light"] < sizes["medium"] < sizes["heavy"] <= self.ms.MAX_ELEMENTS)
        self.assertLessEqual(16 * self.ms.MAX_ELEMENTS, 256 * 2**20)  # buffers alone stay far under the 512 MiB budget
        with self.assertRaises(ValueError):
            self.ms.generate(self.ms.MAX_ELEMENTS + 1)


class Tsp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tsp = load("tsp_held_karp")

    def check_tour(self, dist, cost, tour):
        n = len(dist)
        self.assertEqual(sorted(tour), list(range(n)))
        self.assertEqual(tour[0], 0)
        self.assertEqual(cost, sum(dist[a][b] for a, b in zip(tour, tour[1:] + tour[:1])) if n > 1 else 0)

    def test_matches_exhaustive_permutations_on_random_symmetric_matrices(self):
        rng = random.Random(2024)
        for n in range(1, 10):
            for trial in range(6 if n > 7 else 20):
                dist = random_symmetric(n, rng)
                cost, tour = self.tsp.held_karp(dist)
                self.assertEqual(cost, brute_force_tsp(dist), (n, trial, dist))
                self.check_tour(dist, cost, tour)

    def test_ties_and_degenerate_matrices(self):
        for n in (1, 2, 3, 6):
            dist = [[0 if i == j else 4 for j in range(n)] for i in range(n)]
            cost, tour = self.tsp.held_karp(dist)
            self.assertEqual(cost, 0 if n == 1 else 4 * n)
            self.check_tour(dist, cost, tour)
        self.assertEqual(self.tsp.held_karp([[0, 9], [9, 0]]), (18, [0, 1]))

    def test_rejects_invalid_matrices(self):
        for bad in ([], [[0, 1], [2, 0]], [[1, 1], [1, 0]], [[0, 1], [1]], [[0, -1], [-1, 0]]):
            with self.assertRaises(ValueError, msg=str(bad)):
                self.tsp.held_karp(bad)
        with self.assertRaises(ValueError):
            self.tsp.held_karp([[0] * (self.tsp.MAX_CITIES + 1) for _ in range(self.tsp.MAX_CITIES + 1)])

    def test_ring_instance_optimum_is_proved_by_edge_counting(self):
        for n in (3, 4, 5, 8, 11, 12, 13):
            dist, optimum = self.tsp.ring_instance(n)
            self.assertTrue(all(dist[i][j] == dist[j][i] for i in range(n) for j in range(n)))
            self.assertTrue(all(dist[i][i] == 0 for i in range(n)))
            g = min(dist[i][j] for i in range(n) for j in range(n) if i != j)
            # lower bound: a tour has n edges, each at least g; the g-edges form one Hamiltonian cycle, so the bound is reached
            self.assertEqual(optimum, n * g)
            cheapest = {(i, j) for i in range(n) for j in range(i + 1, n) if dist[i][j] == g}
            self.assertEqual(len(cheapest), n)
            degree = [sum(1 for e in cheapest if v in e) for v in range(n)]
            self.assertEqual(degree, [2] * n)
            seen, stack = {0}, [0]
            while stack:
                v = stack.pop()
                for a, b in cheapest:
                    for x, y in ((a, b), (b, a)):
                        if x == v and y not in seen:
                            seen.add(y)
                            stack.append(y)
            self.assertEqual(len(seen), n)
            cost, tour = self.tsp.held_karp(dist)
            self.assertEqual(cost, optimum)
            self.check_tour(dist, cost, tour)
            self.assertTrue(all(tuple(sorted(e)) in cheapest for e in zip(tour, tour[1:] + tour[:1])))
            if n <= 9:
                self.assertEqual(brute_force_tsp(dist), optimum)

    def test_ring_instance_is_scrambled_not_in_ring_order(self):
        dist, _ = self.tsp.ring_instance(13)
        self.assertNotEqual(sorted(range(13), key=lambda c: dist[0][c])[:3], [0, 1, 12])
        self.assertNotEqual(self.tsp.held_karp(dist)[1], list(range(13)))

    def test_wrong_claims_are_rejected(self):
        dist, optimum = self.tsp.ring_instance(7)
        cost, tour = self.tsp.held_karp(dist)
        self.tsp.verify(dist, cost, tour, optimum)
        duplicate = tour[:-1] + [tour[1]]
        for bad_cost, bad_tour, bad_opt in ((cost + 1, tour, optimum), (cost, tour[:-1], optimum), (cost, duplicate, optimum), (cost, tour, optimum + 1)):
            with self.assertRaises(self.tsp.VerificationError):
                self.tsp.verify(dist, bad_cost, bad_tour, bad_opt)
        # a tour whose own cost equals its claim is still not proof of optimality
        worst = sorted(range(1, 7), key=lambda c: -dist[0][c])
        t = [0] + worst
        c = sum(dist[a][b] for a, b in zip(t, t[1:] + t[:1]))
        self.assertGreater(c, optimum)
        with self.assertRaises(self.tsp.VerificationError):
            self.tsp.verify(dist, c, t, optimum)

    def test_solve_reports_exact_answer_and_labels_the_method(self):
        body = self.tsp.solve(9)
        self.assertEqual(body["input"]["cities"], 9)
        self.assertEqual(body["result"]["method"], "Held-Karp dynamic programming (exact)")
        dist, optimum = self.tsp.ring_instance(9)
        self.assertEqual(body["result"]["optimal_cost"], optimum)
        self.assertEqual(body["result"]["optimal_cost"], brute_force_tsp(dist))
        self.assertEqual(sorted(body["result"]["tour"]), list(range(9)))
        self.assertTrue(body["check"]["passed"])
        self.assertIn("lower bound", body["check"]["optimality_proof"])

    def test_sizes_are_bounded_and_dp_table_estimate_fits(self):
        sizes = self.tsp.SIZES
        self.assertEqual(tuple(sizes), SIZES)
        self.assertTrue(sizes["light"] < sizes["medium"] < sizes["heavy"] <= self.tsp.MAX_CITIES)
        self.assertLessEqual(self.tsp.MAX_CITIES, 22)  # no factorial or runaway default; 2^(n-1)*(n-1) cells at most
        self.assertLessEqual(self.tsp.table_bytes(self.tsp.SIZES["heavy"]), 256 * 2**20)


class CommandLine(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coffee-test-algo-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_light_runs_write_the_documented_result_and_are_stable_across_runs(self):
        for task, script in SCRIPTS.items():
            outs = []
            for run in (1, 2):
                out = os.path.join(self.tmp, "%s-%d.json" % (task, run))
                r = run_script(script, ["--size", "light", "--output", out], self.tmp)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                outs.append(load_json(out))
            for res in outs:
                self.assertEqual(res["size"], "light")
                self.assertTrue(res["check"]["passed"], res)
                self.assertGreater(res["elapsed_seconds"], 0)
                self.assertGreater(res["peak_rss_bytes"], 1 << 20)
                self.assertIn("algorithm", res)
                self.assertIn("input", res)
                self.assertIn("result", res)
            self.assertEqual(stable(outs[0]), stable(outs[1]), task)

    def test_running_the_scripts_leaves_the_source_tree_exactly_as_it_was(self):
        """A bytecode cache that already exists is legitimate and must survive; a new one, or any change, must not appear.
        The scripts run here without PYTHONDONTWRITEBYTECODE, so they have to keep bytecode out of the source tree themselves."""
        before = source_state()
        env = {k: v for k, v in os.environ.items() if k != "PYTHONDONTWRITEBYTECODE"}
        for script in SCRIPTS.values():
            out = os.path.join(self.tmp, script + ".json")
            r = subprocess.run([sys.executable, os.path.join(ALGO, script), "--size", "light", "--output", out], cwd=self.tmp, capture_output=True,
                               text=True, timeout=300, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(source_state(), before)

    def test_size_defaults_to_light(self):
        for script in SCRIPTS.values():
            out = os.path.join(self.tmp, "default.json")
            self.assertEqual(run_script(script, ["--output", out], self.tmp).returncode, 0)
            self.assertEqual(load_json(out)["size"], "light")

    def test_stable_fields_match_the_independent_oracles(self):
        mm = load("matrix_multiply")
        out = os.path.join(self.tmp, "mm.json")
        self.assertEqual(run_script("matrix_multiply.py", ["--size", "light", "--output", out], self.tmp).returncode, 0)
        res = load_json(out)
        self.assertEqual(res["input"]["n"], mm.SIZES["light"])
        self.assertEqual(res["result"]["c_sha256"], matrix_oracle_digest(mm.SIZES["light"]))
        out = os.path.join(self.tmp, "tsp.json")
        self.assertEqual(run_script("tsp_held_karp.py", ["--size", "light", "--output", out], self.tmp).returncode, 0)
        res = load_json(out)
        self.assertEqual(res["result"]["optimal_cost"], load("tsp_held_karp").ring_instance(res["input"]["cities"])[1])

    def test_invalid_size_and_missing_arguments_exit_nonzero_without_writing(self):
        out = os.path.join(self.tmp, "out.json")
        bad_args = (["--size", "huge", "--output", out], ["--size", "LIGHT", "--output", out], ["--size", "", "--output", out],
                    ["--size", "1000000", "--output", out], ["--size", "--output", out], ["--bogus", "--output", out], ["--size", "light"], [],
                    ["--size", "light", "--output", out, "extra"], ["--size", "li", "--output", out])  # no abbreviation of the value
        for script in SCRIPTS.values():
            for args in bad_args:
                r = run_script(script, args, self.tmp)
                self.assertNotEqual(r.returncode, 0, (script, args, r.stdout))
                self.assertFalse(os.path.exists(out), (script, args))
                self.assertEqual(r.stdout, "")

    def test_output_path_errors_exit_nonzero_before_doing_the_work(self):
        for script in SCRIPTS.values():
            for out in (os.path.join(self.tmp, "no-such-dir", "r.json"), self.tmp, os.path.join(self.tmp, "r.json", "x"), ""):
                if out.endswith("x"):
                    with open(os.path.join(self.tmp, "r.json"), "w"):
                        pass
                r = run_script(script, ["--size", "light", "--output", out], self.tmp)
                self.assertNotEqual(r.returncode, 0, (script, out))
                self.assertIn("output", r.stderr.lower(), (script, out, r.stderr))

    def test_output_never_overwrites_the_input_scripts(self):
        work = os.path.join(self.tmp, "staged")
        os.makedirs(work)
        for name in os.listdir(ALGO):
            if name.endswith(".py"):
                shutil.copy(os.path.join(ALGO, name), work)
        before = {n: slurp(os.path.join(work, n), "rb") for n in os.listdir(work)}
        os.symlink(os.path.join(work, "merge_sort.py"), os.path.join(work, "link.json"))
        for script in SCRIPTS.values():
            for target in sorted(before) + ["link.json"]:
                r = run_script(script, ["--size", "light", "--output", os.path.join(work, target)], work, directory=work)
                self.assertNotEqual(r.returncode, 0, (script, target))
        self.assertEqual({n: slurp(os.path.join(work, n), "rb") for n in before}, before)

    def test_existing_result_file_is_replaced_but_failure_leaves_it_alone(self):
        out = os.path.join(self.tmp, "r.json")
        with open(out, "w") as f:
            f.write("old")
        self.assertEqual(run_script("merge_sort.py", ["--size", "light", "--output", out], self.tmp).returncode, 0)
        self.assertTrue(load_json(out)["check"]["passed"])
        with open(out, "w") as f:
            f.write("keep")
        self.assertNotEqual(run_script("merge_sort.py", ["--size", "nope", "--output", out], self.tmp).returncode, 0)
        self.assertEqual(slurp(out), "keep")

    def test_verification_failure_exits_nonzero_and_writes_nothing(self):
        common = load("algo_common")
        out = os.path.join(self.tmp, "r.json")

        def bad(_size):
            raise common.VerificationError("injected mismatch")
        rc = common.main("t", "bad", {"light": 1, "medium": 2, "heavy": 3}, bad, ["--size", "light", "--output", out])
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(out))

        def lying(_size):
            return {"input": {}, "check": {"name": "x", "passed": False}, "result": {}}
        self.assertEqual(common.main("t", "bad", {"light": 1, "medium": 2, "heavy": 3}, lying, ["--size", "light", "--output", out]), 1)
        self.assertFalse(os.path.exists(out))


def parse_recipe(path):
    """Read the command, input and output lines of the Recipes in examples/algorithms (flow-style YAML written one field per line).
    Used only for facts `coffee plan --json` does not print; `coffee validate`, `coffee plan` and a real brew are the authority."""
    text = slurp(path)
    steps = {}
    for block in text.split("\n    - id: ")[1:]:
        tid = block.split("\n", 1)[0].strip()
        command = json.loads(re.search(r"^      command: (\[.*\])\s*$", block, re.M).group(1))
        inputs = re.findall(r"^        - \{source: (\S+), target: (\S+)\}\s*$", block, re.M)
        outputs = re.search(r"^      outputs: \[(.*?)\]\s*$", block, re.M).group(1).split(", ")
        steps[tid] = {"command": command, "inputs": inputs, "outputs": outputs}
    return steps


class Recipes(CoffeeTestCase):
    def recipe(self, size):
        return os.path.join(ALGO, "coffee-%s.yaml" % size)

    def plan(self, path):
        r = self.run_cli(["plan", path, "--json"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return json.loads(r.stdout)

    def planned_tasks(self, path):
        layers = self.plan(path)["layers"]
        self.assertEqual(len(layers), 1)  # three independent Steps: every one is in the first layer
        return layers[0]

    def test_each_recipe_validates_and_has_three_independent_one_cpu_steps(self):
        for size in SIZES:
            path = self.recipe(size)
            r = self.run_cli(["validate", path])
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            tasks = self.planned_tasks(path)
            steps = parse_recipe(path)
            self.assertEqual(sorted(t["id"] for t in tasks), sorted(SCRIPTS), size)
            self.assertEqual(sorted(steps), sorted(SCRIPTS), size)
            for t in tasks:
                self.assertEqual(t["depends_on"], [], (size, t["id"]))
                self.assertEqual(t["resources"]["cpus"], 1)
                self.assertTrue(256 <= t["resources"]["memory_mb"] <= 768, t)
                step = steps[t["id"]]
                self.assertEqual(step["outputs"], ["result.json"])
                self.assertEqual(sorted(i[1] for i in step["inputs"]), sorted([SCRIPTS[t["id"]], "algo_common.py"]))
                self.assertEqual(step["command"], ["python3", SCRIPTS[t["id"]], "--size", size, "--output", "result.json"])
                for source, target in step["inputs"]:
                    self.assertEqual(source, target)
                    self.assertTrue(os.path.isfile(os.path.join(ALGO, source)), source)

    def test_recipes_use_the_unchanged_schema_and_no_shell(self):
        for size in SIZES:
            text = slurp(self.recipe(size))
            self.assertIn("apiVersion: coffee/v1alpha1", text)
            self.assertNotIn("depends_on", text)

    def test_memory_reservations_never_shrink_with_the_size(self):
        mem = {size: {t["id"]: t["resources"]["memory_mb"] for t in self.planned_tasks(self.recipe(size))} for size in SIZES}
        for task in SCRIPTS:
            self.assertTrue(mem["light"][task] <= mem["medium"][task] <= mem["heavy"][task], (task, mem))

    def test_light_recipe_runs_on_a_local_manager_and_bean_and_results_verify(self):
        self.start_coordinator()
        self.start_worker("algo-a", cpus=3, memory_mb=3072)
        out = os.path.join(self.tmp, "results")
        r = self.cli(["brew", self.recipe("light"), "--job-id", "algo-light", "--wait", "--out", out, "--timeout", "240"], timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr + self.read_log("worker-algo-a.log"))
        self.assertEqual(self.job("algo-light")["state"], "succeeded")
        mm, tsp = load("matrix_multiply"), load("tsp_held_karp")
        for task in SCRIPTS:
            res = load_json(os.path.join(out, task, "result.json"))
            self.assertEqual(res["size"], "light")
            self.assertTrue(res["check"]["passed"])
            self.assertGreater(res["elapsed_seconds"], 0)
            if task == "matrix-multiply":
                self.assertEqual(res["result"]["c_sha256"], matrix_oracle_digest(mm.SIZES["light"]))
            if task == "tsp-exact":
                self.assertEqual(res["result"]["optimal_cost"], tsp.ring_instance(tsp.SIZES["light"])[1])


class ReadmeCommands(unittest.TestCase):
    def test_documented_brew_block_runs_on_a_local_manager_and_bean(self):
        text = slurp(os.path.join(ALGO, "README.md"))
        m = re.search(r"<!-- brew:start -->\s*```sh\n(.*?)```\s*<!-- brew:end -->", text, re.S)
        self.assertTrue(m, "README brew markers not found")
        tmp = tempfile.mkdtemp(prefix="coffee-test-algo-readme-")
        self.addCleanup(shutil.rmtree, tmp, True)
        os.makedirs(os.path.join(tmp, "build"))
        os.symlink(COFFEE, os.path.join(tmp, "build", "coffee"))
        os.makedirs(os.path.join(tmp, "examples"))
        os.symlink(ALGO, os.path.join(tmp, "examples", "algorithms"))
        script = m.group(1).replace("7077", str(free_port())).replace("/tmp/algo-", tmp + "/algo-").replace('"$PWD/../../build', '"%s/build' % tmp)
        env = {k: v for k, v in os.environ.items() if not k.startswith("COFFEE_")}
        os.makedirs(os.path.join(tmp, "algo-state"), exist_ok=True)
        r = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", "set -m\ncd '%s'\n%s" % (tmp, script)], env=env, capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK: workload 'algorithms-light' with 3 task(s) is valid", r.stdout)
        self.assertIn("job algo-light (algorithms-light): succeeded", r.stdout)
        res = load_json(os.path.join(tmp, "algo-results", "tsp-exact", "result.json"))
        self.assertTrue(res["check"]["passed"])
        for task in SCRIPTS:
            self.assertTrue(load_json(os.path.join(tmp, "algo-results", task, "result.json"))["check"]["passed"])
        for _ in range(150):  # `kill` only asks the processes to stop; give the Manager and Bean time to finish their shutdown
            left = subprocess.run(["pgrep", "-fa", "coffee (serve|join)"], capture_output=True, text=True).stdout
            if tmp not in left:
                break
            time.sleep(0.1)
        self.assertNotIn(tmp, left)


if __name__ == "__main__":
    unittest.main(verbosity=2)
