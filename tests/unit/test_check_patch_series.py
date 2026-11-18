"""Patch series order and shared references, using real unified/Git diffs."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'check_patch_series', ROOT / 'tools/build/check_patch_series.py')
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

DIFF = ('--- a/example.c\n+++ b/example.c\n@@ -1 +1 @@\n'
        '-old();\n+fixed();\n')
UPSTREAM = {'status': 'downstream', 'url': 'https://example.org/source',
            'drop_when': 'The selected upstream source includes the fix and passes its regression test.'}


class CheckPatchSeriesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='patch-series-')
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def patch(self, name, text=DIFF):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_verbatim_patch_id_ignores_mail_header_but_preserves_spaces(self):
        mail = 'From ' + 'a' * 40 + ' Mon Sep 17 00:00:00 2001\n\n'
        diff = DIFF.replace('+fixed();', '+fixed("Audio Route");')
        original = self.patch('original.patch', mail + diff)
        replayed = self.patch('replayed.patch', mail.replace('a' * 40, 'b' * 40) + diff)
        changed = self.patch('changed.patch', mail + diff.replace('Audio Route', 'AudioRoute'))
        self.assertEqual(checker.patch_id(original, verbatim=True),
                         checker.patch_id(replayed, verbatim=True))
        self.assertNotEqual(checker.patch_id(original, verbatim=True),
                            checker.patch_id(changed, verbatim=True))

    def series(self, name, patches, **fields):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        rows = [{'file': filename, 'sha256': checker.digest(path.parent / filename),
                 'upstream': dict(UPSTREAM)}
                for filename in patches]
        path.write_text(json.dumps({'schema_version': 1, 'targets': ['steamos'],
                                    'patches': rows, **fields}))
        return path

    def test_repository_series_are_consistent(self):
        report = checker.check(ROOT)
        self.assertEqual(report['errors'], [])
        self.assertGreater(report['shared_files'], 0)
        self.assertGreater(report['series'], 0)

    def test_shared_file_is_not_a_duplicate_and_legacy_path_key_is_read(self):
        shared = self.patch('patches/example/1.0/fix.patch')
        self.series('patches/example/1.0/series.json', ['fix.patch'])
        other = self.series('patches/example/2.0/series.json', ['../1.0/fix.patch'])
        # FFmpeg uses an ordered series/path list instead of patches/file.
        other.write_text(json.dumps({'schema_version': 1, 'targets': ['nyarch-gnome'],
                                     'series': [{'path': '../1.0/fix.patch',
                                                 'sha256': checker.digest(shared)}]}))
        report = checker.check(self.root)
        self.assertEqual(report['errors'], [])
        self.assertEqual((report['patches'], report['shared_files']), (1, 1))

    def test_identical_copies_and_same_changes_with_different_headers_fail(self):
        self.patch('patches/example/1.0/fix.patch')
        self.series('patches/example/1.0/series.json', ['fix.patch'])
        copy = self.patch('patches/example/2.0/fix.patch')
        self.series('patches/example/2.0/series.json', ['fix.patch'])
        self.assertTrue(any('identical files' in error for error in
                            checker.check(self.root)['errors']))
        copy.write_text('Subject: different mail header\n\n'
                        'diff --git a/example.c b/example.c\n' +
                        DIFF.replace('@@ -1 +1 @@', '@@ -20 +20 @@'))
        self.series('patches/example/2.0/series.json', ['fix.patch'])
        errors = checker.check(self.root)['errors']
        self.assertEqual(len(errors), 1, errors)
        self.assertIn('same Git patch-id', errors[0])

    def test_series_repeated_patch_is_an_error(self):
        self.patch('patches/example/1.0/fix.patch')
        self.series('patches/example/1.0/series.json', ['fix.patch', './fix.patch'])
        self.assertTrue(any('occurs twice' in error for error in
                            checker.check(self.root)['errors']))

    def test_matrix_checks_unused_versions_unknown_products_and_orphans(self):
        self.patch('patches/example/1.0/fix.patch')
        self.series('patches/example/1.0/series.json', ['fix.patch'], targets=['retired-product'])
        old = self.root / 'patches/example/0.9/series.json'
        old.parent.mkdir(parents=True)
        old.write_text(json.dumps({'patches': []}))
        self.patch('patches/example/0.9/unused.patch')
        errors = checker.check(self.root)['errors']
        for reason in ('targets must name distinct CI products', 'targets must be a nonempty product list',
                       'patch is outside every series'):
            self.assertTrue(any(reason in error for error in errors), (reason, errors))

    def test_hash_drift_and_repository_escape_fail(self):
        path = self.patch('patches/example/1.0/fix.patch')
        series = self.series('patches/example/1.0/series.json', ['fix.patch'])
        path.write_text(DIFF.replace('fixed()', 'changed()'))
        self.assertTrue(any('SHA256 differs' in error for error in
                            checker.check(self.root)['errors']))
        data = json.loads(series.read_text())
        data['patches'][0]['file'] = '../../../../outside.patch'
        series.write_text(json.dumps(data))
        self.assertTrue(any('missing or unsafe patch' in error for error in
                            checker.check(self.root)['errors']))

    def test_upstream_metadata_requires_drop_condition_and_consistent_shared_status(self):
        self.patch('patches/example/1.0/fix.patch')
        first = self.series('patches/example/1.0/series.json', ['fix.patch'])
        second = self.series('patches/example/2.0/series.json', ['../1.0/fix.patch'])
        metadata = {'status': 'merged', 'url': 'https://example.org/commit', 'commit': 'a' * 40}
        for path in (first, second):
            data = json.loads(path.read_text())
            data['patches'][0]['upstream'] = dict(metadata)
            if path == first:
                data['patches'][0]['upstream']['drop_when'] = 'The selected release includes the fix.'
            path.write_text(json.dumps(data))
        errors = checker.check(self.root)['errors']
        self.assertTrue(any('drop_when condition' in error for error in errors))
        self.assertTrue(any('shared upstream metadata conflicts' in error for error in errors))

    def test_patch_requires_provenance(self):
        self.patch('patches/example/1.0/fix.patch')
        path = self.series('patches/example/1.0/series.json', ['fix.patch'])
        data = json.loads(path.read_text())
        del data['patches'][0]['upstream']
        path.write_text(json.dumps(data))
        self.assertTrue(any('retained patch needs upstream' in error
                            for error in checker.check(self.root)['errors']))
        # A series-wide provenance record also covers its patches.
        data['upstream'] = dict(UPSTREAM)
        path.write_text(json.dumps(data))
        self.assertEqual(checker.check(self.root)['errors'], [])

    def test_series_requires_targets_without_a_previous_checkout(self):
        self.patch('patches/example/1.0/fix.patch')
        path = self.series('patches/example/1.0/series.json', ['fix.patch'])
        data = json.loads(path.read_text())
        del data['targets']
        for record in (data, dict(data, targets=None), dict(data, targets=[])):
            with self.subTest(record=record):
                path.write_text(json.dumps(record))
                self.assertEqual(checker.check(self.root)['errors'], [
                    'patches/example/1.0/series.json: targets must be a nonempty product list',
                ])

    def test_orphan_is_rejected_even_without_a_series_manifest(self):
        name = 'patches/example/1.0/fix.patch'
        self.patch(name)
        self.assertEqual(checker.check(self.root)['errors'],
                         [name + ': patch is outside every series'])

    def test_firmware_uses_uefi_target_and_patch_key(self):
        path = self.patch('patches/firmware/component/fix.patch')
        series = self.root / 'patches/firmware/series.json'
        series.write_text(json.dumps({'targets': ['uefi'], 'patches': [{
            'patch': 'component/fix.patch', 'repository': 'upstream/component',
            'sha256': checker.digest(path), 'upstream': UPSTREAM}]}))
        self.assertEqual(checker.check(self.root)['errors'], [])


if __name__ == '__main__':
    unittest.main()
