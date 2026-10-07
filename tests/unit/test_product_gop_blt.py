"""Actual product GOP adapter linked with actual selected FrameBufferBltLib."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
SIL=ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'
DRIVER=ROOT/'uefi/components/product-support/Drivers/PianoGopDxe'


class ProductGopBltTests(unittest.TestCase):
    def test_actual_adapter_and_library_with_canaries(self):
        includes=['-I'+str(BASE/'MdePkg/Include'),'-I'+str(BASE/'MdePkg/Include/X64'),
                  '-I'+str(BASE/'MdeModulePkg/Include'),'-I'+str(BASE/'ArmPkg/Include'),'-I'+str(SIL)]
        with tempfile.TemporaryDirectory(prefix='product-gop-blt-')as tmp:
            exe=Path(tmp)/'gop'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-fshort-wchar','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',*includes,
                str(ROOT/'tests/native/PianoGopBltTest.c'),
                str(BASE/'MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
            '-fshort-wchar','-ffreestanding','-fsyntax-only','-Wall','-Wextra','-Werror',
            '-D_PCD_VALUE_PcdFrameBufferWidth=3200','-D_PCD_VALUE_PcdFrameBufferHeight=2136',
            '-D_PCD_VALUE_PcdFrameBufferColorDepth=32','-I'+str(BASE/'MdePkg/Include'),
            '-I'+str(BASE/'MdePkg/Include/AArch64'),'-I'+str(BASE/'ArmPkg/Include'),'-I'+str(SIL),
            '-I'+str(BASE/'MdeModulePkg/Include'),
            str(DRIVER/'SimpleFb.c')],check=True)


if __name__=='__main__':unittest.main()
