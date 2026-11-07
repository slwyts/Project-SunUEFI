"""Actual unified product lifecycle manager, no device I/O."""
import platform
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
class ProductOwnersTests(unittest.TestCase):
    def test_actual_raw_linux_payload_budget(self):
        source=(ROOT/'uefi/core/PianoRawLinuxBoot.c').read_text()
        start=source.index('STATIC EFI_STATUS Parse(')
        parse=source[start:source.index('\nSTATIC EFI_STATUS Ready(',start)]
        limits='\n'.join(line for line in source.splitlines() if line.startswith('#define RAW_'))
        with tempfile.TemporaryDirectory(prefix='piano-raw-budget-') as directory:
            native=Path(directory)/'budget.c';binary=Path(directory)/'budget'
            native.write_text('''#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
'''+ '#include "'+str(ROOT/'uefi/core/PianoFastbootBoot.h')+'"\n'
                '#include "'+str(ROOT/'uefi/core/PianoFastboot.h')+'"\n'
                '#include "'+str(ROOT/'uefi/core/PianoCpuInput.h')+'"\n'+limits+r'''
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT32 EFIAPI ReadUnaligned32(CONST UINT32 *P){UINT32 V;memcpy(&V,P,sizeof(V));return V;}
UINT64 EFIAPI ReadUnaligned64(CONST UINT64 *P){UINT64 V;memcpy(&V,P,sizeof(V));return V;}
'''+parse+r'''
static UINT8 Header[1660],Image[64];
static VOID Put(UINT8 *P,UINTN At,UINTN Bytes,UINT64 V){
  for(UINTN I=0;I<Bytes;++I)P[At+I]=(UINT8)(V>>(I*8));
}
static VOID Layout(UINT64 Total,UINT32 Initrd,UINT32 Dtb){
  memset(Header,0,sizeof(Header));memset(Image,0,sizeof(Image));
  memcpy(Header,"ANDROID!",8);Put(Header,8,4,Total-4096-Initrd-Dtb);
  Put(Header,16,4,Initrd);Put(Header,36,4,4096);Put(Header,40,4,2);
  Put(Header,1644,4,sizeof(Header));Put(Header,1648,4,Dtb);
  Put(Image,8,8,0x80000);Put(Image,16,8,Total-4096-Initrd-Dtb);memcpy(Image+56,"ARMd",4);
}
static EFI_STATUS Read(VOID *C,UINT64 At,UINTN N,VOID *Out){
  (VOID)C;
  if(!At&&N<=sizeof(Header)){memcpy(Out,Header,N);return EFI_SUCCESS;}
  if(At==4096&&N<=sizeof(Image)){memcpy(Out,Image,N);return EFI_SUCCESS;}
  return EFI_ACCESS_DENIED;
}
int main(void){
  const UINT64 MiB=1024*1024;PIANO_BOOT_IMAGE Parsed;
  PIANO_BOOT_SOURCE Source={NULL,Read,PIANO_CPU_INPUT_LOW_BYTES};
  assert(Source.Bytes==128*MiB&&PIANO_FASTBOOT_MAX_DOWNLOAD==64*MiB);
  Layout(Source.Bytes,16*MiB,MiB);assert(Parse(&Source,&Parsed)==EFI_SUCCESS);
  Source.Bytes+=4096;Layout(Source.Bytes,16*MiB,MiB);
  assert(Parse(&Source,&Parsed)==EFI_UNSUPPORTED);Source.Bytes-=4096;
  Layout(Source.Bytes,16*MiB+4096,MiB);assert(Parse(&Source,&Parsed)==EFI_UNSUPPORTED);
  Layout(Source.Bytes,16*MiB,RAW_DTB_MAX-0x10000+4096);assert(Parse(&Source,&Parsed)==EFI_UNSUPPORTED);
  Layout(Source.Bytes,16*MiB,MiB);Put(Image,16,8,RAW_DTB-RAW_KERNEL);
  assert(Parse(&Source,&Parsed)==EFI_COMPROMISED_DATA);
  puts("Actual RawLinux parser: ESP128MiB, USB64MiB, oversized source/initrd/DTB and kernel-span boundaries");
  return 0;
}
''')
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(INC),'-I'+str(INC/'X64'),
                str(native),str(ROOT/'uefi/core/PianoFastbootBoot.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_raw_linux_clean_ledger(self):
        # Compile the production CPU-only handoff predicate with the actual
        # owner manager; ARM entry/MMIO and image copying are outside this test.
        source=(ROOT/'uefi/core/PianoRawLinuxBoot.c').read_text()
        def function(text, name):
            start=text.index(name+'(');start=text.rfind('\n',0,start)+1;body=text.index('{',start)
            end=body+1;depth=1
            while depth:
                depth+=(text[end]=='{')-(text[end]=='}');end+=1
            return text[start:end]
        clean=function(source,'Clean').replace('BOOLEAN Clean(','BOOLEAN RawOwnersClean(',1)
        state=source[source.index('typedef struct {'):source.index('} RAW_STATE;')+len('} RAW_STATE;')]
        predicates='\n'.join(function(source,name)for name in ('SameImage','SameExitImage','PianoRawLinuxPrepared')).replace('Clean(O)','RawOwnersClean(O)')
        released=function((ROOT/'uefi/components/os-boot/PianoEspBootSource.c').read_text(),'PianoEspBootReleased')
        fixture=ROOT/'tests/native/PianoProductOwnersTest.c'
        with tempfile.TemporaryDirectory(prefix='piano-raw-owners-') as directory:
            native=Path(directory)/'raw-clean.c';binary=Path(directory)/'raw-clean'
            native.write_text('#define main OriginalOwnersMain\n#include "'+str(fixture)+'"\n#undef main\n'
                '#include "'+str(ROOT/'uefi/core/PianoRawLinuxBoot.h')+'"\n'
                '#include "'+str(ROOT/'uefi/core/PianoFastbootDownloadBlob.h')+'"\n'
                '#include "'+str(ROOT/'uefi/components/os-boot/PianoEspBootSource.h')+'"\n'
                +state+'\nstatic RAW_STATE mRaw;\n'+clean+'\n'+released+'\n'+predicates+r'''
int main(void) {
  fresh();mRaw.FileMode=FALSE;BootAction.Context=&mRaw;BootAction.Token=&mRaw.Adapter;mRaw.Token=BootAction.Token;
  initialize_action(PianoUsbServiceActionBoot);assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS);
  assert(RawOwnersClean(&Owners));
  EFI_SYSTEM_TABLE Table={0};EFI_LOADED_IMAGE_PROTOCOL Core={.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,
    .SystemTable=&Table,.ParentHandle=(VOID *)0x100,.DeviceHandle=(VOID *)0x101,
    .ImageBase=(VOID *)0x100000,.ImageSize=4096,.ImageCodeType=EfiLoaderCode,.ImageDataType=EfiLoaderData},Exit=Core;
  Exit.ParentHandle=(VOID *)0x333;Exit.ImageBase=(VOID *)0x200000;
  mRaw.Image=(VOID *)0x222;mRaw.Identity=&Core;mRaw.Loaded=Core;
  mRaw.ExitImage=(VOID *)0x111;mRaw.ExitIdentity=&Exit;mRaw.ExitLoaded=Exit;mRaw.Prepared=TRUE;
  mRaw.Report=(PIANO_RAW_LINUX_REPORT){.Image=mRaw.Image,.Identity=&Core,.ExitImage=mRaw.ExitImage,.ExitIdentity=&Exit};
  mRaw.Owners=&Owners;mRaw.Clean=Owners.Report;mRaw.Config=Owners.Config;
  mRaw.Loan=(VOID *)0x991;mRaw.View=(VOID *)0x992;mRaw.Adapter.Taken=TRUE;mRaw.Adapter.ActiveLoan=mRaw.Loan;mRaw.Adapter.Owned=(VOID *)mRaw.View;
  EFI_BOOT_SERVICES *SavedServices=gBS;gBS=(VOID *)1;UINTN SavedCalls=Calls;
  assert(PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image));
  EFI_LOADED_IMAGE_PROTOCOL SavedExit=Exit;
  for(UINTN I=0;I<8;++I){Exit=SavedExit;switch(I){
    case 0:Exit.Revision++;break;case 1:Exit.ImageBase=(VOID *)0xdead;break;case 2:Exit.ImageSize++;break;
    case 3:Exit.SystemTable=NULL;break;case 4:Exit.ParentHandle=NULL;break;case 5:Exit.DeviceHandle=NULL;break;
    case 6:Exit.ImageCodeType=EfiBootServicesCode;break;case 7:Exit.ImageDataType=EfiBootServicesData;break;
  }assert(!PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image));}
  Exit=SavedExit;mRaw.Report.ExitImage=mRaw.Image;assert(!PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image));mRaw.Report.ExitImage=mRaw.ExitImage;
  mRaw.Report.ExitIdentity=&Core;assert(!PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image));mRaw.Report.ExitIdentity=&Exit;
  Core.ImageBase=NULL;assert(!PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image));Core=mRaw.Loaded;
  assert(PianoRawLinuxPrepared(&mRaw.Report,&Owners,mRaw.Image)&&Calls==SavedCalls);gBS=SavedServices;
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
  puts("Actual RawLinux Clean/Prepared: Core ownership, distinct current exit identity, CPU-only mutation checks, USB and startup-absent ESP retirement");
  return 0;
}
''')
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
                '-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/HOST_PROCESSOR_BIND),'-I'+str(ROOT/'uefi/components/nv'),'-I'+str(ROOT/'uefi/components/ext4'),str(native),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_source_order_and_retained_cases(self):
        with tempfile.TemporaryDirectory(prefix='piano-product-owners-') as directory:
            binary=Path(directory)/'owners'
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/HOST_PROCESSOR_BIND),'-I'+str(ROOT/'uefi/components/nv'),'-I'+str(ROOT/'uefi/components/ext4'),str(ROOT/'tests/native/PianoProductOwnersTest.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_actual_source_aarch64(self):
        subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(INC),'-I',str(INC/'AArch64'),'-I'+str(ROOT/'uefi/components/nv'),'-I'+str(ROOT/'uefi/components/ext4'),str(ROOT/'uefi/core/PianoProductOwners.c')],check=True)
    def test_actual_raw_linux_aarch64(self):
        with tempfile.TemporaryDirectory(prefix='piano-raw-syntax-') as directory:
            staged=Path(directory);headers=staged/'Components/os-boot';headers.mkdir(parents=True)
            for header in (ROOT/'uefi/components/os-boot').glob('*.h'):
                (headers/header.name).write_bytes(header.read_bytes())
            (staged/'core').symlink_to(ROOT/'uefi/core',target_is_directory=True)
            subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding',
                '-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
                '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(INC.parents[1]/'CryptoPkg/Include'),
                '-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(staged),
                '-I'+str(ROOT/'uefi/handoff/early-memory'),
                str(ROOT/'uefi/core/PianoRawLinuxBoot.c')],check=True)
if __name__=='__main__':unittest.main()
