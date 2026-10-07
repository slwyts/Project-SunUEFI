"""Actual persistent Device+Controller code, CPU-only hardware fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
OTHER=[ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include', ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include']
class UsbServiceTests(unittest.TestCase):
    def test_actual_persistent_sources_and_default_off(self):
        with tempfile.TemporaryDirectory(prefix='piano-usb-service-') as directory:
            for enabled in (0,1):
                binary=Path(directory)/f'service-{enabled}'
                flags=['-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',f'-DPIANO_USB_SERVICE={enabled}']
                for path in [INC,INC/'X64',*OTHER]: flags+=['-I',str(path)]
                result=subprocess.run(['cc',*flags,str(ROOT/'tests/native/PianoUsbServiceTest.c'),'-lcrypto','-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                run=subprocess.run([str(binary)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_aarch64_gate_zero_and_one_actual_sources(self):
        for enabled in (0,1):
            flags=['--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter',f'-DPIANO_USB_SERVICE={enabled}','-DPIANO_USB_FASTBOOT=1','-DPIANO_USB_UFS_FETCH=1','-DPIANO_USB_EP0=1','-DPIANO_USB_RAM_BOOT=1']
            for path in [INC,INC/'AArch64',*OTHER]:flags+=['-I',str(path)]
            for name in ['PianoDwc3Device.c','PianoUsbController.c']:
                result=subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),*flags,str(ROOT/'uefi/core'/name)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
