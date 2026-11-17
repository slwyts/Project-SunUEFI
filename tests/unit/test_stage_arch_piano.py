"""Check the package policy read by pacman in Nyarch and SteamOS images."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from package.boot import PACMAN_REPOSITORIES, pacman_repository
from stage_arch_piano import pacman_configuration
from stage_steamos_desktop import configure_package_updates


class StageArchPianoTests(unittest.TestCase):
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
                    updated = pacman_configuration(text, steamos)
                    self.assertEqual(pacman_configuration(updated, steamos), updated)
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
