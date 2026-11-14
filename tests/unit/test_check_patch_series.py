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

    def series(self, name, patches, **fields):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        rows = [{'file': filename, 'sha256': checker.digest(path.parent / filename)}
                for filename in patches]
        path.write_text(json.dumps({'schema_version': 1, 'targets': ['steamos'],
                                    'patches': rows, **fields}))
        return path

    def test_repository_series_are_consistent(self):
        report = checker.check(ROOT)
        self.assertEqual(report['errors'], [])
        self.assertGreater(report['shared_files'], 0)
        self.assertGreater(report['matrix_series'], 0)

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
        for reason in ('targets must name distinct CI products', 'version has no CI products',
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


if __name__ == '__main__':
    unittest.main()
