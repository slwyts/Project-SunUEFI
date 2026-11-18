"""Check the package policy read by pacman in Nyarch and SteamOS images."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from package.boot import PACMAN_REPOSITORIES, pacman_repository
import stage_arch_piano
from stage_steamos_desktop import configure_package_updates


class StageArchPianoTests(unittest.TestCase):
    def test_camera_is_requested_from_the_common_signed_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'piano-camera-1.0-1-aarch64.pkg.tar.zst'
            metadata = {'name': 'piano-camera', 'version': '1.0-1', 'sha256': '0' * 64}
            guest = Mock()
            with patch.object(stage_arch_piano.camera, 'shared_package',
                              return_value=(archive, metadata)) as shared, \
                    patch.object(stage_arch_piano.camera, 'ensure') as ensure, \
                    patch.object(stage_arch_piano.overlay_packages, 'install_pacman') as install:
                self.assertEqual(stage_arch_piano.install_camera(root, guest, published=True),
                                 (archive, metadata))
            shared.assert_called_once_with('pacman')
            ensure.assert_not_called()
            target, cache, repository, packages, runner = install.call_args.args
            self.assertEqual((target, repository, packages, runner),
                             (root, 'sunuefi', {archive.name: metadata}, guest))
            self.assertEqual(cache.parent, root / 'var/tmp')

    def test_local_camera_archive_matches_the_sealed_package_before_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'piano-camera-1.0-1-aarch64.pkg.tar.zst'
            archive.write_bytes(b'local camera archive')
            metadata = {'name': 'piano-camera', 'version': '1.0-1',
                        'sha256': stage_arch_piano.release.digest(archive)}
            calls = []

            def guest(arguments):
                calls.append(arguments)
                self.assertEqual(arguments[:3], ['pacman', '-U', '--noconfirm'])
                staged = root / arguments[3].lstrip('/')
                self.assertEqual(staged.name, archive.name)
                self.assertEqual(staged.read_bytes(), archive.read_bytes())

            with patch.object(stage_arch_piano.camera, 'ensure',
                              return_value=(archive, metadata)) as ensure:
                self.assertEqual(stage_arch_piano.install_camera(root, guest, published=False),
                                 (archive, metadata))
                ensure.assert_called_once_with('pacman')
                archive.write_bytes(b'changed archive')
                with self.assertRaisesRegex(ValueError, 'Camera package changed'):
                    stage_arch_piano.install_camera(root, guest, published=False)
            self.assertEqual(len(calls), 1)

    @unittest.skipUnless(shutil.which('pacman-conf'), 'pacman-conf required')
    def test_pacman_loads_the_signed_repositories_in_distribution_order(self):
        text = ('[options]\nArchitecture = aarch64\nSigLevel = Required DatabaseOptional\n'
                '\n[core]\nServer = https://example.invalid/$repo/os/$arch\n'
                'Include = /etc/pacman.d/sunuefi.conf # remove old placement\n')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in PACMAN_REPOSITORIES:
                (root / (name + '.conf')).write_text(pacman_repository(name))
            path = root / 'pacman.conf'
            for steamos, repos in ((False, ['sunuefi', 'core']),
                                   (True, ['sunuefi-steamos', 'sunuefi', 'core'])):
                with self.subTest(steamos=steamos):
                    updated = stage_arch_piano.pacman_configuration(text, steamos)
                    self.assertEqual(stage_arch_piano.pacman_configuration(updated, steamos), updated)
                    path.write_text(updated.replace('/etc/pacman.d/', directory + '/'))
                    command = ['pacman-conf', '--config', path]
                    self.assertEqual(subprocess.check_output([*command, '--repo-list'],
                                                            text=True).splitlines(), repos)
                    for repo in repos[:-1]:
                        flags = subprocess.check_output(
                            [*command, '--repo', repo, 'SigLevel'], text=True).splitlines()
                        self.assertIn('DatabaseRequired', flags)
                        self.assertIn('PackageRequired', flags)

    def test_steamos_only_releases_the_project_hold_after_installing_signed_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            policy = root / 'etc/pacman.conf'
            policy.parent.mkdir()
            (root / 'usr/bin').mkdir(parents=True)
            base = '[options]\nIgnorePkg = admin-selected\n\n[core]\n'
            policy.write_text(base)
            configure_package_updates(root)
            self.assertIn('IgnorePkg = deckard-mesa-linux-aarch64', policy.read_text())
            configure_package_updates(root, published_packages=True)
            self.assertEqual(policy.read_text(), base)


if __name__ == '__main__':
    unittest.main()
