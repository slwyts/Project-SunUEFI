"""Actual GOP renderer pixels/fences and canonical single-product source wiring."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
LIB=ROOT/'uefi/components/product-support/Library/ProductBootManagerLib'
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'


class ProductSplashTests(unittest.TestCase):
    def test_actual_renderer_preview_and_failure_boundaries(self):
        output=ROOT/'build/previews/product-splash';output.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='piano-splash-')as temporary:
            exe=Path(temporary)/'splash'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-fshort-wchar','-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I'+str(INC),'-I'+str(INC/'X64'),str(ROOT/'tests/native/PianoProductSplashTest.c'),
                str(LIB/'ProductSplash.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe),str(output/'product-splash-3200x2136.ppm')],capture_output=True,text=True,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            print(run.stdout.strip())
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
            '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),str(LIB/'ProductSplash.c')],check=True)
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
            '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),
            '-I'+str(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include'),
            '-I'+str(ROOT/'upstream/Mu-Silicium/Common/Mu/MsCorePkg/Include'),
            str(LIB/'ProductBootManagerLib.c')],check=True)

    def test_actual_inf_bds_hook_and_freshness_inputs(self):
        inf=(LIB/'ProductBootManagerLib.inf').read_text()
        for name in ('ProductSplash.c','ProductSplash.h','ProductSplashAssets.h','gEfiGraphicsOutputProtocolGuid'):
            self.assertIn(name,inf)
        hook=(LIB/'ProductBootManagerLib.c').read_text()
        self.assertIn('PianoProductDrawSplash(Gop,SplashAlive,&Report)',hook)
        self.assertIn('EnsureFence()',hook)
        self.assertNotIn('BootLogoEnableLogo',hook)
        self.assertNotIn('SetMode(',hook)
        prepare=(ROOT/'tools/prepare_product.py').read_text()
        self.assertIn("shutil.copytree(root/'uefi/components/product-support',target,dirs_exist_ok=True)",prepare)
        integrity=(ROOT/'tools/build_integrity.py').read_text()
        self.assertIn("'uefi/components/product-support'",integrity)
        assets=(LIB/'ProductSplashAssets.h').read_text()
        self.assertIn('F12: BIOS Setup   Esc: Boot Menu',assets)
        self.assertIn('Volume +/-: Navigate',assets)
        self.assertIn('Power: Select / Confirm',assets)


if __name__=='__main__':unittest.main()
