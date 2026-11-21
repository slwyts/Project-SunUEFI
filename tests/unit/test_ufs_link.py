"""Run the actual standard UFS link/management path with a host register interface."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UFS = ROOT / 'windows/drivers/ufs'


class UfsLinkTest(unittest.TestCase):
    def test_cold_warm_device_initialization_luns_and_dma_retirement(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / 'ufs-link'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', UFS, UFS / 'link.c', UFS / 'transfer.c',
                            ROOT / 'tests/native/UfsLinkTest.c', '-o', executable], check=True)
            subprocess.run([executable], check=True)


if __name__ == '__main__':
    unittest.main()
