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
        binding = 'AArch64' if platform.machine() in ('arm64', 'aarch64') else 'X64'
        with tempfile.TemporaryDirectory(prefix='piano-reset-') as directory:
            binary = Path(directory) / 'reset'
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(include), '-I' + str(include / binding),
                *core_include_flags(ROOT), str(ROOT / 'tests/native/PianoResetTest.c'),
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
