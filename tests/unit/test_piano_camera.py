"""Build the maintained camera sources and execute their real native checks."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_runtime_helpers as runtime


class PianoCameraTests(unittest.TestCase):
    def test_shared_camera_make_checks(self):
        # The ordinary UEFI unit suite also runs without the optional camera
        # development toolchain. Full camera builds run the same Make checks.
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        commands = [compiler, shlex.split(os.environ.get('CC', 'cc')),
                    ['make'], ['cargo'], ['pkg-config']]
        if any(not command or not shutil.which(command[0]) for command in commands):
            self.skipTest('Native camera compiler, Cargo or pkg-config unavailable')
        item = json.loads((ROOT / 'patches/demosaic/0.3.0/series.json').read_text())['source']
        archive = ROOT / 'build/source-cache/camera' / (item['pname'] + '-' + item['version'] + '.crate')
        if not archive.exists():
            self.skipTest('Camera crate not cached; release-rootfs prepares it')
        jpeg = subprocess.run(['pkg-config', '--cflags', '--libs', 'libjpeg'],
                              capture_output=True, text=True)
        if jpeg.returncode:
            self.skipTest('Native libjpeg development files unavailable')
        with tempfile.TemporaryDirectory(prefix='piano-camera-') as temporary:
            work = Path(temporary)
            flags = shlex.split(os.environ.get('YUV_CFLAGS', ''))
            libraries = shlex.split(os.environ.get('YUV_LIBS', '-lyuv'))
            dependency = subprocess.run(
                [*compiler, '-x', 'c++', '-', *flags, *libraries,
                 *shlex.split(jpeg.stdout), '-o', str(work / 'dependencies')],
                input='#include <stdio.h>\n#include <jpeglib.h>\n#include <libyuv.h>\nint main() { return 0; }\n',
                capture_output=True, text=True)
            if dependency.returncode:
                self.skipTest('Native libyuv/libjpeg development files unavailable')
            source = work / 'camera'
            shutil.copytree(ROOT / 'linux/userspace/camera', source,
                            ignore=shutil.ignore_patterns('vendor', 'target'))
            runtime.prepare_camera_quad(source, work, os.environ.copy())
            # Native checks use the host target. Production runtime builds
            # select aarch64 and target libraries through this same Makefile.
            command = ['make', '-C', source, 'check', 'OUT=' + str(work / 'bin'),
                       'YUV_CFLAGS=' + os.environ.get('YUV_CFLAGS', ''),
                       'YUV_LIBS=' + os.environ.get('YUV_LIBS', '-lyuv')]
            subprocess.run(command, check=True)


if __name__ == '__main__':
    unittest.main()
