"""Actual unified product lifecycle manager, no device I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
class ProductOwnersTests(unittest.TestCase):
    def test_actual_raw_linux_clean_ledger(self):
        # Compile the production CPU-only handoff predicate with the actual
        # owner manager; ARM entry/MMIO and image copying are outside this test.
        source=(ROOT/'uefi/core/PianoRawLinuxBoot.c').read_text()
        start=source.index('STATIC BOOLEAN Clean(');body=source.index('{',start)
        end=body+1;depth=1
        while depth:
            depth+=(source[end]=='{')-(source[end]=='}');end+=1
        clean=source[start:end].replace('BOOLEAN Clean(','BOOLEAN RawOwnersClean(',1)
        fixture=ROOT/'tests/native/PianoProductOwnersTest.c'
        with tempfile.TemporaryDirectory(prefix='piano-raw-owners-') as directory:
            native=Path(directory)/'raw-clean.c';binary=Path(directory)/'raw-clean'
            native.write_text('#define main OriginalOwnersMain\n#include "'+str(fixture)+'"\n#undef main\n'
                'static struct { BOOLEAN FileMode; VOID *Token; } mRaw;\n'+clean+r'''
int main(void) {
  fresh();mRaw.FileMode=FALSE;BootAction.Context=&mRaw;mRaw.Token=BootAction.Token;
  initialize_action(PianoUsbServiceActionBoot);assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS);
  assert(RawOwnersClean(&Owners));
  fresh();without_usb();mRaw.FileMode=TRUE;mRaw.Token=(VOID *)0x702;
  assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
  Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
  assert(PianoProductOwnersRequestFileBoot(&Owners,&mRaw,mRaw.Token)==EFI_SUCCESS);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&RawOwnersClean(&Owners));
  UINTN Checks=StartupValidations;
  assert(!Owners.Report.UsbStopped&&!Owners.Report.BootActionConsumed&&!UsbStatusReads);
  Owners.Report.UsbStopped=TRUE;assert(!RawOwnersClean(&Owners));Owners.Report.UsbStopped=FALSE;
  Owners.Report.UsbStartupFailedClean=FALSE;assert(!RawOwnersClean(&Owners));Owners.Report.UsbStartupFailedClean=TRUE;
  Owners.Report.ProofAccepted=FALSE;assert(!RawOwnersClean(&Owners));Owners.Report.ProofAccepted=TRUE;
  Owners.Report.BootActionConsumed=TRUE;assert(!RawOwnersClean(&Owners));Owners.Report.BootActionConsumed=FALSE;
  Owners.Report.FileToken=(VOID *)0x703;assert(!RawOwnersClean(&Owners));Owners.Report.FileToken=mRaw.Token;
  mRaw.FileMode=FALSE;assert(!RawOwnersClean(&Owners));mRaw.FileMode=TRUE;
  StartupProof.TablePhysical+=4096;assert(!RawOwnersClean(&Owners));StartupProof.TablePhysical-=4096;
  Owners.Report.RetiredMask|=PIANO_OWNER_USB;assert(!RawOwnersClean(&Owners));Owners.Report.RetiredMask&=~PIANO_OWNER_USB;
  assert(RawOwnersClean(&Owners)&&StartupValidations==Checks);
  puts("Actual RawLinux Clean: normal USB boot and accepted startup-absent ESP; forged Stop/ACK, lost proof, changed token/peer/mask refused; CPU-only");
  return 0;
}
''')
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
                '-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),str(native),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_source_order_and_retained_cases(self):
        with tempfile.TemporaryDirectory(prefix='piano-product-owners-') as directory:
            binary=Path(directory)/'owners'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),str(ROOT/'tests/native/PianoProductOwnersTest.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_source_aarch64(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(INC),'-I',str(INC/'AArch64'),str(ROOT/'uefi/core/PianoProductOwners.c')],check=True)
    def test_actual_raw_linux_aarch64(self):
        with tempfile.TemporaryDirectory(prefix='piano-raw-syntax-') as directory:
            staged=Path(directory);headers=staged/'Components/os-boot';headers.mkdir(parents=True)
            for header in (ROOT/'uefi/components/os-boot').glob('*.h'):
                (headers/header.name).write_bytes(header.read_bytes())
            (staged/'core').symlink_to(ROOT/'uefi/core',target_is_directory=True)
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding',
                '-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
                '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(INC.parents[1]/'CryptoPkg/Include'),
                '-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(staged),
                str(ROOT/'uefi/core/PianoRawLinuxBoot.c')],check=True)
if __name__=='__main__':unittest.main()
