"""Actual early-memory source with byte-addressed injected reads; no device."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'bootprofiles/early-memory'
INC = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'


class SmemRamTests(unittest.TestCase):
    def test_v3_current_slices_synthetic_and_physical_test95(self):
        captured = ROOT / 'private/analysis/usb-live-test95/ram402.bin'
        self.assertEqual(hashlib.sha256(captured.read_bytes()).hexdigest(),
                         '16aed6a815309af4109f34000f43de0058d310e293b2aecc29c6d4d9a7ed828c')
        with tempfile.TemporaryDirectory(prefix='piano-smem-v3-') as directory:
            binary = Path(directory) / 'v3'
            command = ['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra',
                       '-Werror', '-g', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            for path in (SOURCE, INC, INC / 'X64'):
                command += ['-I', str(path)]
            command += [str(ROOT / 'tests/PianoSmemRamV3Test.c'), str(SOURCE / 'PianoSmemRam.c'), '-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary), str(captured)], check=True, timeout=60)
    def test_actual_source(self):
        with tempfile.TemporaryDirectory(prefix='piano-smem-ram-') as directory:
            binary = Path(directory) / 'smem'
            command = ['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra',
                       '-Werror', '-g', '-fsanitize=address,undefined',
                       '-fno-pie', '-no-pie']
            for path in (SOURCE, INC, INC / 'X64'):
                command += ['-I', str(path)]
            command += [str(ROOT / 'tests/PianoSmemRamTest.c'),
                        str(SOURCE / 'PianoSmemRam.c'), '-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_aarch64_actual_source(self):
        command = [str(ROOT / 'build/host-tools/usr/bin/clang'),
                   '--target=aarch64-windows-msvc', '-ffreestanding',
                   '-fshort-wchar', '-fsyntax-only', '-Wall', '-Wextra', '-Werror']
        for path in (SOURCE, INC, INC / 'AArch64'):
            command += ['-I', str(path)]
        subprocess.run(command + [str(SOURCE / 'PianoSmemRam.c')],
                       check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
