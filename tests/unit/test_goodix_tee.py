"""Exercise the real Linux TEE object framing without opening a device."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixTeeTests(unittest.TestCase):
    def test_objects_memory_callbacks_and_ambiguous_transport_failure(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        qcbor = ROOT / 'build/source-cache/qcbor'
        if not compiler or not shutil.which(compiler[0]) or not (qcbor / 'inc').is_dir():
            self.skipTest('A native compiler and cached QCBOR source are required')
        with tempfile.TemporaryDirectory(prefix='goodix-tee-') as directory:
            binary = Path(directory) / 'test_goodix_tee'
            subprocess.run([
                *compiler, '-std=gnu17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-Wl,--wrap=ioctl', '-Wl,--wrap=open', '-I' + str(qcbor / 'inc'),
                ROOT / 'tests/native/test_goodix_tee.c', '-pthread', '-o', binary,
            ], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
