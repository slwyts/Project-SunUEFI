"""Exercise real Git histories; no project or kernel refs are modified."""

import importlib.util
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).resolve().parents[1] / "tools/audit_kernel_upstream.py"
SPEC = importlib.util.spec_from_file_location("kernel_audit", TOOL)
kernel_audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(kernel_audit)


class KernelAuditTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "base")
        self.git("config", "user.name", "Audit fixture")
        self.git("config", "user.email", "audit@example.invalid")
        self.write("driver.c", "value = 1\n")
        self.base = self.commit("base")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True).stdout.strip()

    def write(self, name, text):
        (self.repo / name).write_text(text)

    def commit(self, subject):
        self.git("add", "--all")
        self.git("commit", "-q", "-m", subject)
        return self.git("rev-parse", "HEAD")

    def source_patch(self):
        self.git("switch", "-q", "-c", "source", self.base)
        self.write("driver.c", "value = 2\n")
        return self.commit("fix the device")

    def report(self, canonical=None, limit=100):
        refs = self.git("for-each-ref", "--format=%(refname) %(objectname)")
        head = self.git("rev-parse", "HEAD")
        worktree = self.git("status", "--porcelain")
        result = kernel_audit.audit(kernel_audit.Repository(self.repo),
                                    self.base, "source", "target", self.base,
                                    canonical or [], max_target_commits=limit)
        self.assertEqual(refs, self.git("for-each-ref", "--format=%(refname) %(objectname)"))
        self.assertEqual(head, self.git("rev-parse", "HEAD"))
        self.assertEqual(worktree, self.git("status", "--porcelain"))
        self.assertFalse(result["automatic_patch_removal"])
        return result

    def test_cherry_pick_with_different_history_is_equal(self):
        source = self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("unrelated.txt", "target-only preparation\n")
        self.commit("prepare the target")
        self.git("cherry-pick", source)
        target = self.git("rev-parse", "HEAD")
        self.assertNotEqual(source, target)
        row = self.report()["source_commits"][0]
        self.assertEqual("patch_id_match", row["status"])
        self.assertEqual([target], row["matching_commits"])

    def test_same_filename_and_subject_do_not_prove_equivalence(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value = 3\n")
        self.commit("fix the device")
        row = self.report()["source_commits"][0]
        self.assertEqual("no_patch_id_match_in_range", row["status"])
        self.assertEqual([], row["patch_id_candidates"])

    def test_whitespace_patch_id_match_is_not_accepted_as_equal(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value  = 2\n")
        self.commit("same patch-id, different changed bytes")
        row = self.report()["source_commits"][0]
        self.assertEqual("unknown", row["status"])
        self.assertEqual("patch_id_matches_but_changed_bytes_differ", row["reason"])
        self.assertTrue(row["patch_id_candidates"])

    def test_ancestor_is_reported_separately_even_after_a_revert(self):
        source = self.source_patch()
        self.git("switch", "-q", "-c", "target", source)
        self.git("revert", "--no-edit", source)
        row = self.report()["source_commits"][0]
        self.assertEqual("ancestor", row["status"])
        self.assertNotIn("matching_commits", row)

    def test_missing_canonical_commit_is_unknown(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value = 3\n")
        self.commit("target fix")
        row = self.report([{"name": "unfetched origin", "commit": "f" * 40}])["canonical_commits"][0]
        self.assertEqual("unknown", row["status"])
        self.assertEqual("commit_object_unavailable", row["reason"])

    def test_shallow_history_does_not_prove_non_equivalence(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value = 3\n")
        target = self.commit("target fix")
        (self.repo / ".git/shallow").write_text(target + "\n")
        report = self.report()
        self.assertEqual("unknown", report["source_commits"][0]["status"])
        self.assertFalse(report["target"]["scope"]["patch_search_complete"])
        self.assertIn(target, report["target"]["scope"]["shallow_boundaries"])

    def test_bounded_search_cannot_claim_absence(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value = 3\n")
        self.commit("target fix")
        self.write("unrelated.txt", "newer change\n")
        self.commit("newest change")
        report = self.report(limit=1)
        self.assertTrue(report["target"]["scope"]["truncated"])
        self.assertEqual("unknown", report["source_commits"][0]["status"])

    def test_partial_clone_never_fetches_missing_blobs(self):
        self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("driver.c", "value = 3\n")
        self.commit("target fix")
        self.git("config", "uploadpack.allowFilter", "true")
        partial = Path(self.temporary.name) / "partial"
        subprocess.run(["git", "clone", "-q", "--filter=blob:none", "--no-checkout",
                        self.repo.as_uri(), str(partial)], check=True, capture_output=True)
        marker = Path(self.temporary.name) / "fetch-attempted"
        uploadpack = Path(self.temporary.name) / "forbidden-uploadpack"
        uploadpack.write_text("#!/bin/sh\n: > " + shlex.quote(str(marker)) + "\nexit 1\n")
        uploadpack.chmod(0o700)
        subprocess.run(["git", "-C", str(partial), "config", "remote.origin.uploadpack",
                        str(uploadpack)], check=True)
        report = kernel_audit.audit(kernel_audit.Repository(partial), self.base,
                                    "origin/source", "origin/target", self.base, [])
        row = report["source_commits"][0]
        self.assertEqual("unknown", row["status"])
        self.assertEqual("source_patch_unavailable", row["reason"])
        self.assertFalse(marker.exists(), "read-only audit invoked a promisor fetch")

    def test_metadata_only_does_not_claim_patch_equivalence(self):
        source = self.source_patch()
        self.git("switch", "-q", "-c", "target", self.base)
        self.write("unrelated.txt", "target preparation\n")
        self.commit("prepare")
        self.git("cherry-pick", source)
        report = kernel_audit.audit(kernel_audit.Repository(self.repo), self.base,
                                    "source", "target", self.base, [], search_patch_ids=False)
        self.assertEqual("unknown", report["source_commits"][0]["status"])
        self.assertEqual("patch_id_search_disabled", report["source_commits"][0]["reason"])
        self.assertFalse(report["target"]["scope"]["patch_id_search_enabled"])


if __name__ == "__main__":
    unittest.main()
