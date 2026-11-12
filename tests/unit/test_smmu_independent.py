"""Actual source prototype, injected register operations only; no device binding."""
import sys
import platform
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from core_source import core_source, core_include_dirs


ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
TOOLCHAIN = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', ROOT / 'build/host-tools/usr'))
SOURCE = ROOT/'uefi/components/smmu-independent'
INC = ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'


class IndependentSmmuTests(unittest.TestCase):
    def test_actual_backend_register_fixture(self):
        with tempfile.TemporaryDirectory(prefix='piano-independent-smmu-') as temporary:
            exe = Path(temporary)/'backend'
            args = ['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-g', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie']
            for path in (SOURCE, *core_include_dirs(ROOT), INC, INC/HOST_PROCESSOR_BIND):
                args += ['-I', str(path)]
            args += [str(ROOT/'tests/native/PianoSmmuIndependentTest.c'),
                     str(ROOT/'uefi/components/smmu-independent/PianoSmmuNativeIndependent.c'),
                     str(ROOT/'uefi/core/memory/PianoIoPageTable.c'), '-o', str(exe)]
            subprocess.run(args, check=True)
            subprocess.run([str(exe)], check=True)

    def test_actual_arm64_source(self):
        args = [str(TOOLCHAIN/'bin/clang'), '--target=aarch64-windows-msvc',
                '-ffreestanding', '-fshort-wchar', '-fsyntax-only', '-Wall', '-Wextra', '-Werror']
        for path in (SOURCE, *core_include_dirs(ROOT), INC, INC/'AArch64'):
            args += ['-I', str(path)]
        subprocess.run(args+[str(ROOT/'uefi/components/smmu-independent/PianoSmmuNativeIndependent.c')], check=True)


if __name__ == '__main__':
    unittest.main()
