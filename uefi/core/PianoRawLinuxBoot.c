// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoRawLinuxBoot.h"
#include "PianoPanelSelection.h"
#include "PianoFastbootDownloadBlob.h"
#include "Components/os-boot/PianoEspBootSource.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>

#define RAW_KERNEL 0xA8000000ULL
#define RAW_DTB 0xB0000000ULL
#define RAW_INITRD 0xB0200000ULL
#define RAW_INITRD_MAX 0x1000000U
#define RAW_DTB_MAX 0x200000U
typedef struct {
  EFI_HANDLE Image;EFI_SYSTEM_TABLE *SystemTable;EFI_LOADED_IMAGE_PROTOCOL *Identity,Loaded;
  BOOLEAN Registered,InTake,Captured,Prepared,Entered;
  BOOLEAN FileMode,FileLoading,FileQuiet;
  PIANO_ESP_BOOT_SOURCE File;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime;
  CONST UINT8 *Accepted;UINTN AcceptedBytes;UINT8 AcceptedSha[32];
  PIANO_FASTBOOT_DOWNLOAD_BLOB Adapter;PIANO_LAUNCH_BLOB Blob;VOID *Token,*Loan;
  CONST VOID *View;PIANO_BOOT_IMAGE Parsed;PIANO_RAW_LINUX_REPORT Report;
  CONST PIANO_PRODUCT_OWNERS *Owners;PIANO_PRODUCT_OWNERS_REPORT Clean;PIANO_PRODUCT_OWNERS_CONFIG Config;
} RAW_STATE;
STATIC RAW_STATE mRaw;
STATIC BOOLEAN Overlap(UINT64 A,UINT64 N,UINT64 B,UINT64 M){return A>MAX_UINT64-N||B>MAX_UINT64-M||(A<B+M&&B<A+N);}
STATIC BOOLEAN Live(VOID){return mRaw.SystemTable&&mRaw.SystemTable->BootServices==gBS;}
STATIC BOOLEAN SameImage(VOID){CONST EFI_LOADED_IMAGE_PROTOCOL *L=mRaw.Identity;return L&&
 L->Revision==mRaw.Loaded.Revision&&L->ImageBase==mRaw.Loaded.ImageBase&&L->ImageSize==mRaw.Loaded.ImageSize&&
 L->SystemTable==mRaw.Loaded.SystemTable&&L->ParentHandle==mRaw.Loaded.ParentHandle&&
 L->ImageCodeType==mRaw.Loaded.ImageCodeType&&L->ImageDataType==mRaw.Loaded.ImageDataType;}
STATIC BOOLEAN Clean(CONST PIANO_PRODUCT_OWNERS *O){
 if(!O)return FALSE;CONST PIANO_PRODUCT_OWNERS_REPORT *R=&O->Report;
 return R->Revision==1&&R->Initialized&&R->Phase==PianoProductOwnersClean&&R->Status==EFI_SUCCESS&&R->Clean&&
 !R->Busy&&!R->Retained&&!R->ServicesLost&&!R->OuterTplHeld&&R->ManagerEventClosed&&
 R->PolicyStopped&&PianoProductOwnersUsbRetired(O)&&R->BridgeStopped&&R->UfsStopped&&R->InputStopped&&
 (!(R->RegisteredStartedMask&PIANO_OWNER_DISPLAY)||R->DisplayStopped)&&
 O->Config.ExpectedOwnerMask==PIANO_OWNER_ALL_MASK&&R->RegisteredStartedMask==O->Config.StartedOwnerMask&&
 R->RegisteredAbsentMask==O->Config.AbsentOwnerMask&&R->RetiredMask==R->RegisteredStartedMask&&
 !(R->RegisteredStartedMask&R->RegisteredAbsentMask)&&(R->RegisteredStartedMask|R->RegisteredAbsentMask)==PIANO_OWNER_ALL_MASK&&
 R->RequestedAction==PianoUsbServiceActionBoot&&R->AllowedAction==PianoUsbServiceActionBoot&&
 (mRaw.FileMode?(R->Origin==PianoProductRequestFile&&R->FileContext==&mRaw&&R->FileToken==mRaw.Token&&
 !R->BootActionConsumed&&!R->Boot.Token&&!R->Boot.Taken):
 (R->Origin==PianoProductRequestUsb&&R->BootActionConsumed&&
 R->Boot.Context==&mRaw&&R->Boot.Token==mRaw.Token&&R->Boot.Status==EFI_SUCCESS&&R->Boot.Taken&&!R->Boot.Retained&&
 R->Boot.Proof.AckCompleted&&R->Boot.Proof.QueueEmpty&&R->Boot.Proof.DeviceHalted&&R->Boot.Proof.DmaFreed&&
 R->Boot.Proof.DispatchFrozen&&R->Boot.Proof.AckBytes==4&&R->Boot.Proof.DmaBuffersFreed==9));
}
STATIC BOOLEAN Download(CONST PIANO_FASTBOOT *F){return F&&F->Download&&F->Complete&&!F->Receiving&&
 F->Expected&&F->Expected<=PIANO_FASTBOOT_MAX_DOWNLOAD&&F->Received==F->Expected&&
 F->UploadBorrowed&&F->Upload==F->Download&&F->UploadBytes==F->Received&&!F->RebootRequested&&!F->ExitRequested;}
