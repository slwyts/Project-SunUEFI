"""Exercise the real native Keymaster transport without opening any device."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixKeymasterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        includes = [
            ROOT / 'build/source-cache/minkipc/libminkadaptor/include',
            ROOT / 'build/source-cache/qcbor/inc',
        ]
        if not all(path.is_dir() for path in includes):
            raise unittest.SkipTest('Cached MinkIPC and QCBOR public headers are required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-keymaster-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_keymaster'
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            *[f'-I{path}' for path in includes],
            str(ROOT / 'tests/native/test_goodix_keymaster.c'),
            '-o', str(cls.binary),
        ], check=True)

    def check_reply(self, name):
        result = subprocess.run([str(self.binary), name],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_status_preserves_retry_error_and_payload(self):
        self.check_reply('status')

    def test_transport_and_framing_failures_clear_outputs(self):
        self.check_reply('framing')

    def test_strict_status_accepts_fixed_replies_and_unaligned_requests(self):
        self.check_reply('strict')


if __name__ == '__main__':
    unittest.main()
