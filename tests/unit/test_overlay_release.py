"""Build and upgrade native packages with publication revisions."""
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import build_arch_package
from build import overlay_packages, overlay_release


class OverlayVersionTests(unittest.TestCase):
    def setUp(self):
        self.enterContext(patch.dict(os.environ))
        os.environ.pop(overlay_release.ENVIRONMENT, None)

    def test_local_versions_and_recipes_are_unchanged(self):
        recipe = 'pkgrel=1.2\n'
        self.assertEqual(overlay_release.pacman_recipe(recipe), recipe)
        self.assertEqual(overlay_release.debian_version('1:2.3-1+sunuefi2'), '1:2.3-1+sunuefi2')

    def test_invalid_publication_and_non_native_pkgrel_fail(self):
        for value in ('3', '00.3', '3.0', '3.x'):
            os.environ[overlay_release.ENVIRONMENT] = value
            with self.assertRaisesRegex(ValueError, 'RUN_ID.RUN_ATTEMPT'):
                overlay_release.generation()
        os.environ[overlay_release.ENVIRONMENT] = '3.1'
        for recipe in ('pkgrel=$release\n', 'pkgrel=1\npkgrel=2\n'):
            with self.assertRaisesRegex(ValueError, 'declare one numeric pkgrel'):
                overlay_release.pacman_recipe(recipe)
        self.assertIn("pkgrel='" + overlay_release.pacman_release('2.3') + "' # upstream",
                      overlay_release.pacman_recipe("pkgrel='2.3' # upstream\n"))

    @unittest.skipUnless(shutil.which('dpkg') and shutil.which('vercmp'),
                         'Native version comparators required')
    def test_native_versions_increase_for_attempts_and_runs(self):
        arch, debian = '2.3-1.9', '1:2.3-1+sunuefi2'
        for generation in ('9.1', '9.2', '9.10', '10.1', '44170111681.1'):
            os.environ[overlay_release.ENVIRONMENT] = generation
            new_arch = '2.3-' + overlay_release.pacman_release('1.9')
            new_debian = overlay_release.debian_version('1:2.3-1+sunuefi2')
            self.assertEqual(subprocess.check_output(['vercmp', arch, new_arch], text=True).strip(), '-1')
            subprocess.run(['dpkg', '--compare-versions', debian, 'lt', new_debian], check=True)
            self.assertTrue(overlay_release.uses_pacman_generation(new_arch))
            self.assertTrue(overlay_release.uses_debian_generation(new_debian))
            self.assertFalse(overlay_release.uses_pacman_generation(arch))
            self.assertFalse(overlay_release.uses_debian_generation(debian))
            arch, debian = new_arch, new_debian

    def test_fresh_debian_export_rejects_unrevised_builds_except_stock_fastrpc(self):
        os.environ[overlay_release.ENVIRONMENT] = '42.2'
        stock = {'component': 'sensors', 'name': 'libfastrpc1', 'version': '1'}
        overlay_packages.verify_debian_generation({'stock.deb': (None, stock)})
        rebuilt = {'component': 'sensors', 'name': 'libssc2', 'version': '1+piano1'}
        with self.assertRaisesRegex(ValueError, 'no current publication revision'):
            overlay_packages.verify_debian_generation({'rebuilt.deb': (None, rebuilt)})
        rebuilt['version'] = overlay_release.debian_version(rebuilt['version'])
        overlay_packages.verify_debian_generation({'rebuilt.deb': (None, rebuilt)})


class NativeOverlayUpgradeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-overlay-upgrade-')
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.folder.chmod(0o755)
        self.enterContext(patch.dict(os.environ))
        os.environ.pop(overlay_release.ENVIRONMENT, None)

    @unittest.skipUnless(os.geteuid() == 0 and all(shutil.which(tool) for tool in
                                                 ('dpkg', 'dpkg-query', 'dpkg-deb')),
                         'An isolated native dpkg install requires root and Debian tools')
    def test_debian_upgrade_installs_both_version_linked_packages(self):
        root = self.folder / 'debian'
        database = root / 'var/lib/dpkg'
        database.mkdir(parents=True)
        (database / 'status').touch()
        for run, generation in enumerate((None, '52.2')):
            if generation:
                os.environ[overlay_release.ENVIRONMENT] = generation
            version = overlay_release.debian_version('1:2.3-1+sunuefi2')
            packages = []
            for name in ('test-overlay-lib', 'test-overlay-app'):
                tree = self.folder / f'{name}-{run}'
                control = tree / 'DEBIAN/control'
                control.parent.mkdir(parents=True)
                depends = f'Depends: test-overlay-lib (= {version})\n' if name.endswith('-app') else ''
                control.write_text(f'Package: {name}\nVersion: {version}\nArchitecture: all\n'
                                   f'Maintainer: Test <test@example.invalid>\n{depends}'
                                   'Description: Publication version fixture\n')
                payload = tree / 'usr/share' / name
                payload.parent.mkdir(parents=True)
                payload.write_text(version)
                archive = self.folder / f'{name}_{version.split(":", 1)[-1]}_all.deb'
                subprocess.run(['dpkg-deb', '--build', '--root-owner-group', tree, archive],
                               check=True, capture_output=True)
                packages.append(archive)
            subprocess.run(['dpkg', f'--root={root}', '--install', *packages],
                           check=True, capture_output=True)
            installed = subprocess.check_output(
                ['dpkg-query', f'--admindir={database}', '-W', '-f=${Version}\n',
                 'test-overlay-app', 'test-overlay-lib'], text=True).splitlines()
            self.assertEqual(installed, [version, version])

    @unittest.skipUnless(os.geteuid() == 0 and all(shutil.which(tool) for tool in
                                                 ('makepkg', 'fakeroot', 'runuser', 'pacman',
                                                  'bsdtar', 'gpg')),
                         'An isolated signed pacman install requires root and native tools')
    def test_makepkg_upgrade_installs_both_version_linked_split_packages(self):
        root = self.folder / 'arch'
        root.mkdir()
        keyring = self.folder / 'gnupg'
        keyring.mkdir(mode=0o700)
        gpg = ['gpg', '--homedir', keyring, '--batch']
        subprocess.run([*gpg, '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Package fixture <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        config = self.folder / 'pacman.conf'
        config.write_text('[options]\nArchitecture = aarch64\nSigLevel = Required\n')
        database = self.folder / 'pacman-db'
        database.mkdir()
        pacman = ['pacman', '--config', config, '--root', root, '--dbpath', database,
                  '--gpgdir', keyring, '--logfile', self.folder / 'pacman.log']
        user = pwd.getpwnam('nobody')
        recipe = """pkgname=(test-overlay-lib test-overlay-app)
pkgver=2.3
pkgrel=1.9
pkgdesc='Publication version fixture'
arch=(any)
license=(MIT)
options=(!strip !debug)

package_test-overlay-lib() {
  install -Dm644 /dev/null "$pkgdir/usr/share/test-overlay-lib"
}

package_test-overlay-app() {
  depends=("test-overlay-lib=$pkgver-$pkgrel")
  install -Dm644 /dev/null "$pkgdir/usr/share/test-overlay-app"
}
"""
        for run, generation in enumerate((None, '52.2')):
            if generation:
                os.environ[overlay_release.ENVIRONMENT] = generation
            work = self.folder / f'makepkg-{run}'
            work.mkdir()
            path = work / 'PKGBUILD'
            path.write_text(recipe)
            path.write_bytes(build_arch_package.prepared_recipe(path))
            for entry in (work, path):
                os.chown(entry, user.pw_uid, user.pw_gid)
            result = subprocess.run(['runuser', '-u', user.pw_name, '--',
                                     'makepkg', '--nodeps', '--nocheck', '--noconfirm'],
                                    cwd=work, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            packages = sorted(work.glob('*.pkg.tar.*'))
            version = '2.3-' + overlay_release.pacman_release('1.9')
            expected = {('test-overlay-lib', version, 'any'), ('test-overlay-app', version, 'any')}
            self.assertEqual({overlay_packages.pacman_identity(path) for path in packages}, expected)
            for package in packages:
                subprocess.run([*gpg, '--detach-sign', package], check=True, capture_output=True)
            transaction = subprocess.run([*pacman, '-U', '--noconfirm', *packages],
                                         capture_output=True, text=True)
            self.assertEqual(transaction.returncode, 0, transaction.stdout + transaction.stderr)
            installed = subprocess.check_output([*pacman, '-Q'], text=True).splitlines()
            self.assertEqual(set(installed), {f'{name} {version}' for name, _, _ in expected})


if __name__ == '__main__':
    unittest.main()
