"""Check the native archives and their service and configuration metadata."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

from package import camera


@unittest.skipUnless(shutil.which('aarch64-linux-gnu-as') and
                     shutil.which('aarch64-linux-gnu-ld'), 'ARM64 binutils required')
class CameraPackageTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='sunuefi-camera-package-')
        self.addCleanup(folder.cleanup)
        self.work = Path(folder.name)
        self.work.chmod(0o755)
        self.output = self.work / 'archives'
        self.recipe = self.work / 'recipe'
        built = self.work / 'build'
        built.mkdir()
        self.build = built
        subprocess.run(['aarch64-linux-gnu-as', '-o', built / 'example.o'], check=True,
                       input='.global _start\n_start:\n mov x0, #0\n mov x8, #93\n svc #0\n',
                       text=True)
        binary = built / 'example'
        subprocess.run(['aarch64-linux-gnu-ld', '-static', '-o', binary, built / 'example.o'],
                       check=True)
        license_file = built / 'LICENSE-MIT'
        shutil.copyfile(ROOT / 'linux/userspace/camera/LICENSE-MIT', license_file)
        self.files = {}
        for name in camera.CAMERA_RUNTIME_PATHS:
            is_license = name.startswith('usr/share/licenses/')
            path = license_file if is_license else binary
            self.files[name] = {'file': path.name, 'sha256': camera.sha(path),
                                'bytes': path.stat().st_size, 'mode': 0o644 if is_license else 0o755}

    def build_archive(self, fmt):
        return camera.build_package(fmt, self.output, self.recipe, self.build, self.files,
                                    version='1.0', revision='2', epoch=1_783_000_000,
                                    kernel_version='7.3.3+123-1')

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                             ('dpkg-deb', 'dh_gencontrol', 'dh_installsystemd', 'dh_installdeb')),
                         'Debian packaging helpers required')
    def test_debian_archive_keeps_config_and_uses_service_lifecycle(self):
        archive, _, _ = self.build_archive('debian')
        control = self.work / 'control'
        subprocess.run(['dpkg-deb', '-e', archive, control], check=True)
        self.assertIn('linux-image-piano (>= 7.3.3+123-1)', (control / 'control').read_text())
        self.assertIn('piano-device-config', (control / 'control').read_text())
        self.assertIn('deb-systemd-invoke', (control / 'postinst').read_text())
        self.assertIn('piano-camerad.service', (control / 'prerm').read_text())
        self.assertIn('dpkg-query --search', (control / 'preinst').read_text())
        self.assertEqual(set((control / 'conffiles').read_text().splitlines()),
                         {'/' + path for path in camera.configuration() if path.startswith('etc/')})
        self.assertNotIn('etc/piano/camera.conf', camera.archive_contents('debian', archive))

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('makepkg', 'bsdtar', 'zstd', 'fakeroot'))
                         and (os.geteuid() != 0 or Path('/.dockerenv').exists()
                              or bool(os.environ.get('SUDO_UID'))),
                         'makepkg and an unprivileged builder required')
    def test_pacman_archive_keeps_config_and_does_not_own_service_enablement(self):
        archive, _, _ = self.build_archive('pacman')
        metadata = subprocess.check_output(['bsdtar', '-xOf', archive, '.PKGINFO'], text=True)
        self.assertIn('depend = linux-piano>=7.3.3+123-1', metadata)
        self.assertIn('depend = piano-device-config', metadata)
        self.assertEqual({line.removeprefix('backup = ') for line in metadata.splitlines()
                          if line.startswith('backup = ')},
                         {path for path in camera.configuration() if path.startswith('etc/')})
        payload = camera.archive_contents('pacman', archive)
        self.assertIn('usr/lib/systemd/system/piano-camerad.service', payload)
        self.assertNotIn('etc/systemd/system/multi-user.target.wants/piano-camerad.service', payload)

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                             ('dpkg-deb', 'dh_gencontrol', 'dh_installsystemd', 'dh_installdeb')),
                         'Debian packaging helpers required')
    def test_restored_archive_must_match_current_inputs_and_checksum(self):
        archive, name, version = self.build_archive('debian')
        metadata = {'name': name, 'version': version, 'architecture': 'arm64',
                    'sha256': camera.sha(archive)}
        (self.output / 'manifest.json').write_text(json.dumps(
            {'schema_version': 1, 'build': 'inputs', 'package': metadata}))
        with patch.object(camera, 'policy', return_value={'version': '1.0', 'revision': '2'}):
            self.assertEqual(camera.shared_package('debian', self.output, digest='inputs'),
                             (archive, metadata))
            with self.assertRaisesRegex(ValueError, 'differs from the current build'):
                camera.shared_package('debian', self.output, digest='other')
            with archive.open('ab') as stream:
                stream.write(b'tampered package')
            with self.assertRaisesRegex(ValueError, 'differs from the current build'):
                camera.shared_package('debian', self.output, digest='inputs')

    def test_rejects_changed_camera_binary_before_packaging(self):
        binary = self.build / self.files['usr/lib/piano/piano-camerad']['file']
        with binary.open('ab') as stream:
            stream.write(b'changed after building')
        with self.assertRaisesRegex(ValueError, 'differs from the manifest'):
            self.build_archive('debian')
        self.assertFalse(self.output.exists())
        self.assertFalse(self.recipe.exists())
