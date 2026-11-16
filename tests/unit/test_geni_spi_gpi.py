"""Check production GPI DMA retirement and completion bounds, not hardware support."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

from core_source import core_include_flags

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'


class GeniSpiGpiTests(unittest.TestCase):
    def test_dma_retirement_and_completion_validation(self):
        with tempfile.TemporaryDirectory(prefix='spi-gpi-') as temporary:
            binary = Path(temporary) / 'gpi'
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-g', '-O1',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(INCLUDE), '-I' + str(INCLUDE / BIND),
                '-I' + str(ROOT / 'uefi/components/guarded-read'),
                *core_include_flags(ROOT),
                str(ROOT / 'tests/native/PianoGeniSpiGpiTest.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
