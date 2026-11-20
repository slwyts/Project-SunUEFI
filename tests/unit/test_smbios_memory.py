"""Check the SMBIOS records delivered to the standard EDK2 table service."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MU = ROOT / 'upstream/Mu-Silicium'


class SmbiosMemoryTest(unittest.TestCase):
    def test_installed_capacity_and_discontiguous_ddr(self):
        with tempfile.TemporaryDirectory() as work:
            exe = Path(work) / 'smbios-memory'
            memory = ROOT / 'uefi/core/memory'
            includes = (memory, ROOT / 'uefi/handoff/early-memory', MU / 'Mu_Basecore/MdePkg/Include',
                        MU / 'Mu_Basecore/MdePkg/Include/X64', MU / 'Silicon/Qualcomm/QcomPkg/Include')
            subprocess.run(['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                            *(f'-I{p}' for p in includes), ROOT / 'tests/native/PianoSmbiosMemoryTest.c',
                            memory / 'PianoSmbiosMemory.c', '-o', exe], check=True)
            subprocess.run([exe], check=True)


if __name__ == '__main__':
    unittest.main()
