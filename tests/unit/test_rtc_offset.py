import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore'


class RtcOffsetTests(unittest.TestCase):
    def test_factory_read_only_and_reserved_nvmem(self):
        include = BASE / 'MdePkg/Include'
        fdt = BASE / 'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        arch = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as folder:
            exe = Path(folder) / 'rtc'
            command = ['cc', '-std=gnu11', '-fshort-wchar', '-DNO_MSABI_VA_FUNCS',
                       '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                       '-I' + str(include), '-I' + str(include / arch), '-I' + str(fdt),
                       str(ROOT / 'tests/native/PianoRtcOffsetTest.c'),
                       str(ROOT / 'tests/native/PianoLinuxFdtHost.c')]
            command += [str(fdt / name) for name in (
                'fdt.c', 'fdt_ro.c', 'fdt_rw.c', 'fdt_wip.c', 'fdt_sw.c',
                'fdt_check.c', 'fdt_empty_tree.c')]
            subprocess.run(command + ['-o', str(exe)], check=True, timeout=60)
            for case in range(8):
                with self.subTest(case=case):
                    subprocess.run([str(exe), str(case)], check=True, timeout=10,
                                   env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
