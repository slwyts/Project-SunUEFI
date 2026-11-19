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

    def test_arch_and_debian_apply_the_same_reviewed_patches(self):
        policy = json.loads(builder.CONFIG.read_text())['arch']
        upstream, _ = policy['upstream_package_version'].rsplit('-', 1)
        series = ROOT / 'patches/plasma-camera'
        debian = builder.source_patches(series / '26.08.2/series.json')
        arch = builder.source_patches(series / upstream / 'series.json')
        self.assertEqual([path.resolve() for path in arch], [path.resolve() for path in debian])


if __name__ == '__main__':
    unittest.main()
