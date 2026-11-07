"""Check real RPMB packet validation; never open hardware or mock a transport."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixRpmbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-rpmb-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_rpmb'
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            str(ROOT / 'tests/native/test_goodix_rpmb.c'), '-o', str(cls.binary),
        ], check=True)

    def check_request(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_single_authenticated_transactions(self):
        self.check_request('allowed')

    def test_key_programming_purge_and_mixed_batches_are_refused(self):
        self.check_request('prohibited')

    def test_lengths_capacity_and_batch_consistency(self):
        self.check_request('bounds')

    def test_metadata_partition_refusal_and_failed_session(self):
        self.check_request('metadata')


if __name__ == '__main__':
    unittest.main()
