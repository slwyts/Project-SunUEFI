"""Actual persistent Device+Controller code, CPU-only hardware fixtures."""
import sys
import platform
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from core_source import core_include_flags, core_source

ROOT=Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
OTHER=[ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include', ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include']
class UsbServiceTests(unittest.TestCase):
    def test_fat_stop_before_raw_write(self):
        with tempfile.TemporaryDirectory(prefix='piano-fat-raw-write-') as directory:
            folder=Path(directory)
            relative='FatPkg/EnhancedFatDxe/Init.c'
            source=folder/relative
            source.parent.mkdir(parents=True)
            source.write_bytes(subprocess.check_output(['git','-C',str(INC.parents[1]),'show',f'HEAD:{relative}']))
            patch=ROOT/'patches/firmware/mu-basecore/cfb9372/0005-keep-open-fat-files-during-raw-write.patch'
            subprocess.run(['git','apply',str(patch)],cwd=folder,check=True)
            subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc',
                            '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                            '-I',str(INC),'-I',str(INC/'AArch64'),
                            '-I',str((INC.parents[1]/relative).parent),
                            str(source)],check=True)
            text=source.read_text()
            start=text.index('EFI_STATUS\nFatAbandonVolume (')
            (folder/'PianoActualFatAbandon.h').write_text(text[start:text.index('\n}\n',start)+3])
            binary=folder/'fat'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror',
                            '-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                            '-I',str(INC),'-I',str(INC/HOST_PROCESSOR_BIND),'-I',str(folder),
                            str(ROOT/'tests/native/test_fat_raw_write.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_fastboot_partition_scope(self):
        with tempfile.TemporaryDirectory(prefix='piano-fastboot-partition-') as directory:
            binary=Path(directory)/'partition'
            flags=['-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                   '-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                   '-DPIANO_UFS_PRODUCT_STORAGE=1']
            for path in (INC,INC/HOST_PROCESSOR_BIND,*OTHER,ROOT/'uefi/components/ext4',
                         ROOT/'uefi/components/nv',ROOT/'uefi/components/nv/Include'):
                flags+=['-I',str(path)]
            subprocess.run(['cc',*flags,str(ROOT/'tests/native/test_fastboot_block_read.c'),
                            '-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
    def test_actual_fastboot_diagnostics_restriction(self):
        with tempfile.TemporaryDirectory(prefix='piano-fastboot-diagnostics-') as directory:
            for enabled in (0,1):
                binary=Path(directory)/f'diagnostics-{enabled}'
                flags=['-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                       '-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                       f'-DPIANO_USB_SERVICE={enabled}',f'-DPIANO_USB_RAM_BOOT={enabled}']
                for path in [INC,INC/HOST_PROCESSOR_BIND,*OTHER]: flags+=['-I',str(path)]
                sources=[str(ROOT/'tests/native/test_fastboot.c')]
                if enabled: sources+=[str(ROOT/'uefi/core/usb/PianoFastbootBoot.c')]
                subprocess.run(['cc',*flags,*sources,'-lcrypto','-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
                if not enabled:
                    reset=Path(directory)/'diagnostics-usb-reset'
                    subprocess.run(['cc',*flags,str(ROOT/'tests/native/test_usb_screenshot_route.c'),'-lcrypto','-o',str(reset)],check=True)
                    subprocess.run([str(reset)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
    def test_actual_persistent_sources_and_default_off(self):
        with tempfile.TemporaryDirectory(prefix='piano-usb-service-') as directory:
            for enabled in (0,1):
                binary=Path(directory)/f'service-{enabled}'
                flags=['-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',f'-DPIANO_USB_SERVICE={enabled}']
                flags += [f'-DPIANO_USB_HOST={enabled}']
                for path in [INC,INC/HOST_PROCESSOR_BIND,*OTHER,ROOT/'uefi/components/usb-host']: flags+=['-I',str(path)]
                result=subprocess.run(['cc',*flags,str(ROOT/'tests/native/PianoUsbServiceTest.c'),'-lcrypto','-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                run=subprocess.run([str(binary)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_actual_qmp_orientation_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix='piano-usb-phy-') as directory:
            folder=Path(directory)
            # Only the QMP power entries matter to this reset sequence.
            (folder/'PianoUsbPhyConfig.h').write_text(
                'STATIC CONST struct {UINT32 Offset,Value;} mPianoUsbSsPhy[]={{8,1},{0x1e40,1}};\n')
            flags=['-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                   '-DMDEPKG_NDEBUG','-I',str(folder)]
            for path in [INC,*OTHER]:
                flags+=['-I',str(path)]
            subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc',
                            '-ffreestanding','-fsyntax-only',*flags,'-I',str(INC/'AArch64'),
                            str(core_source('PianoUsbPhy.c',ROOT))],check=True)
            binary=folder/'phy'
            subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                            *flags,'-I',str(INC/HOST_PROCESSOR_BIND),
                            str(ROOT/'tests/native/PianoUsbPhyTest.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_aarch64_gate_zero_and_one_actual_sources(self):
        for enabled in (0,1):
            flags=['--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter',f'-DPIANO_USB_SERVICE={enabled}','-DPIANO_USB_FASTBOOT=1','-DPIANO_USB_UFS_FETCH=1','-DPIANO_USB_EP0=1','-DPIANO_USB_RAM_BOOT=1']
            for path in [INC,INC/'AArch64',*OTHER]:flags+=['-I',str(path)]
            for name in ['PianoDwc3Device.c','PianoUsbController.c']:
                result=subprocess.run([str(TOOLCHAIN/'bin/clang'),*flags,str(core_source(name, ROOT))],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            if enabled:
                host=[*flags,'-DPIANO_USB_HOST=1',*core_include_flags(ROOT),'-I',str(ROOT/'uefi/components/usb-host')]
                for path in [ROOT/'uefi/core/usb/PianoUsbController.c',*sorted((ROOT/'uefi/components/usb-host').glob('*.c'))]:
                    result=subprocess.run([str(TOOLCHAIN/'bin/clang'),*host,str(path)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
