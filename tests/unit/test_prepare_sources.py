"""Check stage-local source selection against the product source lock."""
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import prepare_sources as sources

ROOT = Path(__file__).resolve().parents[2]
TRUNCATED_PACK = 'error: RPC failed; curl 92 HTTP/2 stream was not closed cleanly\nfatal: early EOF\n'


class PrepareSourcesTests(unittest.TestCase):
    def test_stage_groups_cover_the_locked_repositories_once(self):
        locked = sources.expected(ROOT)
        combined = {}
        for group in sources.SOURCE_GROUPS:
            selected = sources.expected(ROOT, group)
            self.assertTrue(selected)
            self.assertFalse(selected.keys() & combined.keys())
            combined.update(selected)
        self.assertEqual(combined, locked)
        self.assertEqual(set(sources.expected(ROOT, 'kernel')), {'upstream/v4l2loopback'})

    def test_nested_repositories_are_fetched_after_their_parent(self):
        for group in ('all', *sources.SOURCE_GROUPS):
            rows = sources.expected(ROOT, group)
            seen = {Path('.')}
            for parent, paths in sources.submodule_batches(rows):
                self.assertIn(parent, seen)
                for path in paths:
                    self.assertNotIn(parent / path, seen)
                    seen.add(parent / path)
            self.assertEqual(seen - {Path('.')}, {Path(name) for name in rows})

    def test_retry_preserves_original_submodule_arguments(self):
        outcomes = [subprocess.CompletedProcess([], 1, stderr=TRUNCATED_PACK),
                    subprocess.CompletedProcess([], 0, stderr='')]
        with patch.object(sources.subprocess, 'run', side_effect=outcomes) as run, \
                redirect_stderr(StringIO()) as log:
            sources.update_submodules(ROOT, [Path('upstream/Mu-Silicium')])
        first, second = (row.args[0] for row in run.call_args_list)
        self.assertEqual(first, ['git', '-C', str(ROOT), 'submodule', 'update',
                                 '--init', '--depth', '1', '--', Path('upstream/Mu-Silicium')])
        self.assertEqual(second, first[:3] + ['-c', 'http.version=HTTP/1.1'] + first[3:])
        self.assertIn(TRUNCATED_PACK, log.getvalue())
        self.assertIn('Retrying the interrupted Git download', log.getvalue())

    def test_retry_does_not_mask_other_failures(self):
        for diagnostics, count in (
                (TRUNCATED_PACK, 2),
                ('fatal: remote error: upload-pack: not our ref 1234\n', 1),
                (TRUNCATED_PACK + 'fatal: the requested URL returned error: 403\n', 1),
                ('error: Your local changes would be overwritten by checkout\n', 1)):
            with self.subTest(diagnostics=diagnostics), \
                    patch.object(sources.subprocess, 'run',
                                 return_value=subprocess.CompletedProcess([], 1, stderr=diagnostics)) as run, \
                    redirect_stderr(StringIO()):
                with self.assertRaises(subprocess.CalledProcessError):
                    sources.update_submodules(ROOT, [Path('upstream/Mu-Silicium')])
                self.assertEqual(run.call_count, count)


if __name__ == '__main__':
    unittest.main()
