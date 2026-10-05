"""Actual source prototype, injected register operations only; no device binding."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'bootprofiles/smmu-independent'
INC = ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'


class IndependentSmmuTests(unittest.TestCase):
    def test_actual_backend_register_fixture(self):
        with tempfile.TemporaryDirectory(prefix='piano-independent-smmu-') as temporary:
            exe = Path(temporary)/'backend'
            args = ['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-g', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie']
            for path in (SOURCE, ROOT/'bootprofiles/uefi-app', INC, INC/'X64'):
                args += ['-I', str(path)]
            args += [str(ROOT/'tests/PianoSmmuIndependentTest.c'),
                     str(SOURCE/'PianoSmmuNativeIndependent.c'),
                     str(ROOT/'bootprofiles/uefi-app/PianoIoPageTable.c'), '-o', str(exe)]
            subprocess.run(args, check=True)
            subprocess.run([str(exe)], check=True)

    def test_actual_arm64_source(self):
        args = [str(ROOT/'build/host-tools/usr/bin/clang'), '--target=aarch64-windows-msvc',
                '-ffreestanding', '-fshort-wchar', '-fsyntax-only', '-Wall', '-Wextra', '-Werror']
        for path in (SOURCE, ROOT/'bootprofiles/uefi-app', INC, INC/'AArch64'):
            args += ['-I', str(path)]
        subprocess.run(args+[str(SOURCE/'PianoSmmuNativeIndependent.c')], check=True)


if __name__ == '__main__':
    unittest.main()
