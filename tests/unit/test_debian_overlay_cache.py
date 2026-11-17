"""Verify cached overlay archives against a real, signed rolling APT source."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build import overlay_cache, overlay_packages, overlay_release
from desktop import debian_package


@unittest.skipUnless(os.geteuid() == 0 and all(shutil.which(tool) for tool in
                                             ('apt-get', 'apt-ftparchive', 'dpkg-deb',
                                              'dpkg-scanpackages', 'gpg', 'gpgv')),
                     'An isolated APT state and chroot require root and Debian tools')
class DebianOverlayCacheTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-debian-overlay-cache-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.root.chmod(0o755)
        self.project = self.root / 'project'
        self.project.mkdir()
        subprocess.run(['git', '-C', self.project, 'init', '-q'], check=True)
        self.recipe = self.project / 'tools/recipe.py'
        self.recipe.parent.mkdir()
        self.recipe.write_text('package recipe\n')
        subprocess.run(['git', '-C', self.project, 'add', 'tools/recipe.py'], check=True)
        gnupg = self.root / 'gnupg'
        gnupg.mkdir(mode=0o700)
        self.enterContext(patch.dict(os.environ, {
            'GNUPGHOME': str(gnupg),
            'SUNUEFI_BUILD_ENVIRONMENT_ID': 'sha256:test-builder',
        }))
        subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Test Archive <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        keyring = self.root / 'archive-keyring.gpg'
        keyring.write_bytes(subprocess.check_output(['gpg', '--export']))
        self.enterContext(patch.object(debian_package, 'KEYRING', str(keyring)))

        repo = self.root / 'repo'
        self.release = repo / 'dists/sid'
        self.packages = self.release / 'main/binary-arm64/Packages'
        self.sources = self.release / 'main/source/Sources'
        self.packages.parent.mkdir(parents=True)
        self.sources.parent.mkdir(parents=True)
        package = repo / 'pool/main/fixture_1_arm64.deb'
        package.parent.mkdir(parents=True)
        control = self.root / 'archive-fixture/DEBIAN/control'
        control.parent.mkdir(parents=True)
        control.write_text('Package: fixture\nVersion: 1\nArchitecture: arm64\n'
                           'Maintainer: Fixture <test@example.invalid>\nDescription: Signed fixture\n')
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', control.parents[1], package],
                       check=True, capture_output=True)
        self.packages.write_bytes(subprocess.check_output(
            ['dpkg-scanpackages', 'pool', '/dev/null'], cwd=repo, stderr=subprocess.PIPE))
        self.lists = self.root / 'lists'
        self.lists.mkdir()
        sources = self.root / 'debian.list'
        sources.write_text(''.join(f'{kind} [signed-by={keyring}] {repo.as_uri()} sid main\n'
                                   for kind in ('deb', 'deb-src')))
        self.status = self.root / 'status'
        self.install_base(1)
        parts = self.root / 'apt.conf.d'
        parts.mkdir()
        main = self.root / 'apt-defaults.conf'
        main.touch()
        config = self.root / 'apt.conf'
        config.write_text(
            f'Dir::Etc::parts "{parts}";\nDir::Etc::main "{main}";\n'
            f'Dir::Etc::sourcelist "{sources}";\nDir::Etc::sourceparts "-";\n'
            f'Dir::State::lists "{self.lists}";\nDir::State::status "{self.status}";\n'
            f'Dir::Cache "{self.root}/cache";\n'
            'APT::Architecture "arm64";\nAPT::Architectures { "arm64"; };\n'
            'Acquire::Languages "none";\nAcquire::GzipIndexes "false";\n'
            'Debug::NoLocking "true";\n')
        self.enterContext(patch.dict(os.environ, APT_CONFIG=str(config), DPKG_ADMINDIR=str(self.root)))
        self.publish(source_version=1)
        self.update()

    def install_base(self, version):
        self.status.write_text('Package: cache-base\nStatus: install ok installed\n'
                               'Priority: optional\nSection: misc\nArchitecture: arm64\n'
                               f'Version: {version}\nMaintainer: Archive Fixture <test@example.invalid>\n'
                               'Description: Installed fixture\n')

    def publish(self, source_version, release_revision=None):
        self.sources.write_text(f'Package: fixture\nVersion: {source_version}\nDirectory: pool/main\n')
        release = self.release / 'Release'
        release.unlink(missing_ok=True)
        arguments = ['apt-ftparchive']
        for key, value in {'Origin': 'Debian', 'Suite': 'unstable', 'Codename': 'sid',
                           'Architectures': 'arm64', 'Components': 'main'}.items():
            arguments.extend(('-o', f'APT::FTPArchive::Release::{key}={value}'))
        revision = release_revision or source_version
        arguments.extend(('-o', f'APT::FTPArchive::Release::Description=Fixture revision {revision}'))
        release.write_bytes(subprocess.check_output([*arguments, 'release', '.'], cwd=self.release))
        signed = self.release / 'InRelease'
        subprocess.run(['gpg', '--batch', '--yes', '--output', signed, '--clearsign', release],
                       check=True, capture_output=True)
        # APT's file transport checks integer modification times on refresh.
        modified = 1730000000 + revision
        os.utime(signed, (modified, modified))

    def update(self):
        subprocess.run(['apt-get', '-o', 'APT::Update::Error-Mode=any', 'update'],
                       check=True, capture_output=True, text=True)
        # file: can leave symlinks to the mirror, whereas a network download
        # keeps a separate copy until the next APT refresh.
        for path in self.lists.iterdir():
            if path.is_symlink():
                data = path.read_bytes()
                path.unlink()
                path.write_bytes(data)

    def build_cache(self, inputs):
        archives = self.root / 'archives'
        archives.mkdir()
        sources = {}
        for component in (*overlay_packages.DEBIAN_SHARED, *overlay_packages.DEBIAN_GNOME,
                          'fingerprint'):
            name = 'test-' + component.replace('_', '-')
            control = self.root / 'payload/DEBIAN/control'
            control.parent.mkdir(parents=True, exist_ok=True)
            control.write_text(f'Package: {name}\nVersion: 1\nArchitecture: arm64\n'
                               'Maintainer: Fixture <test@example.invalid>\nDescription: Overlay fixture\n')
            path = archives / f'{name}_1_arm64.deb'
            subprocess.run(['dpkg-deb', '--build', '--root-owner-group', control.parents[1], path],
                           check=True, capture_output=True)
            sources[component] = (archives, {'packages': {name: {
                'file': path.name, 'version': '1', 'architecture': 'arm64',
                'sha256': overlay_packages.sha(path),
            }}})
        output = self.project / 'build/overlay-cache/debian-gnome'
        overlay_packages.export(output, 'debian-gnome', 'apt',
                                overlay_packages.debian_packages(sources),
                                {name: metadata for name, (_, metadata) in sources.items()},
                                build_inputs=inputs)
        return output

    def inputs(self, uapi='a' * 64):
        # Use the native container's APT binaries at /, with all repository
        # configuration and mutable APT state directed into this fixture.
        return overlay_cache.debian_inputs(self.project, Path('/'), uapi)

    def test_cache_matches_signed_inputs_and_complete_native_archives(self):
        inputs = self.inputs()
        cached = self.build_cache(inputs)
        restored = self.root / 'restored'
        self.publish(source_version=1, release_revision=2)
        self.update()
        with patch.dict(os.environ, {overlay_release.ENVIRONMENT: '45.2'}):
            result = overlay_cache.restore_debian(self.project, restored, 'debian-gnome', self.inputs())
        self.assertEqual(result['build_inputs'], inputs)
        self.assertEqual(set(result['packages']), {path.name for path in cached.glob('*.deb')})
        for name in result['packages']:
            self.assertEqual(overlay_packages.sha(restored / name), overlay_packages.sha(cached / name))

        output = self.root / 'not-restored'
        self.assertIsNone(overlay_cache.restore_debian(self.project, output, 'debian-gnome',
                                                       self.inputs('b' * 64)))
        self.install_base(2)
        self.assertNotEqual(self.inputs()['installed'], inputs['installed'])
        self.assertIsNone(overlay_cache.restore_debian(self.project, output, 'debian-gnome',
                                                       self.inputs()))
        package = next(cached.glob('*.deb'))
        package.write_bytes(package.read_bytes() + b'changed bytes')
        self.assertIsNone(overlay_cache.restore_debian(self.project, output, 'debian-gnome', inputs))
        self.assertFalse(output.exists())

    def test_child_cannot_select_a_new_signed_generation_in_the_same_build(self):
        selected = self.inputs()
        self.build_cache(selected)
        self.publish(source_version=2)

        with debian_package.use_build_archive(selected['archive']):
            def unexpected_refresh(_):
                self.fail('A child builder must not refresh the selected APT indexes')

            debian_package.prepare_build_indexes(Path('/'), unexpected_refresh)
            self.update()  # An out-of-band refresh must still invalidate the build.
            with self.assertRaisesRegex(ValueError, 'differ from the selected signed sid archive'):
                debian_package.verify_build_archive(Path('/'))

        changed = self.inputs()
        self.assertEqual(changed['installed'], selected['installed'])
        for identifier, equal in (('Packages', True), ('Sources', False)):
            old, = selected['archive']['indexes'][identifier]
            new, = changed['archive']['indexes'][identifier]
            self.assertEqual(old['sha256'] == new['sha256'], equal)
        self.assertIsNone(overlay_cache.restore_debian(
            self.project, self.root / 'not-restored', 'debian-gnome', changed))

    def test_corrupted_apt_index_does_not_produce_an_input_identity(self):
        packages, = self.lists.glob('*_binary-arm64_Packages')
        packages.write_text(packages.read_text().replace('Version: 1', 'Version: 2'))
        with self.assertRaisesRegex(ValueError, 'differs from its signed InRelease'):
            self.inputs()


if __name__ == '__main__':
    unittest.main()
