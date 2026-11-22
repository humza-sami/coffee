"""The algorithm examples survive installation: a real `cmake --install` into a temporary prefix carries every script, Recipe and
document, the installed Recipes validate with the installed binary, the installed scripts run from the installed tree without
writing into it, the documentation links resolve to real installed files in both the source and the installed layout, and no
private artifact or bytecode cache is installed. A bytecode cache that already exists in the source tree is legitimate: it must be
neither installed nor deleted, and the tests never require a pristine tree."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_install import BUILD, CMAKE, UNINSTALL, install, read_manifest, ROOT

ALGO_REL = "share/coffee/examples/algorithms"
ALGO_DOC_REL = "share/doc/coffee/examples/algorithms"  # the guide is also installed here so the documentation tree links resolve
FILES = ("README.md", "algo_common.py", "matrix_multiply.py", "merge_sort.py", "tsp_held_karp.py", "coffee-light.yaml", "coffee-medium.yaml", "coffee-heavy.yaml")
SIZES = ("light", "medium", "heavy")
SOURCE_ALGO = os.path.join(ROOT, "examples", "algorithms")
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def is_cache(rel):
    return "__pycache__" in rel.split(os.sep) or rel.endswith(".pyc")


def tree_state(root):
    """Relative path -> SHA-256 for every regular file below root, caches included."""
    out = {}
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            with open(p, "rb") as f:
                out[os.path.relpath(p, root)] = hashlib.sha256(f.read()).hexdigest()
    return out


def markdown_links(path):
    """(line number, target) of every relative file link in a Markdown file; URLs, mail and pure anchors are skipped, code fences ignored."""
    links, fenced = [], False
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            if line.lstrip().startswith("```"):
                fenced = not fenced
            if fenced:
                continue
            for target in LINK.findall(line):
                if re.match(r"^([a-z][a-z0-9+.-]*:|#)", target, re.I):
                    continue
                links.append((number, target.split("#", 1)[0]))
    return links


def broken_links(path, boundary):
    """Links of the Markdown file that do not resolve to an existing regular file inside boundary (resolved on the real filesystem)."""
    bad = []
    for number, target in markdown_links(path):
        resolved = os.path.realpath(os.path.join(os.path.dirname(path), target))
        inside = os.path.commonpath([resolved, os.path.realpath(boundary)]) == os.path.realpath(boundary)
        if not (inside and os.path.isfile(resolved)):
            bad.append("%s:%d -> %s (resolves to %s)" % (os.path.relpath(path, boundary), number, target, resolved))
    return bad


class InstalledExamples(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="coffee-test-examples-install-")
        cls.source_before = tree_state(SOURCE_ALGO)  # whatever is there, e.g. a legitimate pre-existing __pycache__
        cls.prefix = os.path.join(cls.tmp, "prefix")
        install(cls.prefix, check=True)
        cls.exe = os.path.join(cls.prefix, "bin", "coffee")
        cls.algo = os.path.join(cls.prefix, *ALGO_REL.split("/"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, True)

    def test_every_needed_file_is_installed_and_recorded_in_the_manifest(self):
        entries, _ = read_manifest(self.prefix)
        for name in FILES:
            self.assertIn("%s/%s" % (ALGO_REL, name), entries)
            self.assertTrue(os.path.isfile(os.path.join(self.algo, name)), name)
        # the committed example files are exactly the expected ones; a cache that already exists beside them is not a source file
        source = sorted(n for n in os.listdir(SOURCE_ALGO) if n != "__pycache__")
        self.assertEqual(source, sorted(FILES))
        installed = sorted(rel[len(ALGO_REL) + 1:] for rel in entries if rel.startswith(ALGO_REL + "/"))
        self.assertEqual(installed, sorted(FILES))
        self.assertIn(ALGO_DOC_REL + "/README.md", entries)
        # nothing generated or private rides along, whatever the source tree happened to contain
        for rel in entries:
            self.assertFalse(is_cache(rel), rel)
            self.assertFalse(rel.endswith(".log"), rel)
        actual = {os.path.relpath(os.path.join(d, n), self.prefix) for d, _, files in os.walk(self.prefix) for n in files}
        self.assertEqual({a for a in actual if is_cache(a)}, set())
        self.assertEqual(tree_state(SOURCE_ALGO), self.source_before)  # installing neither created nor removed anything in the source tree

    def test_installed_documentation_links_resolve_to_existing_installed_files(self):
        docs = os.path.join(self.prefix, "share", "doc", "coffee")
        pages = [os.path.join(docs, "README.md"), os.path.join(self.prefix, *ALGO_DOC_REL.split("/"), "README.md"), os.path.join(self.algo, "README.md")]
        for page in pages:
            self.assertTrue(os.path.isfile(page), page)
        # not vacuous: the root page links to the guide and the guide links to the build and limitation documents
        self.assertIn("examples/algorithms/README.md", [t for _, t in markdown_links(pages[0])])
        for page in pages[1:]:
            names = [os.path.basename(t) for _, t in markdown_links(page)]
            self.assertIn("native-build.md", names)
            self.assertIn("limitations.md", names)
        boundary = self.prefix
        self.assertEqual([b for page in pages for b in broken_links(page, boundary)], [])
        # the links land on the real installed documents, which are manifest-owned like everything else
        entries, _ = read_manifest(self.prefix)
        for page in pages[1:]:
            for _, target in markdown_links(page):
                rel = os.path.relpath(os.path.realpath(os.path.join(os.path.dirname(page), target)), os.path.realpath(self.prefix))
                self.assertIn(rel, entries)

    def test_source_documentation_links_resolve_too(self):
        for page in (os.path.join(ROOT, "README.md"), os.path.join(SOURCE_ALGO, "README.md")):
            self.assertEqual(broken_links(page, ROOT), [])
        self.assertIn("examples/algorithms/README.md", [t for _, t in markdown_links(os.path.join(ROOT, "README.md"))])

    def test_installed_recipes_validate_and_plan_with_the_installed_binary(self):
        for size in SIZES:
            path = os.path.join(self.algo, "coffee-%s.yaml" % size)
            for sub in (["validate", path], ["plan", path]):
                r = subprocess.run([self.exe] + sub, capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_installed_scripts_run_in_place_without_writing_into_the_installed_tree(self):
        def snapshot():
            return sorted((os.path.join(d, n), os.stat(os.path.join(d, n)).st_size) for d, _, files in os.walk(self.prefix) for n in files)
        before = snapshot()
        out = os.path.join(self.tmp, "out")
        os.makedirs(out)
        env = dict(os.environ)
        env.pop("PYTHONDONTWRITEBYTECODE", None)  # the scripts themselves must keep bytecode out of the installed tree
        for script in ("matrix_multiply.py", "merge_sort.py", "tsp_held_karp.py"):
            result = os.path.join(out, script + ".json")
            r = subprocess.run([sys.executable, os.path.join(self.algo, script), "--size", "light", "--output", result], cwd=out, capture_output=True,
                               text=True, timeout=300, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            with open(result) as f:
                self.assertTrue(json.load(f)["check"]["passed"])
        self.assertEqual(snapshot(), before)


class Packaging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coffee-test-examples-packaging-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.prefix = os.path.join(self.tmp, "prefix")

    def plant_cache(self):
        """Put a bytecode cache file into the source examples, as a Python import could; remove only what this test added."""
        cache = os.path.join(SOURCE_ALGO, "__pycache__")
        created_dir = not os.path.isdir(cache)
        os.makedirs(cache, exist_ok=True)
        planted = os.path.join(cache, "coffee-test-planted.cpython-0.pyc")
        self.assertFalse(os.path.exists(planted))
        with open(planted, "wb") as f:
            f.write(b"planted by test_examples_install")

        def cleanup():
            if os.path.exists(planted):
                os.unlink(planted)
            if created_dir and os.path.isdir(cache) and not os.listdir(cache):
                os.rmdir(cache)
        self.addCleanup(cleanup)
        return planted

    def test_a_source_bytecode_cache_is_neither_installed_nor_deleted(self):
        planted = self.plant_cache()
        before = tree_state(SOURCE_ALGO)
        self.assertIn(os.path.join("__pycache__", os.path.basename(planted)), before)
        # configure the real source tree (cache present) in a private build directory and read the install rules it generates
        cache = {}
        with open(os.path.join(BUILD, "CMakeCache.txt")) as f:
            for line in f:
                m = re.match(r"^(CMAKE_GENERATOR|CMAKE_MAKE_PROGRAM):[A-Z]+=(.*)$", line.rstrip("\n"))
                if m:
                    cache[m.group(1)] = m.group(2)
        build = os.path.join(self.tmp, "build")
        args = [CMAKE, "-S", ROOT, "-B", build, "-G", cache["CMAKE_GENERATOR"], "-DCOFFEE_BUILD_TESTS=OFF", "-DCOFFEE_BUILD_BENCH=OFF"]
        if cache.get("CMAKE_MAKE_PROGRAM"):
            args.append("-DCMAKE_MAKE_PROGRAM=" + cache["CMAKE_MAKE_PROGRAM"])
        r = subprocess.run(args, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(os.path.join(build, "cmake_install.cmake")) as f:
            rules = f.read()
        self.assertIn("examples/algorithms/matrix_multiply.py", rules)  # the rules do list the real sources
        self.assertNotIn("__pycache__", rules)
        self.assertNotIn(".pyc", rules)
        self.assertEqual(tree_state(SOURCE_ALGO), before)  # configuring left the pre-existing cache exactly as it was
        with open(planted, "rb") as f:
            self.assertEqual(f.read(), b"planted by test_examples_install")
        # and a real install from the main build, with the cache present, installs none of it and still preserves it
        install(self.prefix, check=True)
        entries, _ = read_manifest(self.prefix)
        self.assertEqual([rel for rel in entries if is_cache(rel)], [])
        self.assertEqual(tree_state(SOURCE_ALGO), before)

    def test_uninstall_removes_both_guide_copies_and_keeps_foreign_files(self):
        install(self.prefix, check=True)
        foreign = os.path.join(self.prefix, ALGO_DOC_REL, "notes-by-the-owner.txt")
        with open(foreign, "w") as f:
            f.write("mine")
        r = subprocess.run(["sh", UNINSTALL, "--prefix", self.prefix], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for rel in (ALGO_DOC_REL + "/README.md", ALGO_REL + "/README.md", ALGO_REL + "/merge_sort.py"):
            self.assertFalse(os.path.exists(os.path.join(self.prefix, rel)), rel)
        with open(foreign) as f:
            self.assertEqual(f.read(), "mine")

    def test_a_foreign_file_at_the_documentation_guide_path_blocks_installation_before_anything_is_written(self):
        target = os.path.join(self.prefix, ALGO_DOC_REL, "README.md")
        os.makedirs(os.path.dirname(target))
        with open(target, "w") as f:
            f.write("owner's own file")
        before = tree_state(self.prefix)
        r = install(self.prefix)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refusing to install", r.stdout + r.stderr)
        self.assertIn(ALGO_DOC_REL + "/README.md", r.stdout + r.stderr)
        self.assertEqual(tree_state(self.prefix), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
