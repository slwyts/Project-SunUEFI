"""Actual DXE guard failure injection and real AArch64 LDR/fixup object."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
class GuardedReadTests(unittest.TestCase):
    def test_actual_guard_and_aarch64_assembly(self):
        with tempfile.TemporaryDirectory(prefix='piano-guarded-read-')as directory:
            exe=Path(directory)/'guard'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),str(ROOT/'tests/native/PianoGuardedReadTest.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
            output=Path(directory)/'guard.obj'
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-Wall','-Wextra','-Werror','-I',str(INC),'-I',str(INC/'AArch64'),'-c',str(ROOT/'uefi/components/guarded-read/PianoGuardedRead.c'),'-o',str(output)],check=True)
            self.assertGreater(output.stat().st_size,0)
if __name__=='__main__':unittest.main()
