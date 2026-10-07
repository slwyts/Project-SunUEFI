"""Actual production coordinator joined to Owners and the real GCC guard."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore';INC=BASE/'MdePkg/Include'
QCOM=ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include';PRINT=BASE/'MdePkg/Library/BasePrintLib'
SRC=ROOT/'uefi/core/PianoProductDisplayOwner.c'
class ProductDisplayOwnerTests(unittest.TestCase):
    def test_actual_coordinator_owner_and_gcc_guard_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='display-owner-joint-')as directory:
            exe=Path(directory)/'owner'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough','-Wno-unused-parameter',
                '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DPIANO_GUARDED_HOST_TEST','-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0',
                '-I'+str(INC),'-I'+str(INC/'X64'),'-I'+str(QCOM),'-I'+str(ROOT/'uefi/core'),'-I'+str(ROOT/'uefi/components/guarded-read'),'-I'+str(ROOT/'uefi/components/display-rail'),
                str(ROOT/'tests/native/PianoProductDisplayOwnerTest.c'),str(SRC),str(ROOT/'uefi/core/PianoProductOwners.c'),str(ROOT/'uefi/components/guarded-read/PianoGuardedRead.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_actual_aarch64_coordinator(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(QCOM),'-I'+str(ROOT/'uefi/core'),'-I'+str(ROOT/'uefi/components/guarded-read'),'-I'+str(ROOT/'uefi/components/display-rail'),str(SRC)],check=True)
if __name__=='__main__':unittest.main()
