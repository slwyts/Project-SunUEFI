"""Check native camera module packaging against the exact kernel ABI."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

from package import v4l2loopback


class V4l2loopbackTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable')
    def test_rejects_real_deb_for_a_different_kernel(self):
        release = '7.3.3-piano-test'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            control = output / 'kernel' / 'DEBIAN'
            control.mkdir(parents=True)
            (control / 'control').write_text(
                'Package: linux-image-7.3.2-piano-test\nVersion: 7.3.2-1\nArchitecture: arm64\n'
                'Maintainer: Linux Package Test <test@example.invalid>\n'
                'Description: Kernel identity test\n')
            kernel = output / 'kernel.deb'
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', control.parent, kernel],
                           capture_output=True, check=True)
            with self.assertRaisesRegex(ValueError, 'does not match the camera module ABI'):
                v4l2loopback.build_package({'format': 'debian', 'kernel_release': release},
                                          kernel, {})

    def test_rejects_path_like_kernel_release(self):
        for release in ('..', '7.3.3/other', ''):
            with self.subTest(release=release), self.assertRaisesRegex(ValueError, 'kernel release'):
                v4l2loopback.module_path(release)


if __name__ == '__main__':
    unittest.main()
