"""Actual Git change lists select public product builds, without device inputs."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ci_build_targets as targets


class CiBuildTargetsTests(unittest.TestCase):
    def test_domains_and_shared_inputs(self):
        self.assertEqual(targets.select(['uefi/core/PianoProductCore.c']), ['uefi'])
        self.assertEqual(targets.select(['linux/dts/piano-audio-dmic-clock.dtso']), ['debian-gnome'])
        self.assertEqual(targets.select(['config/release.json']), ['debian-gnome'])
        self.assertEqual(targets.select(['sources.lock.json']), ['debian-gnome'])
        self.assertEqual(targets.select(['tools/build_simpleinit.sh']), ['uefi'])
        self.assertEqual(targets.select(['patches/firmware/series.json']), ['uefi'])
        self.assertEqual(targets.select(['upstream/piano-mesa-current']), ['debian-gnome'])
        self.assertEqual(targets.select(['patches/piano-sensors/import.patch']), ['debian-gnome'])
        self.assertEqual(targets.select(['android/native/piano-boot-request.c']), ['debian-gnome'])
        self.assertEqual(targets.select(['uefi/handoff/bootselect/BootRequest.h']), ['debian-gnome'])
        self.assertEqual(targets.select(['tools/export_installer.py']), ['debian-gnome'])
        self.assertEqual(targets.select(['docs/user/install-from-artifact.md']), ['debian-gnome'])
        self.assertEqual(targets.select(['uefi/core/PianoProductCore.c', 'linux/config']), ['debian-gnome'])
        self.assertEqual(targets.select(['README.md', 'docs/status.md']), [])
        self.assertEqual(targets.select(['patches/linux/7.2.9/drafts/0018-piano-pen-dock.patch',
                                         'linux/dts/drafts/piano-pen-dock.dtso']), [])
        self.assertEqual(targets.select(['linux/dts/drafts/piano-pen-dock.dtso',
                                         'config/release.json']), ['debian-gnome'])
        self.assertEqual(targets.select([], 'debian-gnome'), ['debian-gnome'])
        self.assertEqual(targets.select(['linux/config'], 'uefi'), ['uefi'])
        self.assertEqual(targets.select(['config/release.json'], 'linux'), ['linux'])
        with self.assertRaises(ValueError):
            targets.select([], 'unknown')

    def test_real_git_diff_and_new_push_fallback(self):
        with tempfile.TemporaryDirectory(prefix='ci-targets-') as temporary:
            root = Path(temporary)
            env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], env=env, text=True).strip()
            git('init', '-q')
            (root / 'linux').mkdir(); (root / 'linux/config').write_text('base\n')
            (root / 'build.sh').write_text('shared\n'); git('add', '.')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'base')
            before = git('rev-parse', 'HEAD')
            (root / 'linux/config').write_text('new\n'); git('add', '.')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Linux change')
            self.assertEqual(targets.select(targets.changed_files(root, before)), ['debian-gnome'])
            self.assertEqual(targets.select(targets.changed_files(root, '0' * 40)), ['debian-gnome'])


if __name__ == '__main__':
    unittest.main()
