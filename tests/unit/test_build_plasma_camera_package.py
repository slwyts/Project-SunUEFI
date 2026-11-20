"""Verify that Plasma Camera builds against the selected native camera packages."""
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import build_libcamera_packages, build_plasma_camera_package as builder


@unittest.skipUnless(shutil.which('bsdtar'), 'Native Arch package inspection needs bsdtar')
class PlasmaCameraArchTests(unittest.TestCase):
    def setUp(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        self.runtime = Path(directory) / 'runtime'
        self.runtime.mkdir(parents=True)
        self.root = Path(directory) / 'root'
        camera = build_libcamera_packages.policy()
        self.version = build_libcamera_packages.upstream_version(camera) + '-1'
        self.archives, self.proof = [], {}
        for name in ('libcamera', 'libcamera-ipa'):
            filename = f'{name}-{self.version}-aarch64.pkg.tar.gz'
            path = f'usr/lib/{name}.bin'
            archive = self.runtime / filename
            with tarfile.open(archive, 'w:gz') as tar:
                content = name.encode()
                info = tarfile.TarInfo(path)
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))
            installed = self.root / path
            installed.parent.mkdir(parents=True, exist_ok=True)
            installed.write_bytes(name.encode())
            self.archives.append(archive)
            self.proof[name] = {'version': self.version}

    def test_same_installed_version_is_not_enough_for_ipa(self):
        run = subprocess.run

        def query(command, *args, **kwargs):
            if command[0] == 'chroot':
                return subprocess.CompletedProcess(command, 0, stdout=''.join(
                    f'{name} {self.version}\n' for name in self.proof))
            return run(command, *args, **kwargs)

        with patch.object(builder.subprocess, 'run', side_effect=query):
            builder.verify_installed_arch_camera(self.root, self.archives, self.proof)
            (self.root / 'usr/lib/libcamera-ipa.bin').write_bytes(b'another build')
            with self.assertRaisesRegex(ValueError, 'differs from its selected build'):
                builder.verify_installed_arch_camera(self.root, self.archives, self.proof)

    def test_pacman_and_debian_apply_the_same_reviewed_patches(self):
        policy = json.loads(builder.CONFIG.read_text())['arch']
        upstream, _ = policy['upstream_package_version'].rsplit('-', 1)
        series = ROOT / 'patches/plasma-camera'
        pacman_series = series / upstream / 'series.json'
        debian = builder.source_patches(series / '26.08.2/series.json')
        pacman = builder.source_patches(pacman_series)
        self.assertEqual([path.resolve() for path in pacman], [path.resolve() for path in debian])
        self.assertIn('steamos', json.loads(pacman_series.read_text())['targets'])

    def test_steamos_selects_installed_qt_and_arch_checks_the_source_release(self):
        policy = json.loads(builder.CONFIG.read_text())
        with patch.object(builder.debian_package, 'capture', return_value='qt6-base 6.11.1-1') as query:
            steamos, minor = builder.pacman_recipe_policy(self.root, 'steamos')
        query.assert_called_once_with(['chroot', self.root, 'pacman', '-Q', 'qt6-base'])
        self.assertEqual(minor, '6.11')
        self.assertEqual(steamos['source'], policy['arch']['source'])
        self.assertEqual(steamos['package_release'], policy['steamos']['package_release'])

        version = policy['arch']['upstream_package_version']
        with patch.object(builder.debian_package, 'capture', return_value=f'Version : {version}') as query:
            self.assertEqual(builder.pacman_recipe_policy(self.root, 'arch'), (policy['arch'], None))
        query.assert_called_once_with(['chroot', self.root, 'env', 'LC_ALL=C', 'pacman', '-Si',
                                       'extra/plasma-camera'])
        with patch.object(builder.debian_package, 'capture', return_value='Version : 1-1'):
            with self.assertRaisesRegex(ValueError, 'selected Arch archive'):
                builder.pacman_recipe_policy(self.root, 'arch')


if __name__ == '__main__':
    unittest.main()
