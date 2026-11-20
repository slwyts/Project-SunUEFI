"""Execute the reset path with host implementations of EFI services."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from core_source import core_include_flags

ROOT = Path(__file__).resolve().parents[2]


class ResetTests(unittest.TestCase):
    def test_reset_requires_clean_owners_and_exit_boot_services(self):
        include = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
        fdt = include.parent / 'Library/BaseFdtLib/libfdt/libfdt'
        binding = 'AArch64' if platform.machine() in ('arm64', 'aarch64') else 'X64'
        with tempfile.TemporaryDirectory(prefix='piano-reset-') as directory:
            binary = Path(directory) / 'reset'
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-DNO_MSABI_VA_FUNCS', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(include), '-I' + str(include / binding), '-I' + str(fdt),
                *core_include_flags(ROOT), str(ROOT / 'tests/native/PianoResetTest.c'),
                str(ROOT / 'tests/native/PianoLinuxFdtHost.c'),
                *[str(fdt / name) for name in ('fdt.c', 'fdt_ro.c', 'fdt_wip.c')],
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary), str(ROOT / 'vendor/piano/dtb/piano.dtb')], check=True)


if __name__ == '__main__':
    unittest.main()
