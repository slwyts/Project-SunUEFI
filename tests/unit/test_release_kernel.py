"""Prepare a release source tree from real local Git fixtures, without network."""
from copy import deepcopy
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prepare_release_kernel", ROOT / "tools/prepare_release_kernel.py"
)
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)

SOURCE_URL = "https://github.com/example/release-kernel-fixture.git"
PREPARATION_ERRORS = (ValueError, RuntimeError, subprocess.CalledProcessError)


@unittest.skipUnless(shutil.which("git"), "Git is required for release source fixtures")
class ReleaseKernelTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sunuefi-release-kernel-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repository = self.root / "local-kernel"
        self.repository.mkdir()
        self.environment = os.environ.copy()
        self.environment.update({
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "Fixture Author",
            "GIT_AUTHOR_EMAIL": "fixture-author@example.invalid",
            "GIT_COMMITTER_NAME": "Fixture Committer",
            "GIT_COMMITTER_EMAIL": "fixture-committer@example.invalid",
            "GIT_AUTHOR_DATE": "2020-01-02T03:04:05+00:00",
            "GIT_COMMITTER_DATE": "2020-01-02T03:04:05+00:00",
        })
        self.git("init", "--quiet")
        # A missing local identity must not stop a builder that uses a temporary
        # committer identity; disable signing regardless of the host setup.
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "protocol.allow", "never")
        self.git("config", "protocol.file.allow", "always")
        (self.repository / "config.txt").write_text("baseline\n", encoding="utf-8")
        self.git("add", "config.txt")
        self.git("commit", "--quiet", "-m", "Public fixture baseline")
        self.base_commit = self.git("rev-parse", "HEAD")
        self.base_tree = self.git("rev-parse", "HEAD^{tree}")
        self.patch_directory = self.root / "patches/linux"
        self.patch_directory.mkdir(parents=True)
        records = []
        for index, contents in enumerate(("baseline\nenabled\n", "baseline\nenabled\nready\n"), 1):
            parent = self.git("rev-parse", "HEAD")
            (self.repository / "config.txt").write_text(contents, encoding="utf-8")
            self.git("add", "config.txt")
            self.git("commit", "--quiet", "-m", f"Fixture source change {index}")
            commit = self.git("rev-parse", "HEAD")
            filename = f"{index:04d}-fixture-change.patch"
            patch_path = self.patch_directory / filename
            patch_path.write_bytes(self.git_bytes("format-patch", "--stdout", "-1", commit))
            records.append({
                "file": filename,
                "sha256": self.sha256(patch_path),
                "original_commit": commit,
                "original_parent": parent,
                "committer_date": self.git("show", "-s", "--format=%cI", commit),
                "tree": self.git("rev-parse", f"{commit}^{{tree}}"),
            })
        self.original_target_commit = self.git("rev-parse", "HEAD")
        self.target_tree = self.git("rev-parse", "HEAD^{tree}")
        self.manifest = {
            "schema_version": 1,
            "target": "fixture",
            "base_commit": self.base_commit,
            "base_tree": self.base_tree,
            "target_tree": self.target_tree,
            "original_target_commit": self.original_target_commit,
            "source_url": SOURCE_URL,
            "patches": records,
        }
        self.write_manifest()
        self.target = {
            "base_commit": self.base_commit,
            "target_tree": self.target_tree,
            "original_target_commit": self.original_target_commit,
            "source_url": SOURCE_URL,
        }
        self.worktree = self.root / "build/kernel-worktrees/release-kernel"
        self.source_manifest = self.root / "build/release-kernel/source-manifest.json"
        self.initial_worktrees = self.git("worktree", "list", "--porcelain")
        self.initial_head = self.git("rev-parse", "HEAD")
        self.initial_configuration = self.git("config", "--local", "--list")

    def git_bytes(self, *arguments, directory=None):
        result = subprocess.run(
            ["git", "-C", str(directory or self.repository), *arguments],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.environment,
        )
        return result.stdout

    def git(self, *arguments, directory=None):
        return self.git_bytes(*arguments, directory=directory).decode("utf-8").strip()

    @staticmethod
    def sha256(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def write_manifest(self):
        (self.patch_directory / "series.json").write_text(
            json.dumps(self.manifest, indent=2) + "\n", encoding="utf-8"
        )

    def prepare(self, **arguments):
        real_run = subprocess.run
        repository = arguments.pop("repository", self.repository)
        if repository is not None:
            arguments["repository"] = repository

        def local_run(command, *args, **kwargs):
            if isinstance(command, (list, tuple)):
                words = [str(word) for word in command]
                if words and Path(words[0]).name == "git":
                    if any(word in {"fetch", "clone", "ls-remote"} for word in words):
                        self.fail(f"A local fixture unexpectedly requested network access: {words}")
            return real_run(command, *args, **kwargs)

        with patch.dict(builder.TARGETS, {"fixture": self.target}, clear=True), \
                patch.dict(os.environ, {
                    "GIT_CONFIG_GLOBAL": os.devnull,
                    "GIT_CONFIG_NOSYSTEM": "1",
                }), patch.object(subprocess, "run", side_effect=local_run):
            return builder.prepare(
                root=self.root, target="fixture", **arguments,
            )

    def assert_no_preparation_mutation(self):
        self.assertFalse(self.worktree.exists())
        self.assertFalse(self.source_manifest.exists())
        self.assertEqual(self.git("worktree", "list", "--porcelain"), self.initial_worktrees)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.initial_head)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def assert_failure_retained(self):
        self.assertTrue(self.worktree.is_dir(), "Keep the failed worktree for inspection")
        self.assertTrue(self.source_manifest.is_file(), "Keep preparation failure metadata")
        failure = json.loads(self.source_manifest.read_text(encoding="utf-8"))
        self.assertEqual(failure["status"], "SOURCE_PREPARATION_FAILED")
        self.assertEqual(self.git("rev-parse", "HEAD"), self.initial_head)
        self.assertEqual(self.git("config", "--local", "--list"), self.initial_configuration)
        return failure

    def stable_fixture(self, conflict=False, resolve=False):
        original_branch = self.git('branch', '--show-current')
        self.git('checkout', '--detach', self.base_commit)
        (self.repository / 'Makefile').write_text('VERSION = 7\nPATCHLEVEL = 2\nSUBLEVEL = 9\n')
        (self.repository / 'stable.txt').write_text('official stable fix\n')
        if conflict:
            (self.repository / 'config.txt').write_text('stable baseline\n')
        self.git('add', '.')
        self.git('commit', '--quiet', '-m', 'Official stable fixture update')
        stable = self.git('rev-parse', 'HEAD')
        stable_tree = self.git('rev-parse', 'HEAD^{tree}')
        self.git('checkout', original_branch)
        merged = subprocess.run(['git', '-C', str(self.repository), 'merge-tree', '--write-tree',
                                 self.initial_head, stable], capture_output=True, text=True, env=self.environment)
        tree = merged.stdout.splitlines()[0]
        self.target.update(series_target='fixture', patch_tree=self.target_tree,
                           target_tree=tree, stable_commit=stable, stable_tree=stable_tree,
                           stable_version='7.2.9', stable_url=SOURCE_URL,
                           upstream_base_commit=self.base_commit, stable_merged_tree=tree,
                           merge_date='2020-01-03T00:00:00+00:00')
        if resolve:
            before = (self.repository / 'config.txt').read_text()
            after = before + 'stable baseline\n'
            resolution = self.patch_directory / 'resolve-config.patch'
            resolution.write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                            fromfile='a/config.txt', tofile='b/config.txt')))
            environment = self.environment.copy()
            environment['GIT_INDEX_FILE'] = str(self.root / 'merge-index')
            subprocess.run(['git', '-C', str(self.repository), 'read-tree', tree], check=True, env=environment)
            blob = subprocess.check_output(['git', '-C', str(self.repository), 'hash-object', '-w', '--stdin'],
                                           input=after.encode(), env=environment).decode().strip()
            subprocess.run(['git', '-C', str(self.repository), 'update-index', '--cacheinfo', '100644', blob, 'config.txt'],
                           check=True, env=environment)
            tree = subprocess.check_output(['git', '-C', str(self.repository), 'write-tree'], env=environment).decode().strip()
            self.target.update(target_tree=tree, stable_merged_tree=tree,
                               conflict_resolution={'source': 'config.txt', 'file': 'patches/linux/resolve-config.patch',
                                                    'sha256': self.sha256(resolution)})
        return stable

    def test_stable_merge_keeps_real_parentage_and_local_patch_content(self):
        stable = self.stable_fixture()
        result = self.prepare()
        metadata = result['stable_update']
        self.assertEqual(metadata['source_commit'], stable)
        self.assertEqual(metadata['version'], '7.2.9')
        self.assertEqual(self.git('show', '-s', '--format=%P', result['actual_commit'], directory=self.worktree).split(),
                         metadata['parents'])
        self.assertEqual(metadata['parents'][1], stable)
        self.assertEqual((self.worktree / 'config.txt').read_text(), 'baseline\nenabled\nready\n')
        self.assertEqual((self.worktree / 'stable.txt').read_text(), 'official stable fix\n')
        self.assertEqual(result['actual_tree'], self.target['target_tree'])
        self.assertEqual(len(result['patches']), 2)
        self.assertEqual(self.git('merge-base', self.base_commit, result['actual_commit'], directory=self.worktree), self.base_commit)

    def test_stable_merge_is_reproducible_and_repeated_prepare_preserves_marker(self):
        self.stable_fixture()
        first = self.prepare()
        marker = self.source_manifest.read_bytes()
        self.assertEqual(self.prepare(), first)
        self.assertEqual(self.source_manifest.read_bytes(), marker)
        second = self.prepare(worktree=self.root / 'build/kernel-worktrees/stable-reproduction',
                              source_manifest=self.root / 'build/stable-reproduction/source-manifest.json')
        self.assertEqual(first['actual_commit'], second['actual_commit'])
        self.assertEqual(first['actual_tree'], second['actual_tree'])

    def test_reviewed_merge_resolution_keeps_both_changes(self):
        self.stable_fixture(conflict=True, resolve=True)
        result = self.prepare()
        self.assertEqual(result['stable_update']['resolved_conflicts'], ['config.txt'])
        self.assertEqual((self.worktree / 'config.txt').read_text(), 'baseline\nenabled\nready\nstable baseline\n')
        self.assertEqual((self.worktree / 'stable.txt').read_text(), 'official stable fix\n')
        self.assertEqual(self.git('status', '--porcelain', directory=self.worktree), '')

    def test_unreviewed_stable_conflict_retains_merge_and_failure_record(self):
        self.stable_fixture(conflict=True)
        with self.assertRaisesRegex(ValueError, 'Stable merge failed'):
            self.prepare()
        failure = self.assert_failure_retained()
        self.assertEqual(failure['failing_patch'], 'merge Linux 7.2.9')
        self.assertIn('config.txt', failure['error'])
        self.assertIn('UU config.txt', self.git('status', '--porcelain', directory=self.worktree))

    def test_stable_resolution_patch_tamper_is_rejected_before_source_creation(self):
        self.stable_fixture(conflict=True, resolve=True)
        resolution = self.root / self.target['conflict_resolution']['file']
        resolution.write_bytes(resolution.read_bytes() + b'tampered\n')
        with self.assertRaisesRegex(ValueError, 'Stable update patch SHA'):
            self.prepare()
        self.assert_no_preparation_mutation()

    def test_applies_real_patches_and_records_exact_source_tree(self):
        configuration = self.initial_configuration
        environment = os.environ.copy()
        result = self.prepare()
        self.assertEqual(result["status"], "SOURCE_PREPARED_NOT_BUILT")
        self.assertEqual(result["actual_commit"], self.git("rev-parse", "HEAD", directory=self.worktree))
        self.assertEqual(result["actual_tree"], self.target_tree)
        self.assertEqual(result["original_target_commit"], self.original_target_commit)
        self.assertEqual(result["public_baseline"], self.base_commit)
        self.assertEqual((self.worktree / "config.txt").read_text(encoding="utf-8"), "baseline\nenabled\nready\n")
        self.assertEqual(self.git("rev-parse", "HEAD^{tree}", directory=self.worktree), self.target_tree)
        self.assertEqual(self.git("status", "--porcelain", directory=self.worktree), "")
        self.assertEqual(self.git("rev-list", "--count", f"{self.base_commit}..HEAD", directory=self.worktree), "2")
        self.assertEqual(len(result["patches"]), len(self.manifest["patches"]))
        for expected, applied in zip(self.manifest["patches"], result["patches"]):
            self.assertEqual(applied["file"], expected["file"])
            self.assertEqual(applied["sha256"], expected["sha256"])
            self.assertEqual(applied["applied_tree"], expected["tree"])
            self.assertEqual(
                self.git("rev-parse", f'{applied["applied_commit"]}^{{tree}}', directory=self.worktree),
                expected["tree"],
            )
            self.assertEqual(
                self.git("show", "-s", "--format=%cI", applied["applied_commit"], directory=self.worktree),
                expected["committer_date"],
            )
        self.assertEqual(json.loads(self.source_manifest.read_text(encoding="utf-8")), result)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.initial_head)
        self.assertEqual(self.git("config", "--local", "--list"), configuration)
        self.assertEqual(dict(os.environ), environment)

    def test_repeated_preparation_preserves_head_and_manifest(self):
        first = self.prepare()
        original_bytes = self.source_manifest.read_bytes()
        head = self.git("rev-parse", "HEAD", directory=self.worktree)
        second = self.prepare()
        self.assertEqual(second, first)
        self.assertEqual(self.source_manifest.read_bytes(), original_bytes)
        self.assertEqual(self.git("rev-parse", "HEAD", directory=self.worktree), head)
        self.assertEqual(self.git("rev-list", "--count", f"{self.base_commit}..HEAD", directory=self.worktree), "2")

    def test_explicit_output_paths_are_used(self):
        worktree = self.root / "build/kernel-worktrees/chosen-kernel"
        source_manifest = self.root / "build/chosen-metadata/kernel-source.json"
        result = self.prepare(worktree=worktree, source_manifest=source_manifest)
        self.assertEqual(self.git("rev-parse", "HEAD^{tree}", directory=worktree), self.target_tree)
        self.assertEqual(json.loads(source_manifest.read_text(encoding="utf-8")), result)
        self.assertFalse(self.worktree.exists())
        self.assertFalse(self.source_manifest.exists())

    def test_new_worktrees_reproduce_the_same_applied_commits(self):
        first_worktree = self.root / "build/kernel-worktrees/reproduction-one"
        second_worktree = self.root / "build/kernel-worktrees/reproduction-two"
        first = self.prepare(
            worktree=first_worktree,
            source_manifest=self.root / "build/reproduction-one/source-manifest.json",
        )
        second = self.prepare(
            worktree=second_worktree,
            source_manifest=self.root / "build/reproduction-two/source-manifest.json",
        )
        self.assertEqual(first["actual_commit"], second["actual_commit"])
        self.assertEqual(first["actual_tree"], second["actual_tree"])
        self.assertEqual(
            [row["applied_commit"] for row in first["patches"]],
            [row["applied_commit"] for row in second["patches"]],
        )
        self.assertEqual(self.git("rev-parse", "HEAD", directory=first_worktree), first["actual_commit"])
        self.assertEqual(self.git("rev-parse", "HEAD", directory=second_worktree), first["actual_commit"])

    def test_empty_kernel_directory_does_not_use_the_enclosing_project_repository(self):
        self.git("init", "--quiet", directory=self.root)
        (self.root / ".gitignore").write_text(
            "/build/\n/kernels/\n/local-kernel/\n/patches/\n", encoding="utf-8"
        )
        (self.root / "project.txt").write_text("Project fixture\n", encoding="utf-8")
        self.git("add", ".gitignore", "project.txt", directory=self.root)
        self.git("commit", "--quiet", "-m", "Project fixture baseline", directory=self.root)
        project_head = self.git("rev-parse", "HEAD", directory=self.root)
        empty_kernel = self.root / "upstream/linux-piano"
        empty_kernel.mkdir(parents=True)
        self.assertEqual(
            self.git("rev-parse", "--show-toplevel", directory=empty_kernel),
            str(self.root),
        )
        cache = self.root / "build/kernel-sources/linux-piano.git"
        cache.parent.mkdir(parents=True)
        # Local cloning is fixture setup only. The prepare wrapper below refuses
        # every fetch/clone, so repository discovery must use these cached objects.
        subprocess.run(
            ["git", "clone", "--quiet", "--bare", "--local", str(self.repository), str(cache)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.environment,
        )
        result = self.prepare(repository=None)
        self.assertEqual(result["repository"], str(cache.resolve()))
        self.assertEqual(result["actual_tree"], self.target_tree)
        self.assertEqual(self.git("rev-parse", "HEAD", directory=self.root), project_head)
        self.assertEqual(self.git("status", "--porcelain", directory=self.root), "")
        self.assertEqual(list(empty_kernel.iterdir()), [])

    def test_patch_tamper_is_rejected_before_worktree_creation(self):
        patch_path = self.patch_directory / self.manifest["patches"][0]["file"]
        patch_path.write_bytes(patch_path.read_bytes() + b"tampered after export\n")
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        self.assert_no_preparation_mutation()

    def test_pinned_target_drift_is_rejected_before_worktree_creation(self):
        self.manifest["target_tree"] = "0" * 40
        self.write_manifest()
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        self.assert_no_preparation_mutation()

    def test_invalid_schema_or_patch_chain_is_rejected_before_mutation(self):
        original = deepcopy(self.manifest)
        for changed in ("schema_version", "original_parent"):
            with self.subTest(changed=changed):
                self.manifest = deepcopy(original)
                if changed == "schema_version":
                    self.manifest[changed] = 2
                else:
                    self.manifest["patches"][1][changed] = self.base_commit
                self.write_manifest()
                with self.assertRaises(PREPARATION_ERRORS):
                    self.prepare()
                self.assert_no_preparation_mutation()

    def test_unknown_target_is_rejected_before_repository_commands(self):
        with patch.object(subprocess, "run", side_effect=AssertionError("Unknown target ran Git")):
            with self.assertRaises((ValueError, RuntimeError)):
                builder.prepare(root=self.root, repository=self.repository, target="unknown-fixture")
        self.assert_no_preparation_mutation()

    def test_staged_changes_are_preserved_and_preparation_is_refused(self):
        self.prepare()
        (self.worktree / "config.txt").write_text("local staged edit\n", encoding="utf-8")
        self.git("add", "config.txt", directory=self.worktree)
        head = self.git("rev-parse", "HEAD", directory=self.worktree)
        marker = self.source_manifest.read_bytes()
        staged_diff = self.git("diff", "--cached", directory=self.worktree)
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        self.assertEqual(self.git("rev-parse", "HEAD", directory=self.worktree), head)
        self.assertEqual(self.git("diff", "--cached", directory=self.worktree), staged_diff)
        self.assertEqual((self.worktree / "config.txt").read_text(encoding="utf-8"), "local staged edit\n")
        self.assertEqual(self.source_manifest.read_bytes(), marker)

    def test_existing_worktree_without_completed_marker_is_preserved_and_refused(self):
        self.worktree.parent.mkdir(parents=True)
        self.git("worktree", "add", "--quiet", "--detach", str(self.worktree), self.base_commit)
        head = self.git("rev-parse", "HEAD", directory=self.worktree)
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        self.assertEqual(self.git("rev-parse", "HEAD", directory=self.worktree), head)
        self.assertFalse(self.source_manifest.exists())
        self.assertEqual(self.git("status", "--porcelain", directory=self.worktree), "")

    def test_index_flags_cannot_hide_local_changes_from_preparation(self):
        self.prepare()
        head = self.git("rev-parse", "HEAD", directory=self.worktree)
        marker = self.source_manifest.read_bytes()
        for flag, clear_flag in (
            ("--assume-unchanged", "--no-assume-unchanged"),
            ("--skip-worktree", "--no-skip-worktree"),
        ):
            with self.subTest(flag=flag):
                self.git("update-index", flag, "config.txt", directory=self.worktree)
                (self.worktree / "config.txt").write_text("hidden local edit\n", encoding="utf-8")
                self.assertEqual(self.git("status", "--porcelain", directory=self.worktree), "")
                with self.assertRaises(PREPARATION_ERRORS):
                    self.prepare()
                self.assertEqual(self.git("rev-parse", "HEAD", directory=self.worktree), head)
                self.assertEqual(self.source_manifest.read_bytes(), marker)
                self.assertEqual((self.worktree / "config.txt").read_text(encoding="utf-8"), "hidden local edit\n")
                self.git("update-index", clear_flag, "config.txt", directory=self.worktree)
                self.git("restore", "config.txt", directory=self.worktree)

    def test_patch_conflict_retains_worktree_and_failure_metadata(self):
        patch_path = self.patch_directory / self.manifest["patches"][0]["file"]
        contents = patch_path.read_bytes()
        self.assertIn(b" baseline\n", contents)
        patch_path.write_bytes(contents.replace(b" baseline\n", b" absent-baseline\n", 1))
        self.manifest["patches"][0]["sha256"] = self.sha256(patch_path)
        self.write_manifest()
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        failure = self.assert_failure_retained()
        self.assertEqual(self.git("rev-parse", "HEAD", directory=self.worktree), self.base_commit)
        self.assertEqual((self.worktree / "config.txt").read_text(encoding="utf-8"), "baseline\n")
        self.assertEqual(failure["failing_patch"], self.manifest["patches"][0]["file"])
        self.assertIn("error", failure)

    def test_last_patch_tree_mismatch_retains_applied_source_for_inspection(self):
        patch_path = self.patch_directory / self.manifest["patches"][1]["file"]
        contents = patch_path.read_bytes()
        self.assertIn(b"+ready\n", contents)
        patch_path.write_bytes(contents.replace(b"+ready\n", b"+different-source\n", 1))
        self.manifest["patches"][1]["sha256"] = self.sha256(patch_path)
        self.write_manifest()
        with self.assertRaises(PREPARATION_ERRORS):
            self.prepare()
        failure = self.assert_failure_retained()
        self.assertEqual(
            (self.worktree / "config.txt").read_text(encoding="utf-8"),
            "baseline\nenabled\ndifferent-source\n",
        )
        actual_tree = self.git("rev-parse", "HEAD^{tree}", directory=self.worktree)
        self.assertNotEqual(actual_tree, self.target_tree)
        self.assertEqual(failure["actual_tree"], actual_tree)
        self.assertEqual(failure["failing_patch"], self.manifest["patches"][1]["file"])
        self.assertIn("error", failure)


if __name__ == "__main__":
    unittest.main()