STATIC EFI_STATUS WireRead(VOID *C,UINT64 O,UINTN N,VOID *B){CONST PIANO_FASTBOOT *F=C;
 if(!Download(F)||O>F->Received||N>F->Received-O)return EFI_BAD_BUFFER_SIZE;
 CopyMem(B,F->Download+(UINTN)O,N);return EFI_SUCCESS;}
STATIC EFI_STATUS OwnedRead(VOID *C,UINT64 O,UINTN N,VOID *B){RAW_STATE *S=C;
 return S==&mRaw&&S->Captured?S->Blob.Read(S->Blob.Context,S->Token,O,N,B):EFI_ACCESS_DENIED;}
STATIC EFI_STATUS Parse(CONST PIANO_BOOT_SOURCE *Source,PIANO_BOOT_IMAGE *P){
 EFI_STATUS E=PianoFastbootBootParse(Source,P);if(E!=EFI_SUCCESS)return E;
 if(Source->Bytes>PIANO_FASTBOOT_MAX_DOWNLOAD||P->Kind!=PianoBootAndroid||P->Version!=2||
    P->Kernel.Bytes<64||P->Kernel.Bytes>PIANO_FASTBOOT_MAX_DOWNLOAD||!P->Ramdisk.Bytes||P->Ramdisk.Bytes>RAW_INITRD_MAX||
    P->Dtb.Bytes<40||P->Dtb.Bytes>RAW_DTB_MAX-0x10000||P->Second.Bytes||P->RecoveryDtbo.Bytes||P->Signature.Bytes||P->Trailing.Bytes)
   return EFI_UNSUPPORTED;
 UINT8 H[64];E=Source->Read(Source->Context,P->Kernel.Offset,sizeof(H),H);if(E!=EFI_SUCCESS)return E;
 UINT64 Offset=ReadUnaligned64((CONST UINT64 *)(H+8)),Span=ReadUnaligned64((CONST UINT64 *)(H+16));
 if(ReadUnaligned32((CONST UINT32 *)(H+56))!=0x644d5241||Offset>0x100000||Offset%4096||
    (ReadUnaligned64((CONST UINT64 *)(H+24))&1)||Span<P->Kernel.Bytes||Span>RAW_DTB-RAW_KERNEL-Offset)return EFI_COMPROMISED_DATA;
 return EFI_SUCCESS;
}
STATIC EFI_STATUS Ready(VOID *C){return C==&mRaw&&mRaw.Registered&&!mRaw.FileLoading&&!mRaw.FileMode&&!mRaw.Captured&&!mRaw.Prepared&&Live()?EFI_SUCCESS:EFI_NOT_READY;}
STATIC EFI_STATUS Validate(VOID *C,CONST PIANO_FASTBOOT *F,CONST PIANO_FB_BOOT_VIEW *V){
 if(Ready(C)!=EFI_SUCCESS||!Download(F)||!V)return EFI_NOT_READY;
 PIANO_BOOT_SOURCE Source={(VOID *)F,WireRead,F->Received};PIANO_BOOT_IMAGE P;EFI_STATUS E=Parse(&Source,&P);
 if(E!=EFI_SUCCESS)return E;
 if(!V->Wrapped||V->Offset!=P.Kernel.Offset||V->Bytes!=P.Kernel.Bytes)return EFI_COMPROMISED_DATA;
 UINT8 Sha[32];if(!Sha256HashAll(F->Download,F->Received,Sha))return EFI_DEVICE_ERROR;
 // DWC revalidates after the actual IN completion. Never replace the hash
 // accepted before OKAY with newly changed contents during that second call.
 if(F->BootTransferFrozen)return F->Download==mRaw.Accepted&&F->Received==mRaw.AcceptedBytes&&
   !CompareMem(Sha,mRaw.AcceptedSha,32)?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
 CopyMem(mRaw.AcceptedSha,Sha,32);
 mRaw.Accepted=F->Download;mRaw.AcceptedBytes=F->Received;return EFI_SUCCESS;
}
STATIC EFI_STATUS Quiet(VOID *C){return C==&mRaw&&mRaw.InTake&&!mRaw.Captured&&Live()?EFI_SUCCESS:EFI_NOT_READY;}
STATIC EFI_STATUS Take(VOID *C,PIANO_FASTBOOT *F,CONST PIANO_FB_BOOT_VIEW *V,VOID **Token){
 if(!Token)return EFI_INVALID_PARAMETER;*Token=NULL;
 if(Ready(C)!=EFI_SUCCESS||!Download(F)||!V||F->Download!=mRaw.Accepted||F->Received!=mRaw.AcceptedBytes||
 !F->BootPending||!F->BootTransferFrozen||!F->BootProof.AckCompleted||!F->BootProof.QueueEmpty||!F->BootProof.DeviceHalted||
 !F->BootProof.DmaFreed||!F->BootProof.DispatchFrozen||F->BootProof.AckBytes!=4||F->BootProof.DmaBuffersFreed!=9)return EFI_ACCESS_DENIED;
 UINT8 Sha[32];if(!Sha256HashAll(F->Download,F->Received,Sha)||CompareMem(Sha,mRaw.AcceptedSha,32))return EFI_SECURITY_VIOLATION;
 mRaw.InTake=TRUE;EFI_STATUS E=PianoFastbootDownloadBlobBind(&mRaw.Adapter,F,&mRaw,Quiet,gBS->FreePool,&mRaw.Blob);
 if(E==EFI_SUCCESS)E=mRaw.Blob.Take(mRaw.Blob.Context,&mRaw.Token);
 mRaw.InTake=FALSE;if(mRaw.Token){mRaw.Captured=TRUE;*Token=mRaw.Token;}return E;
}
EFI_STATUS PianoRawLinuxRegister(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table){
 if(!Image||!Table||Table->BootServices!=gBS)return EFI_INVALID_PARAMETER;
 if(mRaw.Registered)return EFI_ALREADY_STARTED;
 EFI_LOADED_IMAGE_PROTOCOL *L=NULL;EFI_STATUS E=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&L);
 if(E!=EFI_SUCCESS)return E;
 if(!L||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION||!L->ImageBase||!L->ImageSize||L->ImageSize>MAX_UINTN-(UINTN)L->ImageBase||
 L->SystemTable!=Table||L->ImageCodeType!=EfiLoaderCode||L->ImageDataType!=EfiLoaderData)return EFI_COMPROMISED_DATA;
 mRaw.Image=Image;mRaw.SystemTable=Table;mRaw.Identity=L;mRaw.Loaded=*L;
 PIANO_FB_BOOT Backend={.Context=&mRaw,.MaxImageBytes=PIANO_FASTBOOT_MAX_DOWNLOAD,.Ready=Ready,.Validate=Validate,.TakeAfterAck=Take,.AllowRawLinux=TRUE};
 E=PianoDwc3SetBootForExperiment(&Backend);if(E==EFI_SUCCESS)mRaw.Registered=TRUE;return E;
}
STATIC BOOLEAN FileAlive(VOID *C){return C==&mRaw&&Live()&&SameImage()&&!mRaw.File.File.ServicesLost;}
STATIC EFI_STATUS FileSlice(VOID *C,UINTN Us){
 if(!FileAlive(C))return EFI_ABORTED;
 if(mRaw.FileQuiet)return EFI_SUCCESS;
 if(!mRaw.Runtime||!mRaw.Runtime->BootServicesAlive(mRaw.Runtime))return EFI_ABORTED;
 EFI_STATUS E=mRaw.Runtime->Pump(mRaw.Runtime,PIANO_PRODUCT_PUMP_APP,Us);
 return E==EFI_NOT_READY?EFI_SUCCESS:E;
}
EFI_STATUS PianoRawLinuxLoadStable(PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime,VOID **Context,VOID **Token){
 if(!Context||!Token||Context==Token)return EFI_INVALID_PARAMETER;*Context=NULL;*Token=NULL;
 if(Ready(&mRaw)!=EFI_SUCCESS||!Runtime||Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION||
 !Runtime->Pump||!Runtime->BootServicesAlive||!Runtime->BootServicesAlive(Runtime))return EFI_NOT_READY;
 if(mRaw.File.Released){
   if(!PianoEspBootReleased(&mRaw.File)||mRaw.File.File.ServicesLost)return EFI_ACCESS_DENIED;
   ZeroMem(&mRaw.File,sizeof(mRaw.File));
 }
 mRaw.FileQuiet=FALSE;
 mRaw.Runtime=Runtime;mRaw.FileLoading=TRUE;
 PIANO_CPU_INPUT_ENV Cpu={.Context=&mRaw,.BootServicesAlive=FileAlive,.ServiceSlice=FileSlice};
 EFI_STATUS E=PianoEspBootLoad(&mRaw.File,&Cpu);
 if(E==EFI_SUCCESS)E=Parse(&mRaw.File.Reader,&mRaw.Parsed);
 if(E!=EFI_SUCCESS){
   if(mRaw.File.Loaded){mRaw.FileQuiet=TRUE;EFI_STATUS R=PianoEspBootRelease(&mRaw.File);mRaw.FileQuiet=FALSE;if(R!=EFI_SUCCESS)E=R;}
   mRaw.FileLoading=FALSE;return E;
 }
 mRaw.Blob=mRaw.File.Blob;mRaw.Token=mRaw.File.Owner;CopyMem(mRaw.AcceptedSha,mRaw.File.File.Sha256,32);
 mRaw.AcceptedBytes=mRaw.File.File.Bytes;mRaw.FileMode=TRUE;mRaw.Captured=TRUE;mRaw.FileLoading=FALSE;
 *Context=&mRaw;*Token=mRaw.Token;return EFI_SUCCESS;
}
BOOLEAN PianoRawLinuxStableRetained(VOID){return mRaw.File.Retained||mRaw.File.File.Retained||mRaw.File.File.ServicesLost;}
EFI_STATUS PianoRawLinuxDiscardStable(VOID){
 if(!mRaw.FileMode||mRaw.Prepared)return EFI_ACCESS_DENIED;
 mRaw.FileQuiet=TRUE;EFI_STATUS E=PianoEspBootRelease(&mRaw.File);mRaw.FileQuiet=FALSE;
 if(E!=EFI_SUCCESS||!PianoEspBootReleased(&mRaw.File))return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E;
 mRaw.Captured=mRaw.FileMode=FALSE;mRaw.Token=mRaw.Loan=NULL;mRaw.View=NULL;ZeroMem(&mRaw.Blob,sizeof(mRaw.Blob));
 return EFI_SUCCESS;
}
STATIC BOOLEAN Destination(UINT64 A,UINT64 N){
 if(!N||A>MAX_UINT64-N||Overlap(A,N,(UINTN)mRaw.View,mRaw.Blob.Bytes)||
 Overlap(A,N,(UINTN)mRaw.Loaded.ImageBase,mRaw.Loaded.ImageSize))return FALSE;
 EFI_MEMORY_REGION_DESCRIPTOR *Map=NULL;UINT8 Count=0;GetMemoryMap(&Map,&Count);
 if(!Map)return FALSE;
 for(UINTN I=0;I<Count;++I)if(!AsciiStrCmp(Map[I].Name,"Kernel")&&Map[I].Address==RAW_KERNEL&&Map[I].Length==0x10000000&&
 Map[I].MemoryType==EfiReservedMemoryType&&Map[I].ResourceType==EFI_RESOURCE_SYSTEM_MEMORY&&
 Map[I].ArmAttributes==WRITE_BACK_XN&&
 A>=Map[I].Address&&A-Map[I].Address<Map[I].Length&&N<=Map[I].Length-(A-Map[I].Address))return TRUE;
 return FALSE;
}
STATIC BOOLEAN Hash(CONST VOID *P,UINTN N,UINT8 Out[32]){return Sha256HashAll(P,N,Out);}
EFI_STATUS PianoRawLinuxPrepare(PIANO_PRODUCT_OWNERS *O,EFI_HANDLE Image,CONST PIANO_RAW_LINUX_REPORT **Out){
 if(!Out)return EFI_INVALID_PARAMETER;*Out=NULL;
 if(!mRaw.Captured||mRaw.Prepared||Image!=mRaw.Image||!Live()||!SameImage()||!Clean(O))return EFI_ACCESS_DENIED;
 if(mRaw.FileMode&&!PianoEspBootOwned(&mRaw.File,mRaw.Token))return EFI_ACCESS_DENIED;
 mRaw.FileQuiet=TRUE; // actual owner ledger is already clean; never pump stopped USB/policy
 UINT64 El;__asm__ volatile("mrs %0, CurrentEL":"=r"(El));if(El!=4)return EFI_UNSUPPORTED;
 PIANO_BOOT_SOURCE Source={&mRaw,OwnedRead,mRaw.Blob.Bytes};EFI_STATUS E=Parse(&Source,&mRaw.Parsed);if(E!=EFI_SUCCESS)return E;
 E=mRaw.Blob.BorrowView(mRaw.Blob.Context,mRaw.Token,(PIANO_BOOT_RANGE){0,mRaw.Blob.Bytes},&mRaw.View,&mRaw.Loan);
 if(E!=EFI_SUCCESS)return E;
 if(mRaw.FileMode){mRaw.File.View=mRaw.View;mRaw.File.Loan=mRaw.Loan;}
 UINT8 Sha[32];if(!Hash(mRaw.View,(UINTN)mRaw.Blob.Bytes,Sha)||CompareMem(Sha,mRaw.AcceptedSha,32))return EFI_SECURITY_VIOLATION;
 CONST UINT8 *Bytes=mRaw.View;PIANO_BOOT_IMAGE *P=&mRaw.Parsed;CONST UINT8 *Kernel=Bytes+P->Kernel.Offset;
 UINT64 Offset=ReadUnaligned64((CONST UINT64 *)(Kernel+8)),Span=ReadUnaligned64((CONST UINT64 *)(Kernel+16)),Address=RAW_KERNEL+Offset;
 UINTN Capacity=(UINTN)P->Dtb.Bytes+0x10000;
 if(!Destination(Address,Span)||!Destination(RAW_DTB,Capacity)||!Destination(RAW_INITRD,P->Ramdisk.Bytes)||
 Overlap(Address,Span,RAW_DTB,Capacity)||Overlap(Address,Span,RAW_INITRD,P->Ramdisk.Bytes)||Overlap(RAW_DTB,Capacity,RAW_INITRD,P->Ramdisk.Bytes))return EFI_ACCESS_DENIED;
 // The full image loan was acquired from the selected producer before dereference.
 CONST VOID *InputDtb=Bytes+P->Dtb.Offset;
 if(((UINTN)InputDtb&7)||FdtCheckHeader(InputDtb)||FdtTotalSize(InputDtb)!=P->Dtb.Bytes||
 FdtPathOffset(InputDtb,"/cpus")<0||FdtPathOffset(InputDtb,"/reserved-memory")<0||FdtPathOffset(InputDtb,"/__fixups__")>=0)return EFI_COMPROMISED_DATA;
 PIANO_RAW_LINUX_REPORT R={.Revision=1,.Epoch=1,.Kernel=Address,.KernelBytes=P->Kernel.Bytes,.KernelSpan=Span,
 .Dtb=RAW_DTB,.Initrd=RAW_INITRD,.InitrdBytes=P->Ramdisk.Bytes,.Image=Image,.Identity=mRaw.Identity,.Token=mRaw.Token};
 if(!Hash(Kernel,(UINTN)P->Kernel.Bytes,R.KernelSha)||!Hash(Bytes+P->Ramdisk.Offset,(UINTN)P->Ramdisk.Bytes,R.InitrdSha))return EFI_DEVICE_ERROR;
 CopyMem((VOID *)(UINTN)Address,Kernel,(UINTN)P->Kernel.Bytes);
 if(Span>P->Kernel.Bytes)ZeroMem((VOID *)(UINTN)(Address+P->Kernel.Bytes),(UINTN)(Span-P->Kernel.Bytes));
 CopyMem((VOID *)(UINTN)RAW_INITRD,Bytes+P->Ramdisk.Offset,(UINTN)P->Ramdisk.Bytes);
 VOID *Tree=(VOID *)(UINTN)RAW_DTB;if(FdtOpenInto(InputDtb,Tree,(INT32)Capacity))return EFI_COMPROMISED_DATA;
 E=PianoPanelSelectDtb(Tree,Capacity);if(E!=EFI_SUCCESS)return E;
 INT32 Chosen=FdtPathOffset(Tree,"/chosen");if(Chosen<0)return EFI_COMPROMISED_DATA;
 UINT64 Start=SwapBytes64(RAW_INITRD),End=SwapBytes64(RAW_INITRD+P->Ramdisk.Bytes);
 CHAR8 Cmdline[1537];ZeroMem(Cmdline,sizeof(Cmdline));
 CopyMem(Cmdline,Bytes+P->Cmdline.Offset,(UINTN)P->Cmdline.Bytes);
 CopyMem(Cmdline+P->Cmdline.Bytes,Bytes+P->ExtraCmdline.Offset,(UINTN)P->ExtraCmdline.Bytes);
 if(FdtSetProp(Tree,Chosen,"linux,initrd-start",&Start,sizeof(Start))||FdtSetProp(Tree,Chosen,"linux,initrd-end",&End,sizeof(End))||
 FdtSetProp(Tree,Chosen,"bootargs",Cmdline,(INT32)AsciiStrLen(Cmdline)+1))return EFI_COMPROMISED_DATA;
 STATIC CONST CHAR8 *Drop[]={"kaslr-seed","rng-seed","linux,uefi-system-table","linux,uefi-mmap-start","linux,uefi-mmap-size","linux,uefi-mmap-desc-size","linux,uefi-mmap-desc-ver"};
 for(UINTN I=0;I<ARRAY_SIZE(Drop);++I){INT32 D=FdtDelProp(Tree,Chosen,Drop[I]);if(D&&D!=-1)return EFI_COMPROMISED_DATA;}
 if(FdtPack(Tree))return EFI_COMPROMISED_DATA;R.DtbBytes=FdtTotalSize(Tree);
 if(!Hash((VOID *)(UINTN)Address,(UINTN)R.KernelBytes,Sha)||CompareMem(Sha,R.KernelSha,32)||
 !Hash((VOID *)(UINTN)RAW_INITRD,(UINTN)R.InitrdBytes,Sha)||CompareMem(Sha,R.InitrdSha,32)||!Hash(Tree,(UINTN)R.DtbBytes,R.DtbSha))return EFI_SECURITY_VIOLATION;
 WriteBackInvalidateDataCacheRange((VOID *)(UINTN)Address,(UINTN)Span);
 WriteBackInvalidateDataCacheRange(Tree,Capacity);WriteBackInvalidateDataCacheRange((VOID *)(UINTN)RAW_INITRD,(UINTN)R.InitrdBytes);
 if(mRaw.FileMode){
   E=PianoEspBootRelease(&mRaw.File);if(E!=EFI_SUCCESS||!PianoEspBootReleased(&mRaw.File))return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E;
   mRaw.View=NULL;mRaw.Loan=NULL;
 }
 mRaw.Report=R;mRaw.Owners=O;mRaw.Clean=O->Report;mRaw.Config=O->Config;mRaw.Prepared=TRUE;*Out=&mRaw.Report;
 DEBUG((DEBUG_WARN,"PIANO_RAW_LINUX_READY kernel=%lx span=%lu dtb=%lx bytes=%lu initrd=%lx bytes=%lu owners_clean=1 full_ddr_efi=0\n",
 R.Kernel,R.KernelSpan,R.Dtb,R.DtbBytes,R.Initrd,R.InitrdBytes));return EFI_SUCCESS;
}
BOOLEAN PianoRawLinuxPrepared(CONST PIANO_RAW_LINUX_REPORT *R,CONST PIANO_PRODUCT_OWNERS *O,EFI_HANDLE Image){
 return R==&mRaw.Report&&mRaw.Prepared&&O==mRaw.Owners&&Image==mRaw.Image&&SameImage()&&Clean(O)&&
 !CompareMem(&O->Report,&mRaw.Clean,sizeof(mRaw.Clean))&&!CompareMem(&O->Config,&mRaw.Config,sizeof(mRaw.Config))&&
 (mRaw.FileMode?(PianoEspBootReleased(&mRaw.File)&&!mRaw.View&&!mRaw.Loan):
 (mRaw.Adapter.Taken&&!mRaw.Adapter.Consumed&&!mRaw.Adapter.Retained&&!mRaw.Adapter.ReleaseAttempted&&
 mRaw.Adapter.ActiveLoan==mRaw.Loan&&mRaw.Token==&mRaw.Adapter&&mRaw.Adapter.Owned==mRaw.View));
}
STATIC VOID __attribute__((naked,noreturn)) RawEnter(UINT64 Tree,UINT64 Entry){
 __asm__ volatile("mov x4, x1\nmsr daifset, #15\nmsr cntv_ctl_el0, xzr\nmrs x9, sctlr_el1\n"
 "mov x10, #5\nbic x9, x9, x10\ndsb sy\nmsr sctlr_el1, x9\nisb\nic iallu\ndsb sy\nisb\n"
 "mov x1, xzr\nmov x2, xzr\nmov x3, xzr\nbr x4\n");
}
EFI_STATUS PianoRawLinuxEnter(CONST PIANO_RAW_LINUX_REPORT *R){
 if(!PianoRawLinuxPrepared(R,mRaw.Owners,mRaw.Image)||mRaw.Entered||!Live())return EFI_ACCESS_DENIED;
 UINTN Size=0,Key,DescriptorSize=0;UINT32 Version;EFI_STATUS E=gBS->GetMemoryMap(&Size,NULL,&Key,&DescriptorSize,&Version);
 if(E!=EFI_BUFFER_TOO_SMALL||!DescriptorSize||DescriptorSize>MAX_UINTN/16||Size>MAX_UINTN-16*DescriptorSize)return EFI_COMPROMISED_DATA;
 UINTN Capacity=Size+16*DescriptorSize;EFI_MEMORY_DESCRIPTOR *Map=AllocatePool(Capacity);if(!Map)return EFI_OUT_OF_RESOURCES;
 mRaw.Entered=TRUE;
 UINT64 Tree=R->Dtb,Entry=R->Kernel;VOID *Self=mRaw.Loaded.ImageBase;UINTN SelfBytes=(UINTN)mRaw.Loaded.ImageSize;
 for(UINTN Attempt=0;Attempt<2;++Attempt){
   Size=Capacity;E=gBS->GetMemoryMap(&Size,Map,&Key,&DescriptorSize,&Version);if(E!=EFI_SUCCESS)return E;
   E=gBS->ExitBootServices(mRaw.Image,Key);if(E==EFI_SUCCESS){
     // No calls into BS, logging or stack-based transition after MMU is off.
     WriteBackInvalidateDataCacheRange(Self,SelfBytes);RawEnter(Tree,Entry);
   }
   if(E!=EFI_INVALID_PARAMETER)return E;
 }
 return E;
}
