"""Actual limited clock collector and actual guard; never physical register IO."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1];INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
PRINT=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Library/BasePrintLib'
SRC=ROOT/'bootprofiles/uefi-app/PianoDisplayClockObserve.c'
class DisplayClockObserveTests(unittest.TestCase):
    def test_actual_guard_and_required_unfinished_collector(self):
        with tempfile.TemporaryDirectory(prefix='display-clock-')as directory:
            exe=Path(directory)/'clock'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0',
                '-I'+str(INC),'-I'+str(INC/'X64'),'-I'+str(ROOT/'bootprofiles/guarded-read'),str(ROOT/'tests/PianoDisplayClockObserveTest.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_actual_aarch64_and_fixed_declarations(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(ROOT/'bootprofiles/guarded-read'),str(SRC)],check=True)
        source=SRC.read_text()
        self.assertIn('required_unfinished=1',source);self.assertIn('hardware_ready=0',source)
        for forbidden in ('MmioWrite','SetMemory','WriteBack','MapRegion','SetMode(','PianoGuardedRead32(Token,mSpecs'):
            self.assertNotIn(forbidden,source)
if __name__=='__main__':unittest.main()
