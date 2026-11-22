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

    def test_storport_late_write_and_reset_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / 'ufs-miniport'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
                            '-fdata-sections', '-Wl,--gc-sections', '-I', ROOT / 'tests/fixtures/storport',
                            '-I', UFS, UFS / 'storport_io.c', UFS / 'storport_link.c', UFS / 'link.c',
                            UFS / 'transfer.c', ROOT / 'tests/native/UfsMiniportTest.c', '-o', executable], check=True)
            subprocess.run([executable], check=True)

    def test_board_s0_power_requires_real_supply_requests(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / 'ufs-rpmh'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'tests/fixtures/storport',
                            '-I', UFS, UFS / 'piano_rpmh.c', ROOT / 'tests/native/PianoUfsRpmhTest.c',
                            '-o', executable], check=True)
            subprocess.run([executable], check=True)


if __name__ == '__main__':
    unittest.main()
