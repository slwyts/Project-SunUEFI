"""Actual unified product lifecycle manager, no device I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
class ProductOwnersTests(unittest.TestCase):
    def test_actual_source_order_and_retained_cases(self):
        with tempfile.TemporaryDirectory(prefix='piano-product-owners-') as directory:
            binary=Path(directory)/'owners'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),str(ROOT/'tests/PianoProductOwnersTest.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_source_aarch64(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(INC),'-I',str(INC/'AArch64'),str(ROOT/'bootprofiles/uefi-app/PianoProductOwners.c')],check=True)
if __name__=='__main__':unittest.main()
