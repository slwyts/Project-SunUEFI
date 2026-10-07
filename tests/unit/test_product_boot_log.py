"""Execute the actual native product boot log renderer, no device operations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
SOURCE = ROOT / 'uefi/core/PianoProductBootLog.c'
SPLASH = ROOT / 'uefi/components/product-support/Library/ProductBootManagerLib/ProductSplash.c'


class ProductBootLogTests(unittest.TestCase):
    def test_actual_pixels_status_elapsed_and_failure_fences(self):
        output = ROOT / 'build/previews/product-splash'
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='piano-bootlog-') as directory:
            executable = Path(directory) / 'bootlog'
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-fshort-wchar', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-I' + str(INC), '-I' + str(INC / 'X64'),
                str(ROOT / 'tests/native/PianoProductBootLogTest.c'), str(SOURCE), str(SPLASH),
                '-o', str(executable)], check=True)
            run = subprocess.run([str(executable), str(output / 'product-bootlog-3200x2136.ppm')],
                capture_output=True, text=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())
        subprocess.run([str(ROOT / 'build/host-tools/usr/bin/clang'),
            '--target=aarch64-windows-msvc', '-ffreestanding', '-fshort-wchar',
            '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-I' + str(INC),
            '-I' + str(INC / 'AArch64'), str(SOURCE)], check=True)


if __name__ == '__main__':
    unittest.main()
