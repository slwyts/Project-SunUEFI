"""Check that the builder cache follows container inputs and archive bytes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build import ci_container
from build.cache import atomic_json, sha, source_identity


class CiContainerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ci-container-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
        subprocess.run(['git', 'init', '--quiet', self.root], env=env, check=True)
        for name in (*ci_container.INPUTS, 'tools/build/shared_products.py', 'config/release.json'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ci_container.ROOT / name, path)

    def test_cache_key_tracks_only_the_container_and_its_build_tools(self):
        original = ci_container.cache_key(self.root)
        for name in ('tools/build/shared_products.py', 'config/release.json'):
            path = self.root / name
            path.write_bytes(path.read_bytes() + b'\n')
            self.assertEqual(ci_container.cache_key(self.root), original)
        for name in ('containers/Dockerfile', 'config/build-container.json',
                     'tools/build_container.py', 'tools/build/ci_container.py',
                     'tools/build/cache.py'):
            with self.subTest(name=name):
                path = self.root / name
                before = path.read_bytes()
                path.write_bytes(before + b'\n')
                self.assertNotEqual(ci_container.cache_key(self.root), original)
                path.write_bytes(before)

    def test_corrupt_archive_is_rejected_before_docker_load(self):
        directory = self.root / 'build/ci-builder'
        directory.mkdir(parents=True)
        archive = directory / 'image.tar'
        archive.write_bytes(b'original bytes')
        atomic_json(directory / 'manifest.json', {
            'inputs': source_identity(self.root, ci_container.INPUTS),
            'archive_sha256': sha(archive),
            'image_id': 'sha256:' + '1' * 64,
        })
        archive.write_bytes(b'changed bytes!')
        with patch.object(ci_container, 'ROOT', self.root):
            with self.assertRaisesRegex(ValueError, 'Shared builder image cache is missing or changed'):
                ci_container.prepare(load_only=True)


if __name__ == '__main__':
    unittest.main()
