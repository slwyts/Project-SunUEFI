"""Actual native RAM inventory guards with a substituted ARM call boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
CRYPTO = ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
SOURCE = ROOT/'bootprofiles/uefi-app'
PE = ROOT/'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.efi'


class RamPartitionTests(unittest.TestCase):
    def test_actual_inventory_and_native_image_identity(self):
        with tempfile.TemporaryDirectory(prefix='piano-ram-partition-') as directory:
            exe = Path(directory)/'inventory'
            args = ['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-g', '-DPIANO_RAM_PARTITION_HOST_TEST=1']
            for path in (INCLUDE, INCLUDE/'X64', CRYPTO, SOURCE):
                args += ['-I', str(path)]
            args += [str(ROOT/'tests/PianoRamPartitionTest.c'),
                     str(SOURCE/'PianoRamPartition.c'), '-lcrypto', '-o', str(exe)]
            build = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            run = subprocess.run([str(exe), str(PE)], capture_output=True, text=True,
                                 env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
            print(run.stdout.strip())

    def test_aarch64_actual_abi_compiles(self):
        args = [str(ROOT/'build/host-tools/usr/bin/clang'),
                '--target=aarch64-windows-msvc', '-ffreestanding', '-fshort-wchar',
                '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation']
        for path in (INCLUDE, INCLUDE/'AArch64', CRYPTO, SOURCE):
            args += ['-I', str(path)]
        result = subprocess.run(args+[str(SOURCE/'PianoRamPartition.c')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
