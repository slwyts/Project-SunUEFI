"""Compile the real bounded GOP inventory, never a shadow JSON observer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
SRC=ROOT/'uefi/core/PianoProductDisplayObserve.c'
PRINT=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Library/BasePrintLib'
class ProductDisplayObserveTests(unittest.TestCase):
    def test_actual_protocol_inventory_and_retention(self):
        with tempfile.TemporaryDirectory(prefix='piano-gop-observe-')as temp:
            exe=Path(temp)/'observe'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DNO_MSABI_VA_FUNCS',
                '-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0',
                '-I'+str(INC),'-I'+str(INC/'X64'),'-I'+str(SRC.parent),
                str(ROOT/'tests/native/PianoProductDisplayObserveTest.c'),str(SRC),
                str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
            self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_actual_aarch64_source_and_no_hardware_operation(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
            '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),str(SRC)],check=True)
        source=SRC.read_text()
        for forbidden in ('Mmio','SetMode(','->Blt(','QueryMode(','ArmMmu','GetMemorySpaceDescriptor','asm'):
            self.assertNotIn(forbidden,source)
if __name__=='__main__':unittest.main()
