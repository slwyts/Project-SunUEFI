"""Check complete-package cache invalidation without rebuilding a desktop."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import package_build


class PackageBuildTests(unittest.TestCase):
    def test_cache_reuses_complete_outputs_and_rebuilds_changed_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'recipe'
            source.write_text('recipe')
            output = root / 'packages'
            command = [sys.executable, '-c',
                       'from pathlib import Path; p=Path(__import__("sys").argv[1]); '
                       'p.mkdir(exist_ok=True); (p/"package.deb").write_bytes(b"package")', str(output)]
            with patch.object(package_build, 'ROOT', root):
                package_build.run_cached(output, [source], command)
                # A hit must not execute even an invalid build command.
                package_build.run_cached(output, [source], ['/missing-build-command'])
                source.write_text('updated recipe')
                package_build.run_cached(output, [source], command)
                (output / 'package.deb').write_bytes(b'changed package')
                package_build.run_cached(output, [source], command)
            self.assertEqual((output / 'package.deb').read_bytes(), b'package')

    def test_empty_build_does_not_publish_a_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'recipe'
            source.write_text('recipe')
            output = root / 'packages'
            output.mkdir()
            with patch.object(package_build, 'ROOT', root), self.assertRaisesRegex(ValueError, 'no Debian packages'):
                package_build.run_cached(output, [source], [sys.executable, '-c', 'pass'])
            self.assertFalse((output / 'package-cache.json').exists())
