"""Install/uninstall safety regressions: the install manifest is a destination like any other, symlinked
directories inside a managed tree are never followed, and refusal happens before anything is written or deleted. Every case runs through
both `cmake --install` and the `scripts/install.sh` wrapper where installation is involved."""
import os
import shutil
import subprocess
import tempfile
import unittest

from test_install import BUILD, CMAKE, ROOT, UNINSTALL, install, read, read_manifest, sha256, tree

INSTALL_SH = os.path.join(ROOT, "scripts", "install.sh")
MANIFEST = os.path.join("share", "coffee", "install-manifest.txt")


def install_wrapper(prefix):
    env = dict(os.environ, PATH=os.path.dirname(CMAKE) + os.pathsep + os.environ["PATH"])
    return subprocess.run(["sh", INSTALL_SH, "--prefix", prefix, "--build-dir", BUILD], capture_output=True, text=True, timeout=120, env=env)


def uninstall(prefix, script=UNINSTALL, *extra):
    return subprocess.run(["sh", script, "--prefix", prefix, *extra], capture_output=True, text=True, timeout=60)


class Base(unittest.TestCase):
    INSTALLERS = (("cmake", install), ("wrapper", lambda p: install_wrapper(p)))

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coffee-test-safety-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.prefix = os.path.join(self.tmp, "prefix")
        self.external = os.path.join(self.tmp, "external")
        os.makedirs(self.external)

    def write(self, path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def fresh(self, name):
        p = os.path.join(self.tmp, name)
        return p


class ManifestIsADestination(Base):
    def each_installer(self, setup, check):
        for label, run in self.INSTALLERS:
            with self.subTest(installer=label):
                prefix = self.fresh("p-" + label)
                ext = setup(prefix)
                before, ext_before = tree(prefix), tree(self.external)
                r = run(prefix)
                self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("refusing to install", r.stdout + r.stderr)
                self.assertEqual(tree(prefix), before, "refusal must leave the prefix byte-identical (nothing partially written)")
                self.assertEqual(tree(self.external), ext_before)
                check(prefix, ext)

    def test_unrelated_manifest_file_is_refused_and_preserved(self):
        def setup(prefix):
            self.write(os.path.join(prefix, MANIFEST), "foreign manifest sentinel")
        self.each_installer(setup, lambda p, e: self.assertEqual(read(os.path.join(p, MANIFEST)), "foreign manifest sentinel"))

    def test_valid_syntax_foreign_or_stale_manifest_is_refused(self):
        cases = {
            "foreign-entry": "unrelated-user-file",
            "user-entry-inside-coffee": "share/coffee/my-state/sentinel",
            "missing-binary": "bin/coffee",
            "missing-uninstaller": "share/coffee/uninstall.sh",
            "known-subtree-but-no-evidence": "share/doc/coffee/NOTICE",
        }
        for name, rel in cases.items():
            with self.subTest(case=name):
                text = "# coffee install manifest v1\n" + "0" * 64 + "  " + rel + "\n"
                self.each_installer(lambda prefix, t=text: self.write(os.path.join(prefix, MANIFEST), t), lambda p, e: None)
                for label, _ in self.INSTALLERS:
                    shutil.rmtree(self.fresh("p-" + label), True)

    def test_fake_anchor_digests_are_refused_without_mutation(self):
        def setup(prefix):
            self.write(os.path.join(prefix, "bin/coffee"), "foreign binary sentinel")
            self.write(os.path.join(prefix, "share/coffee/uninstall.sh"), "foreign script sentinel")
            self.write(os.path.join(prefix, MANIFEST), "# coffee install manifest v1\n" +
                       "0" * 64 + "  bin/coffee\n" + "0" * 64 + "  share/coffee/uninstall.sh\n")
        self.each_installer(setup, lambda p, e: None)

    def test_foreign_entry_is_refused_even_alongside_genuine_anchors(self):
        for label, run in self.INSTALLERS:
            with self.subTest(installer=label):
                prefix = self.fresh("mixed-" + label)
                install(prefix, check=True)
                user = os.path.join(prefix, "share/coffee/my-state/sentinel")
                self.write(user, "user-owned")
                with open(os.path.join(prefix, MANIFEST), "a") as f:
                    f.write(sha256(user) + "  share/coffee/my-state/sentinel\n")
                before = tree(prefix)
                r = run(prefix)
                self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertEqual(tree(prefix), before)

    def test_manifest_symlink_is_refused_and_target_untouched(self):
        target = os.path.join(self.external, "linked-manifest-target")
        self.write(target, "linked manifest sentinel")

        def setup(prefix):
            os.makedirs(os.path.dirname(os.path.join(prefix, MANIFEST)))
            os.symlink(target, os.path.join(prefix, MANIFEST))
        self.each_installer(setup, lambda p, e: self.assertEqual(read(target), "linked manifest sentinel"))

    def test_dangling_manifest_symlink_is_refused_and_not_created_through(self):
        target = os.path.join(self.external, "does-not-exist")

        def setup(prefix):
            os.makedirs(os.path.dirname(os.path.join(prefix, MANIFEST)))
            os.symlink(target, os.path.join(prefix, MANIFEST))
        self.each_installer(setup, lambda p, e: self.assertFalse(os.path.exists(target)))

    def test_malformed_or_non_file_manifest_is_refused(self):
        cases = {
            "wrong-header": "# something else\n%s  bin/coffee\n" % ("0" * 64),
            "empty": "",
            "header-only": "# coffee install manifest v1\n",
            "duplicate": "# coffee install manifest v1\n" + ("0" * 64 + "  bin/coffee\n") * 2,
            "garbage-line": "# coffee install manifest v1\nthis is not a digest line\n",
            "short-digest": "# coffee install manifest v1\nabc  bin/coffee\n",
            "unsafe-path": "# coffee install manifest v1\n%s  ../escape\n" % ("0" * 64),
        }
        for name, text in cases.items():
            with self.subTest(case=name):
                self.each_installer(lambda prefix, t=text: self.write(os.path.join(prefix, MANIFEST), t), lambda p, e: None)
                for label, _ in self.INSTALLERS:
                    shutil.rmtree(self.fresh("p-" + label), True)

    def test_manifest_that_is_a_directory_is_refused(self):
        self.each_installer(lambda prefix: os.makedirs(os.path.join(prefix, MANIFEST)), lambda p, e: None)

    def test_legitimate_reinstall_with_valid_manifest_still_works_via_both_entry_points(self):
        for label, run in self.INSTALLERS:
            with self.subTest(installer=label):
                prefix = self.fresh("ok-" + label)
                for _ in range(2):
                    r = run(prefix)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                entries, lines = read_manifest(prefix)
                for rel, digest in entries.items():
                    self.assertTrue(os.path.isfile(os.path.join(prefix, rel)), rel)
                self.assertEqual(os.stat(os.path.join(prefix, MANIFEST)).st_mode & 0o170000, 0o100000)
                self.assertFalse(os.path.islink(os.path.join(prefix, MANIFEST)))


class SymlinkedInstallParents(Base):
    def test_symlinked_directories_below_the_prefix_are_never_followed_on_install(self):
        for sub in ("bin", "share", "share/coffee", "share/doc", "share/doc/coffee", "share/coffee/examples"):
            for label, run in self.INSTALLERS:
                with self.subTest(parent=sub, installer=label):
                    prefix = self.fresh("p-%s-%s" % (sub.replace("/", "_"), label))
                    ext = os.path.join(self.external, "ext-%s-%s" % (sub.replace("/", "_"), label))
                    os.makedirs(ext)
                    self.write(os.path.join(ext, "keep.txt"), "external sentinel")
                    link = os.path.join(prefix, sub)
                    os.makedirs(os.path.dirname(link))
                    os.symlink(ext, link)
                    before, ext_before = tree(prefix), tree(ext)
                    r = run(prefix)
                    self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertIn("refusing to install", r.stdout + r.stderr)
                    self.assertEqual(tree(prefix), before)
                    self.assertEqual(tree(ext), ext_before)

    def test_regular_file_in_parent_position_is_refused_before_writing(self):
        for sub in ("share", "share/doc", "share/coffee/examples"):
            for label, run in self.INSTALLERS:
                with self.subTest(parent=sub, installer=label):
                    prefix = self.fresh("file-%s-%s" % (sub.replace("/", "_"), label))
                    self.write(os.path.join(prefix, sub), "foreign parent sentinel")
                    before = tree(prefix)
                    r = run(prefix)
                    self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertIn("refusing to install", r.stdout + r.stderr)
                    self.assertEqual(tree(prefix), before)

    def test_a_symlinked_prefix_itself_is_allowed_and_removal_works(self):
        real = os.path.join(self.tmp, "real-prefix")
        os.makedirs(real)
        link = os.path.join(self.tmp, "link-prefix")
        os.symlink(real, link)
        r = install_wrapper(link)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.exists(os.path.join(real, "bin", "coffee")))
        r = uninstall(link, os.path.join(link, "share", "coffee", "uninstall.sh"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(os.path.join(real, "bin", "coffee")))


class UninstallDoesNotFollowSymlinkedParents(Base):
    def installed(self):
        install(self.prefix, check=True)
        return self.prefix

    def test_every_symlinked_ancestor_is_refused_before_anything_is_deleted(self):
        for sub in ("bin", "share", "share/coffee", "share/doc", "share/doc/coffee", "share/coffee/examples", "share/coffee/examples/pipeline"):
            with self.subTest(parent=sub):
                shutil.rmtree(self.prefix, True)
                prefix = self.installed()
                ext = os.path.join(self.external, "ext-" + sub.replace("/", "_"))
                real = os.path.join(prefix, sub)
                shutil.move(real, ext)  # the managed files now live outside; the prefix path is a symlink to them
                os.symlink(ext, real)
                script = os.path.join(self.tmp, "uninstall-copy.sh")  # the installed copy may itself sit below the symlink
                shutil.copy(UNINSTALL, script)
                before, ext_before = tree(prefix), tree(ext)
                r = uninstall(prefix, script)
                self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("symbolic link", r.stdout + r.stderr)
                self.assertEqual(tree(ext), ext_before, "external files must survive byte-for-byte")
                self.assertEqual(tree(prefix), before, "nothing may be removed once an unsafe path is seen")
                shutil.rmtree(ext, True)

    def test_parent_link_probe_scenario_bin_replaced_by_link(self):
        prefix = self.installed()
        ext = os.path.join(self.external, "bin")
        shutil.move(os.path.join(prefix, "bin"), ext)
        os.symlink(ext, os.path.join(prefix, "bin"))
        r = uninstall(prefix, os.path.join(prefix, "share", "coffee", "uninstall.sh"))
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue(os.path.isfile(os.path.join(ext, "coffee")))

    def test_symlinked_manifest_is_refused(self):
        prefix = self.installed()
        manifest = os.path.join(prefix, MANIFEST)
        ext = os.path.join(self.external, "manifest-copy")
        shutil.move(manifest, ext)
        os.symlink(ext, manifest)
        before = tree(prefix)
        r = uninstall(prefix, UNINSTALL)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(tree(prefix), before)
        self.assertTrue(os.path.isfile(ext))

    def test_empty_directory_pruning_does_not_follow_a_symlinked_coffee_dir(self):
        prefix = self.installed()
        entries, _ = read_manifest(prefix)
        # an empty external directory that a followed prune would remove
        victim = os.path.join(self.external, "empty-dir")
        os.makedirs(os.path.join(victim, "nested"))
        for rel in list(entries):
            if rel.startswith("share/doc/coffee/"):
                os.remove(os.path.join(prefix, rel))  # files already gone; only directories remain
        shutil.rmtree(os.path.join(prefix, "share/doc/coffee"))
        os.symlink(victim, os.path.join(prefix, "share/doc/coffee"))
        r = uninstall(prefix, UNINSTALL)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.isdir(os.path.join(victim, "nested")))

    def test_unlisted_empty_directories_survive_removal(self):
        prefix = self.installed()
        locations = ("share/coffee/my-empty-dir/nested", "share/doc/coffee/user-layout/empty",
                     "share/coffee/examples/user-state/empty", "state/empty", "results/empty")
        for rel in locations:
            os.makedirs(os.path.join(prefix, rel))
        before = {rel: tree(os.path.join(prefix, rel)) for rel in locations}
        r = uninstall(prefix, os.path.join(prefix, "share/coffee/uninstall.sh"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(os.path.join(prefix, "bin/coffee")))
        self.assertFalse(os.path.exists(os.path.join(prefix, MANIFEST)))
        for rel in locations:
            self.assertTrue(os.path.isdir(os.path.join(prefix, rel)), rel)
            self.assertEqual(tree(os.path.join(prefix, rel)), before[rel])

    def test_header_only_manifest_is_rejected_without_removing_it(self):
        prefix = self.installed()
        self.write(os.path.join(prefix, MANIFEST), "# coffee install manifest v1\n")
        before = tree(prefix)
        r = uninstall(prefix, UNINSTALL)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(tree(prefix), before)

    def test_unterminated_malformed_last_line_is_rejected_before_deletion(self):
        prefix = self.installed()
        with open(os.path.join(prefix, MANIFEST), "a") as f:
            f.write("invalid final line without newline")
        before = tree(prefix)
        r = uninstall(prefix, UNINSTALL)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(tree(prefix), before)

    def test_healthy_removal_still_preserves_state_edits_and_unrelated_files(self):
        prefix = self.installed()
        state = os.path.join(self.tmp, "state")
        self.write(os.path.join(state, "sentinel"), "state stays")
        notice = os.path.join(prefix, "share", "doc", "coffee", "NOTICE")
        os.chmod(notice, 0o644)
        with open(notice, "a") as f:
            f.write("\nmy edit\n")
        mine = os.path.join(prefix, "share", "coffee", "mine.txt")
        self.write(mine, "unrelated")
        state_before = tree(state)
        r = uninstall(prefix, os.path.join(prefix, "share", "coffee", "uninstall.sh"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(os.path.join(prefix, "bin", "coffee")))
        self.assertIn("my edit", read(notice))
        self.assertEqual(read(mine), "unrelated")
        self.assertEqual(tree(state), state_before)
        self.assertFalse(os.path.exists(os.path.join(prefix, MANIFEST)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
