"""Actual CPU borrow helper + actual fastboot download ownership; no device."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
CRYPTO=ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
class CpuImageLoanTests(unittest.TestCase):
    def test_actual_download_blob_integration(self):
        with tempfile.TemporaryDirectory(prefix='piano-cpu-image-loan-') as directory:
            binary=Path(directory)/'loan'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),'-I',str(CRYPTO),'-I',str(ROOT/'uefi/core'),str(ROOT/'tests/native/PianoCpuImageLoanTest.c'),str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_aarch64_source(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I',str(INC),'-I',str(INC/'AArch64'),'-I',str(CRYPTO),'-I',str(ROOT/'uefi/core'),str(ROOT/'uefi/components/os-boot/PianoCpuImageLoan.c')],check=True)
if __name__=='__main__':unittest.main()
