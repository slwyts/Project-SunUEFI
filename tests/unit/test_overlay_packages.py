"""Validate sealed overlay package sets with the native package tools."""
import io
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import overlay_packages as overlays
from desktop import build_libcamera_packages as libcamera
from desktop.build_pipewire_packages import DEBIAN_PACKAGES
from desktop.build_plasma_camera_package import RUNTIME as PLASMA_PACKAGES


@unittest.skipUnless(shutil.which('dpkg-deb'), 'Debian package tools required')
class OverlayPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-overlays-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_common_device_configuration_matches_the_sealed_native_deb(self):
        output = self.root / 'bsp'
        archive, metadata = overlays.build_bsp(output, 'debian')
        self.assertEqual(metadata['name'], 'piano-device-config')
        self.assertEqual(metadata['architecture'], 'all')
        self.assertEqual(overlays.bsp_package(output, 'debian'), (archive, metadata))
        with archive.open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'archive differs'):
            overlays.bsp_package(output, 'debian')

    def deb(self, name, version='1', *, architecture='arm64', payload=None, directory=None):
        package = self.root / 'payload'
        package.mkdir()
        control = package / 'DEBIAN/control'
        control.parent.mkdir()
        control.write_text(
            f'Package: {name}\nVersion: {version}\nArchitecture: {architecture}\n'
            'Maintainer: Package build test <test@example.invalid>\nDescription: Overlay fixture\n')
        if payload:
            (package / 'usr/share').mkdir(parents=True)
            (package / 'usr/share/fixture').write_text(payload)
        archive = (directory or self.root) / f'{name}_{version}_{architecture}.deb'
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', package, archive],
                       check=True, capture_output=True)
        shutil.rmtree(package)
        return archive

    def test_exports_only_validated_files_and_selects_only_kde_components(self):
        sources = {}
        policy = libcamera.policy()
        abi = libcamera.abi_version(policy)
        for component in (*overlays.DEBIAN_SHARED, *overlays.DEBIAN_GNOME, 'fingerprint'):
            names = {'libcamera': libcamera.runtime_packages(policy), 'pipewire': DEBIAN_PACKAGES,
                     'plasma_camera': PLASMA_PACKAGES}.get(
                         component, {'test-' + component.replace('_', '-'): 'arm64'})
            version = libcamera.base_package_version(policy) if component == 'libcamera' else '1'
            records = {}
            for name, architecture in names.items():
                archive = self.deb(name, version, architecture=architecture)
                records[name] = {'file': archive.name, 'version': version,
                                 'architecture': architecture, 'sha256': overlays.sha(archive)}
            sources[component] = (self.root, {'packages': records, 'package_version': version})
        dependencies = {name: {key: value for key, value in row.items() if key != 'file'}
                        for name, row in sources['libcamera'][1]['packages'].items()
                        if name in (f'libcamera{abi}', 'libcamera-dev', f'libcamera-ipa-{abi}')}
        for component in ('pipewire', 'plasma_camera'):
            sources[component][1]['build_dependencies'] = dependencies
        unlisted = self.deb('test-obsolete')
        output = self.root / 'set'
        expected = overlays.debian_packages(sources)
        overlays.export(output, 'debian-gnome', 'apt', expected,
                        {name: value for name, (_, value) in sources.items()})
        manifest = output / 'manifest.json'
        _, gnome = overlays.read_debian(manifest, 'gnome')
        _, kde = overlays.read_debian(manifest, 'kde')
        self.assertEqual(set(gnome), set(expected))
        self.assertNotIn(unlisted.name, gnome)
        self.assertEqual({row['component'] for row in kde.values()},
                         {*overlays.DEBIAN_SHARED, 'fingerprint'})
        self.assertIn(dependencies[f'libcamera-ipa-{abi}']['sha256'],
                      {row['sha256'] for row in kde.values()})

        # Matching versions are insufficient: an older dev archive can have
        # the same version string and different compiler inputs.
        record = json.loads(manifest.read_text())
        record['components']['plasma_camera']['build_dependencies']['libcamera-dev']['sha256'] = 'a' * 64
        manifest.write_text(json.dumps(record))
        for desktop in ('gnome', 'kde'):
            with self.subTest(desktop=desktop), self.assertRaisesRegex(
                    ValueError, 'plasma_camera was not built with this libcamera package set'):
                overlays.read_debian(manifest, desktop)

        name = 'libpipewire-0.3-modules'
        _, pipewire = sources['pipewire']
        removed = pipewire['packages'].pop(name)['file']
        incomplete = {filename: row for filename, row in expected.items() if filename != removed}
        out = self.root / 'incomplete'
        with self.assertRaisesRegex(ValueError, 'all native packages'):
            overlays.export(out, 'debian-gnome', 'apt', incomplete,
                            {name: value for name, (_, value) in sources.items()})
        self.assertFalse(out.exists())

        out = self.root / 'partial'
        path, row = expected[next(iter(expected))]
        path.write_bytes(path.read_bytes() + b'changed after checking')
        with self.assertRaisesRegex(ValueError, 'Package changed'):
            overlays.export(out, 'debian-gnome', 'apt', expected)
        self.assertFalse(out.exists())

    def test_native_camera_defaults_include_ipa_without_optional_tools(self):
        abi = libcamera.abi_version(libcamera.policy())
        gnome = overlays.debian_package_selection('gnome', {'libcamera-tools'})
        kde = overlays.debian_package_selection('kde', set())
        default = {f'libcamera{abi}', f'libcamera-ipa-{abi}', 'gstreamer1.0-libcamera'}
        self.assertEqual(kde['libcamera'], default)
        self.assertEqual(gnome['libcamera'], default | {'libcamera-tools'})
        self.assertIn('libspa-0.2-libcamera', gnome['pipewire'])
        self.assertEqual(gnome['plasma_camera'] & PLASMA_PACKAGES.keys(), set())
        self.assertTrue(PLASMA_PACKAGES.keys() <= kde['plasma_camera'])

    @unittest.skipUnless(all(shutil.which(name) for name in
                            ('apt-get', 'apt-ftparchive', 'dpkg-scanpackages', 'gpg')),
                         'APT and GnuPG tools required')
    def test_actual_apt_download_authenticates_and_uses_selected_repository(self):
        self.root.chmod(0o755)
        key = self.root / 'keyring.gpg'
        gnupg = self.root / 'gnupg'
        gnupg.mkdir(mode=0o700)
        gpg = ['gpg', '--homedir', gnupg, '--batch']
        subprocess.run([*gpg, '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Test Archive <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        key.write_bytes(subprocess.check_output([*gpg, '--export']))
        sources = self.root / 'etc/apt/sources.list.d'
        download = self.root / 'var/tmp/download'
        sources.mkdir(parents=True)
        download.mkdir(parents=True)
        (self.root / 'var/lib/apt/lists/partial').mkdir(parents=True)
        (self.root / 'var/lib/dpkg').mkdir(parents=True)
        (self.root / 'var/lib/dpkg/status').touch()
        config = self.root / 'apt.conf'
        config.write_text(f'Dir "{self.root}";\nAPT::Architecture "arm64";\n'
                          'APT::Architectures { "arm64"; };\nDebug::NoLocking "true";\n')
        env = {**os.environ, 'APT_CONFIG': str(config)}

        for label in ('debian', 'sunuefi'):
            repository = self.root / label
            repository.mkdir()
            archive = self.deb('test-overlay', payload=label, directory=repository)
            (repository / 'Packages').write_bytes(subprocess.check_output(
                ['dpkg-scanpackages', '.', '/dev/null'], cwd=repository, stderr=subprocess.PIPE))
            release = repository / 'Release'
            release.write_bytes(subprocess.check_output(
                ['apt-ftparchive', 'release', '.'], cwd=repository))
            subprocess.run([*gpg, '--output', repository / 'InRelease', '--clearsign', release],
                           check=True)
            (sources / (label + '.sources')).write_text(
                f'Types: deb\nURIs: {repository.as_uri()}\nSuites: ./\nSigned-By: {key}\n')
        subprocess.run(['apt-get', '-o', 'APT::Update::Error-Mode=any', 'update'],
                       env=env, check=True, capture_output=True)
        packages = {archive.name: {'name': 'test-overlay', 'version': '1', 'architecture': 'arm64',
                                   'sha256': overlays.sha(archive)}}

        def run(args, **kwargs):
            # Use the same download command and APT configuration as an image
            # build, mapping its chroot path onto the isolated test APT tree.
            if args[2:] == ['getent', 'passwd', '_apt']:
                user = '_apt' if os.geteuid() == 0 else pwd.getpwuid(os.getuid()).pw_name
                return subprocess.run(['getent', 'passwd', user], check=True, **kwargs)
            return subprocess.run([*args[2:6], str(self.root / args[6].lstrip('/')), *args[7:]],
                                  env=env, check=True, capture_output=True)

        paths = overlays.download_debian(self.root, download, packages, run)
        self.assertEqual(len(paths), 1)
        self.assertEqual(overlays.sha(self.root / paths[0].lstrip('/')), overlays.sha(archive))
        packages[archive.name]['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'differs from the published generation'):
            overlays.download_debian(self.root, download, packages, run)


@unittest.skipUnless(os.geteuid() == 0 and all(shutil.which(name) for name in
                                             ('pacman', 'repo-add', 'bsdtar', 'gpg')),
                     'Isolated pacman installation requires root and package tools')
class PacmanOverlayTests(unittest.TestCase):
    def test_actual_pacman_installs_only_the_selected_signed_generation(self):
        with tempfile.TemporaryDirectory(prefix='sunuefi-pacman-overlays-') as temporary:
            directory = Path(temporary)
            rootfs = directory / 'rootfs'
            rootfs.mkdir()
            cache = rootfs / 'cache'
            cache.mkdir()
            opaque = directory / 'signing'
            opaque.mkdir(mode=0o700)
            env = {**os.environ, 'GNUPGHOME': str(opaque)}
            subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                            '--quick-generate-key', 'Overlay Test <test@example.invalid>',
                            'ed25519', 'sign', '1d'], env=env, check=True, capture_output=True)
            repo = directory / 'repo'
            repo.mkdir()
            archive = repo / 'test-overlay-1-1-any.pkg.tar.gz'
            metadata = (b'pkgname = test-overlay\npkgbase = test-overlay\npkgver = 1-1\n'
                        b'pkgdesc = test-overlay\nurl = https://example.invalid\n'
                        b'builddate = 1\npackager = Test\nsize = 7\narch = any\nlicense = MIT\n')
            with tarfile.open(archive, 'w:gz') as package:
                for name, data in (('.PKGINFO', metadata), ('usr/share/overlay', b'testing')):
                    member = tarfile.TarInfo(name)
                    member.mode, member.size = 0o644, len(data)
                    package.addfile(member, io.BytesIO(data))
            subprocess.run(['gpg', '--batch', '--detach-sign', archive], env=env, check=True)
            subprocess.run(['repo-add', '--sign', '--include-sigs',
                            repo / 'sunuefi.db.tar.zst', archive], env=env, check=True, capture_output=True)
            config = directory / 'pacman.conf'
            config.write_text('[options]\nArchitecture = aarch64\nSigLevel = Required\n'
                              f'[sunuefi]\nServer = {repo.as_uri()}\n')
            command = ['pacman', '--config', config, '--root', rootfs, '--dbpath', directory / 'db',
                       '--gpgdir', opaque, '--logfile', directory / 'pacman.log']
            (directory / 'db').mkdir()
            subprocess.run([*command, '-Sy', '--noconfirm'], check=True, capture_output=True)
            packages = {archive.name: {'name': 'test-overlay', 'version': '1-1',
                                       'architecture': 'any', 'sha256': '0' * 64}}

            def guest(args):
                args = [cache if arg == '/cache' else arg for arg in args[1:]]
                subprocess.run([*command, *args], check=True, capture_output=True)

            with self.assertRaisesRegex(ValueError, 'differs from the published generation'):
                overlays.install_pacman(rootfs, cache, 'sunuefi', packages, guest)
            self.assertFalse((rootfs / 'usr/share/overlay').exists())
            packages[archive.name]['sha256'] = overlays.sha(archive)
            overlays.install_pacman(rootfs, cache, 'sunuefi', packages, guest)
            self.assertEqual((rootfs / 'usr/share/overlay').read_text(), 'testing')
            listing = subprocess.check_output([*command, '-Q'], text=True)
            overlays.verify_pacman_versions(packages, listing)
            packages[archive.name]['version'] = '2-1'
            with self.assertRaisesRegex(ValueError, r'test-overlay must be 2-1, found 1-1'):
                overlays.verify_pacman_versions(packages, listing)


if __name__ == '__main__':
    unittest.main()
