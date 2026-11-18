"""Round-trip the native archive used to transfer build inputs between jobs."""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest

ACTION = Path(__file__).resolve().parents[2] / '.github/actions/build-input/archive.sh'


@unittest.skipUnless(shutil.which('tar') and shutil.which('zstd'),
                     'The CI build input archive needs tar and zstd')
class BuildInputTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sunuefi-build-input-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.target = self.root / 'target'
        self.source.mkdir()
        self.target.mkdir()
        self.archive = self.root / 'input.tar.zst'

    def invoke(self, operation, workspace, *arguments, archive=None):
        return subprocess.run(
            ['bash', str(ACTION), operation, str(archive or self.archive), *arguments],
            cwd=workspace, capture_output=True, text=True, umask=0o077, timeout=30)

    def test_round_trip_preserves_hidden_files_permissions_and_links(self):
        inputs = self.source / 'build/kernel files'
        inputs.mkdir(parents=True)
        (inputs / '.config').write_text('CONFIG_LOCALVERSION="-sunuefi"\n')
        helper = inputs / 'make-helper'
        helper.write_text('#!/bin/sh\nexit 0\n')
        helper.chmod(0o751)
        (inputs / 'helper-link').symlink_to('make-helper')
        (inputs / '.git').mkdir()
        (inputs / '.git/config').write_text('not a build input\n')
        (self.source / 'seal.json').write_text('{}\n')

        packed = self.invoke('upload', self.source, 'build/kernel files\nseal.json\n')
        self.assertEqual(packed.returncode, 0, packed.stderr)
        restored = self.invoke('download', self.target)
        self.assertEqual(restored.returncode, 0, restored.stderr)

        output = self.target / inputs.relative_to(self.source)
        self.assertEqual((output / '.config').read_bytes(), (inputs / '.config').read_bytes())
        self.assertEqual((output / 'make-helper').read_bytes(), helper.read_bytes())
        self.assertEqual(stat.S_IMODE((output / 'make-helper').stat().st_mode), 0o751)
        self.assertEqual(os.readlink(output / 'helper-link'), 'make-helper')
        self.assertFalse((output / '.git').exists())
        self.assertEqual((self.target / 'seal.json').read_text(), '{}\n')

    def test_upload_rejects_workspace_root_and_escaping_paths(self):
        (self.root / 'other').mkdir()
        (self.root / 'other/input').touch()
        (self.source / 'alias').symlink_to(self.root / 'other', target_is_directory=True)
        for relative in ('', '.', '..', '../other/input', '/etc/hosts', 'alias/input',
                         '.git/config', 'missing'):
            with self.subTest(relative=relative):
                result = self.invoke('upload', self.source, relative)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr.strip())
                self.assertFalse(self.archive.exists())

        result = self.invoke('upload', self.source, 'alias',
                             archive=self.source / 'input.tar.zst')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside the workspace', result.stderr)

    def test_download_fails_on_missing_or_truncated_archive(self):
        absent = self.invoke('download', self.target)
        self.assertNotEqual(absent.returncode, 0)
        self.assertIn('archive is missing', absent.stderr)

        (self.source / 'input').write_bytes(b'actual build bytes' * 128)
        packed = self.invoke('upload', self.source, 'input')
        self.assertEqual(packed.returncode, 0, packed.stderr)
        self.archive.write_bytes(self.archive.read_bytes()[:16])
        self.assertNotEqual(self.invoke('download', self.target).returncode, 0)


if __name__ == '__main__':
    unittest.main()
