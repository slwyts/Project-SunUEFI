"""Actual product provider and actual shared-UFS adapter, host only."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
CRYPTO=ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
class UfsProductVolumeTests(unittest.TestCase):
    def test_actual_provider_reserved_and_original_cases(self):
        with tempfile.TemporaryDirectory(prefix='piano-product-volume-') as folder:
            out=Path(folder)
            spec=importlib.util.spec_from_file_location('piano_capture',ROOT/'tools/prepare_ufs_write_test.py')
            capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)
            (out/'PianoUfsWriteTestBaseline.h').write_bytes(capture._render_header(capture.verify_capture()))
            sources=[ROOT/'tests/PianoUfsProductVolumeTest.c',ROOT/'bootprofiles/uefi-app/PianoUfsProductVolume.c',ROOT/'bootprofiles/uefi-app/PianoGpt.c']
            cmd=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-unused-const-variable','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(out),'-I',str(INC),'-I',str(INC/'X64'),'-I',str(CRYPTO),*[str(p) for p in sources],'-lcrypto','-o',str(out/'volume')]
            subprocess.run(cmd,check=True);subprocess.run([str(out/'volume')],check=True)
            current=ROOT/'private/analysis/linux-live-test113/current-lun4-gpt.bin'
            if current.is_file():
                subprocess.run([str(out/'volume'),'--current-gpt',str(current)],check=True)
            proposal=ROOT/'private/provisioning/piano-storage-v1-final'
            if proposal.is_dir():
                subprocess.run([str(out/'volume'),str(proposal)],check=True)
            transport=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-unused-const-variable','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(out),'-I',str(INC),'-I',str(INC/'X64'),'-I',str(CRYPTO),str(ROOT/'tests/PianoUfsProductTransportTest.c'),*[str(p) for p in sources[1:]],str(ROOT/'bootprofiles/uefi-app/PianoUfsBoundedLayout.c'),'-Wl,--gc-sections','-lcrypto','-o',str(out/'transport')]
            subprocess.run(transport,check=True);subprocess.run([str(out/'transport')],check=True)
    def test_actual_aarch64_provider_and_transport_both_gates(self):
        for enabled in (0,1):
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DPIANO_UFS_BLOCKIO=1',f'-DPIANO_UFS_PRODUCT_STORAGE={enabled}','-I',str(INC),'-I',str(INC/'AArch64'),'-I',str(CRYPTO),str(ROOT/'bootprofiles/uefi-app/PianoUfsReadOnlyDma.c'),str(ROOT/'bootprofiles/uefi-app/PianoUfsProductVolume.c')],check=True)
if __name__=='__main__':unittest.main()
