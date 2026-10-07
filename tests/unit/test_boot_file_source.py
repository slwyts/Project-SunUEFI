from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]


class BootFileSourceTests(unittest.TestCase):
    def test_actual_reader_sfs_sha_and_blob_lifetime(self):
        inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include';crypto=ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include';source=ROOT/'uefi/components/os-boot/PianoBootFileSource.c'
        with tempfile.TemporaryDirectory(prefix='bootfiles-')as tmp:
            exe=Path(tmp)/'file';flags=['-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(crypto)]
            build=subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie',*flags,str(ROOT/'tests/native/PianoBootFileSourceTest.c'),str(source),str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)],capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
            flags[1]='-I'+str(inc/'AArch64');subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',*flags,str(source)],check=True)


if __name__=='__main__':unittest.main()
