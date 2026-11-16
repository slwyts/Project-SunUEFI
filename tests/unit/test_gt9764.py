"""Run the maintained GT9764 driver's movement and failure paths without hardware."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Gt9764Tests(unittest.TestCase):
    def test_position_parking_controls_and_power_failures(self):
        if not shutil.which('cc') or not shutil.which('git'):
            self.skipTest('A C compiler and Git are required')
        folder = ROOT / 'patches/linux/7.3'
        series = json.loads((folder / 'series.json').read_text())
        patch = next(row for row in series['patches']
                     if row['file'].endswith('-media-gt9764ba-lens.patch'))
        source_patch = folder / patch['file']
        self.assertEqual(hashlib.sha256(source_patch.read_bytes()).hexdigest(), patch['sha256'])
        with tempfile.TemporaryDirectory(prefix='gt9764-') as directory:
            work = Path(directory)
            subprocess.run(['git', 'apply', '--include=drivers/media/i2c/gt9764.c',
                            source_patch], cwd=work, check=True)
            source = (work / 'drivers/media/i2c/gt9764.c').read_text()
            # Compile unchanged production functions. Registration is covered
            # by the normal ARM64 module build, not duplicated in host stubs.
            begin = source.index('#define GT9764_CONTROL')
            end = source.index('static const struct v4l2_ctrl_ops gt9764_ctrl_ops')
            (work / 'gt9764-driver.h').write_text(source[begin:end])
            executable = work / 'gt9764'
            subprocess.run([
                'cc', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(work), ROOT / 'tests/native/GT9764Test.c', '-o', executable,
            ], check=True)
            subprocess.run([executable], check=True)


if __name__ == '__main__':
    unittest.main()
