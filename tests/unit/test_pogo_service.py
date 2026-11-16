"""Check the production keyboard service's release order without hardware access."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

from core_source import core_include_flags
import prepare_product

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'


class PogoServiceTests(unittest.TestCase):
    def test_serial_close_failure_retains_board_resources(self):
        if not (ROOT / 'vendor/firmware/piano-keyboard/MCU_Upgrade.bin').is_file():
            self.skipTest('piano-firmware checkout is not present')
        with tempfile.TemporaryDirectory(prefix='piano-pogo-service-') as temporary:
            work = Path(temporary)
            prepare_product.prepare_pogo_firmware(ROOT, work)
            executable = work / 'pogo-service'
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-g', '-O1',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I', str(INCLUDE), '-I', str(INCLUDE / BIND),
                '-I', str(ROOT / 'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'),
                '-I', str(work), *core_include_flags(ROOT),
                str(ROOT / 'tests/native/PianoPogoServiceTest.c'), '-o', str(executable),
            ], check=True)
            subprocess.run([executable], check=True)


if __name__ == '__main__':
    unittest.main()
