"""Check the PMK8550 UEFI clock against the board DT and its published register map."""
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore'


class RtcClockTests(unittest.TestCase):
    def test_factory_device_calendar_rollover_and_handoff(self):
        headers = BASE / 'MdePkg/Include'
        fdt = BASE / 'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        arch = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'rtc-clock'
            sources = [ROOT / 'tests/native/PianoRtcClockTest.c', ROOT / 'tests/native/PianoLinuxFdtHost.c',
                       BASE / 'EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c',
                       *(fdt / name for name in ('fdt.c', 'fdt_ro.c', 'fdt_wip.c', 'fdt_rw.c'))]
            subprocess.run(['cc', '-std=gnu11', '-fshort-wchar', '-DNO_MSABI_VA_FUNCS', '-Wall', '-Wextra',
                            '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I', headers, '-I',
                            headers / arch, '-I', BASE / 'MdeModulePkg/Include', '-I', BASE / 'EmbeddedPkg/Include',
                            '-I', fdt, *sources, '-o', exe], check=True)
            subprocess.run([exe, ROOT / 'vendor/piano/dtb/piano.dtb'], check=True)


if __name__ == '__main__':
    unittest.main()
