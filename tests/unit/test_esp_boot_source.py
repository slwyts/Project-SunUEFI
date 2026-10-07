"""Current ESP geometry/identity discovery through real SFS snapshot code."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EspBootSourceTests(unittest.TestCase):
    def test_actual_esp_selector_and_source_lifetime(self):
        inc = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
        crypto = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
        sources = [ROOT / 'uefi/components/os-boot' / name for name in
                   ('PianoEspBootSource.c', 'PianoBootFileSource.c', 'PianoCpuInput.c')]
        flags = ['-I' + str(inc), '-I' + str(inc / 'X64'), '-I' + str(crypto)]
        with tempfile.TemporaryDirectory(prefix='piano-esp-source-') as tmp:
            exe = Path(tmp) / 'esp'
            subprocess.run(['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', '-Wno-deprecated-declarations',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', *flags,
                            str(ROOT / 'tests/native/PianoEspBootSourceTest.c'), *map(str, sources),
                            '-lcrypto', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        flags[1] = '-I' + str(inc / 'AArch64')
        subprocess.run([str(ROOT / 'build/host-tools/usr/bin/clang'), '--target=aarch64-windows-msvc',
                        '-ffreestanding', '-fshort-wchar', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                        *flags, *map(str, sources)], check=True)


if __name__ == '__main__':
    unittest.main()
