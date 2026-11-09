import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore'


class PianoBluetoothAddressTests(unittest.TestCase):
    def test_factory_fallback_and_existing_dtb_addresses(self):
        include = BASE / 'MdePkg/Include'
        fdt = BASE / 'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        arch = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as folder:
            executable = Path(folder) / 'bluetooth'
            command = ['cc', '-std=gnu11', '-fshort-wchar', '-DNO_MSABI_VA_FUNCS',
                       '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                       '-I' + str(include), '-I' + str(include / arch),
                       '-I' + str(BASE / 'CryptoPkg/Include'),
                       '-I' + str(ROOT / 'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'),
                       '-I' + str(fdt), str(ROOT / 'tests/native/PianoBluetoothAddressTest.c'),
                       str(ROOT / 'tests/native/PianoLinuxFdtHost.c')]
            command += [str(fdt / name) for name in (
                'fdt.c', 'fdt_ro.c', 'fdt_rw.c', 'fdt_wip.c', 'fdt_sw.c',
                'fdt_check.c', 'fdt_empty_tree.c')]
            subprocess.run(command + ['-lcrypto', '-o', str(executable)], check=True, timeout=60)
            for case in range(8):
                with self.subTest(case=case):
                    subprocess.run([str(executable), str(case)], check=True, timeout=10,
                                   env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
