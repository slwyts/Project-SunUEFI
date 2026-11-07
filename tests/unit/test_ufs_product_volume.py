"""Actual product provider and actual shared-UFS adapter, host only."""
import platform
import os
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
CRYPTO=ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
class UfsProductVolumeTests(unittest.TestCase):
    @unittest.skipUnless((ROOT/'private/captures/ufs-test-area-1/manifest.json').is_file(),'private real capture absent')
    def test_actual_provider_reserved_and_original_cases(self):
        with tempfile.TemporaryDirectory(prefix='piano-product-volume-') as folder:
            out=Path(folder)
            for name in ('PianoExt4.h','PianoStorageIo.h','PianoStorageIo.inc'):
                (out/name).write_bytes((ROOT/'uefi/components/ext4'/name).read_bytes())
            spec=importlib.util.spec_from_file_location('piano_capture',ROOT/'tools/prepare_ufs_write_test.py')
            capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)
            (out/'PianoUfsWriteTestBaseline.h').write_bytes(capture._render_header(capture.verify_capture()))
            sources=[ROOT/'tests/native/PianoUfsProductVolumeTest.c',ROOT/'uefi/core/PianoUfsProductVolume.c',ROOT/'uefi/core/PianoGpt.c']
            cmd=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-unused-const-variable','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(out),'-I',str(INC),'-I',str(INC/HOST_PROCESSOR_BIND),'-I',str(CRYPTO),*[str(p) for p in sources],'-lcrypto','-o',str(out/'volume')]
            subprocess.run(cmd,check=True);subprocess.run([str(out/'volume')],check=True)
            for current in (ROOT/'private/analysis/linux-live-test113/current-lun4-gpt.bin',
                            ROOT/'private/provisioning/piano-combined-entry-20261007/product-uefi-request/live-lun4/primary-and-entries.bin'):
                if current.is_file():
                    subprocess.run([str(out/'volume'),'--current-gpt',str(current)],check=True)
            proposal=ROOT/'private/provisioning/piano-storage-v1-final'
            if proposal.is_dir():
                subprocess.run([str(out/'volume'),str(proposal)],check=True)
            transport=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-unused-const-variable','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(out),'-I',str(INC),'-I',str(INC/HOST_PROCESSOR_BIND),'-I',str(CRYPTO),str(ROOT/'tests/native/PianoUfsProductTransportTest.c'),*[str(p) for p in sources[1:]],str(ROOT/'uefi/core/PianoUfsBoundedLayout.c'),'-Wl,--gc-sections','-lcrypto','-o',str(out/'transport')]
            subprocess.run(transport,check=True);subprocess.run([str(out/'transport')],check=True)
    def test_actual_aarch64_provider_and_transport_both_gates(self):
        with tempfile.TemporaryDirectory(prefix='piano-ext4-headers-') as folder:
            for name in ('PianoExt4.h','PianoStorageIo.h','PianoStorageIo.inc'):
                (Path(folder)/name).write_bytes((ROOT/'uefi/components/ext4'/name).read_bytes())
            for enabled in (0,1):
                subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DPIANO_UFS_BLOCKIO=1',f'-DPIANO_UFS_PRODUCT_STORAGE={enabled}','-I',folder,'-I',str(INC),'-I',str(INC/'AArch64'),'-I',str(CRYPTO),str(ROOT/'uefi/core/PianoUfsReadOnlyDma.c'),str(ROOT/'uefi/core/PianoUfsProductVolume.c')],check=True)
if __name__=='__main__':unittest.main()
