"""Execute real mapping observer and compile its real AArch64 AT sequence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
PRINT=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Library/BasePrintLib'
SRC=ROOT/'bootprofiles/uefi-app/PianoFrameBufferMappingObserve.c'
class FrameBufferMappingObserveTests(unittest.TestCase):
    def test_actual_metadata_and_lifetime_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='fb-mapping-')as directory:
            exe=Path(directory)/'fb'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0',
                '-I'+str(INC),'-I'+str(INC/'X64'),str(ROOT/'tests/PianoFrameBufferMappingObserveTest.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_real_aarch64_control_and_translation_object(self):
        with tempfile.TemporaryDirectory(prefix='fb-at-object-')as directory:
            output=Path(directory)/'mapping.obj'
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-I'+str(INC),'-I'+str(INC/'AArch64'),'-c',str(SRC),'-o',str(output)],check=True)
            asm=subprocess.check_output([str(ROOT/'build/host-tools/usr/bin/llvm-objdump'),'-d',str(output)],text=True).lower()
            for instruction in ('at\ts1e1r','par_el1','daif','sctlr_el1','tcr_el1','ttbr0_el1','ttbr1_el1','mair_el1'):self.assertIn(instruction,asm)
        source=SRC.read_text()
        for forbidden in ('Mmio','RegisterInterruptHandler','CreateEvent','Allocate','FreePool','SetMemory','WriteBack','->Blt(','QueryMode(','msr sctlr','msr tcr','msr ttbr'):
            self.assertNotIn(forbidden,source)
if __name__=='__main__':unittest.main()
