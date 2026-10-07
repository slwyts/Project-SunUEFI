import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]


class PogoDxeTests(unittest.TestCase):
    def test_actual_producer_adapter_and_parser_lifecycle(self):
        inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include';source=ROOT/'uefi/core'
        with tempfile.TemporaryDirectory(prefix='pogo-producer-')as tmp:
            exe=Path(tmp)/'pogo';cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fshort-wchar','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),str(ROOT/'tests/native/PianoPogoDxeTest.c'),str(ROOT/'uefi/components/pogo-product/PianoPogoDxe.c'),str(source/'PianoPogoInput.c'),str(source/'PianoPogoReport.c'),'-o',str(exe)]
            build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),'-I'+str(inc/'AArch64'),str(ROOT/'uefi/components/pogo-product/PianoPogoDxe.c')],check=True)


if __name__=='__main__':unittest.main()
