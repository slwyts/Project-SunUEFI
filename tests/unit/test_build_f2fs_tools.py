"""Fetch the pinned object through real Git transports, including mirror fallback."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('build_f2fs_tools', ROOT / 'tools/android/build_f2fs_tools.py')
f2fs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(f2fs)


class BuildF2fsToolsTests(unittest.TestCase):
    def test_unavailable_primary_uses_the_same_commit_from_mirror(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            upstream, cache = root / 'upstream', root / 'cache'
            for path in (upstream, cache):
                subprocess.run(['git', 'init', '-q', str(path)], check=True)
            (upstream / 'VERSION').write_text('fixed\n')
            subprocess.run(['git', '-C', str(upstream), 'add', 'VERSION'], check=True)
            commit = ['git', '-C', str(upstream), '-c', 'user.name=Build',
                      '-c', 'user.email=build@example.invalid', '-c', 'commit.gpgsign=false',
                      'commit', '-qm']
            subprocess.run([*commit, 'Fixed source'], check=True)
            pin = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
            (upstream / 'VERSION').write_text('moving tip\n')
            subprocess.run(['git', '-C', str(upstream), 'add', 'VERSION'], check=True)
            subprocess.run([*commit, 'Later source'], check=True)
            policy = {'base_commit': pin, 'source_url': str(root / 'unavailable'),
                      'source_mirrors': [str(upstream)]}
            f2fs.fetch_commit(cache, policy)
            self.assertEqual(subprocess.check_output(
                ['git', '-C', str(cache), 'show', pin + ':VERSION'], text=True), 'fixed\n')
            # Reuse the verified object even when both servers have gone away.
            f2fs.fetch_commit(cache, {**policy, 'source_mirrors': []})

    def test_unavailable_sources_do_not_accept_a_different_object(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            subprocess.run(['git', 'init', '-q', str(root / 'cache')], check=True)
            with self.assertRaisesRegex(ValueError, 'Cannot fetch the fixed F2FS commit'):
                f2fs.fetch_commit(root / 'cache', {'base_commit': '1' * 40,
                    'source_url': str(root / 'unavailable'), 'source_mirrors': []})


if __name__ == '__main__':
    unittest.main()
