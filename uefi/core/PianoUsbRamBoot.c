// SPDX-License-Identifier: BSD-2-Clause-Patent
// First physical RAM-boot experiment accepts only the built returning probe.
// Generic wire/parser support lives separately; this policy protects the
// initial device test from executing a different downloaded application.
#include "PianoUsbRamBoot.h"
#include "PianoUsbRamBootExperiment.h"
#include "PianoFastbootDownloadBlob.h"
#include "PianoRamBootProbe.h"
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
typedef struct {
  BOOLEAN Prepared,InTake,Captured,Delivered,ControllerClean;
  volatile BOOLEAN EbsObserved;
  EFI_EVENT EbsEvent;
  VOID *CapturedToken;
  PIANO_FASTBOOT_DOWNLOAD_BLOB Adapter;
  PIANO_LAUNCH_BLOB Underlying;
  PIANO_FASTBOOT_LAUNCH Launch;
  PIANO_USB_BOOT_RETIRE_REPORT Retire;
} SESSION;
STATIC SESSION mSession;
STATIC volatile EFI_STATUS mLastFailure;
STATIC CONST UINT8 mExpectedSha[32]={0xe4,0x2f,0x0a,0xe4,0x16,0xc1,0x84,0x3e,0xd1,0xdc,0xda,0xa8,0x73,0x01,0xf9,0xa3,
  0x4b,0x09,0x3b,0xec,0xec,0x9c,0x82,0x9d,0x46,0xce,0x03,0x38,0x80,0x38,0xd2,0xb9};
STATIC EFI_STATUS Ready(VOID *Context) {
  SESSION *S=Context;return S==&mSession && S->Prepared && !S->EbsObserved && !S->Captured?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS Validate(VOID *Context,CONST PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View) {
  if(Ready(Context)!=EFI_SUCCESS || Source==NULL || View==NULL || Source->Download==NULL ||
     !Source->Complete || Source->Receiving || Source->Received!=Source->Expected ||
     View->Bytes!=3584 || View->ImageBytes!=20480 || View->Offset>Source->Received ||
     View->Bytes>Source->Received-View->Offset)return EFI_SECURITY_VIOLATION;
  UINT8 Sha[32];
  if(!Sha256HashAll(Source->Download+(UINTN)View->Offset,(UINTN)View->Bytes,Sha))return EFI_DEVICE_ERROR;
  BOOLEAN Match=CompareMem(Sha,mExpectedSha,sizeof(Sha))==0;ZeroMem(Sha,sizeof(Sha));
  return Match?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
}
STATIC EFI_STATUS Quiet(VOID *Context) {
  SESSION *S=Context;return S==&mSession && S->Prepared && S->InTake && !S->Captured?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS TakeAfterAck(VOID *Context,PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View,VOID **Token) {
  SESSION *S=Context;if(Token==NULL)return EFI_INVALID_PARAMETER;*Token=NULL;
  if(Validate(Context,Source,View)!=EFI_SUCCESS || Source->BootPending!=TRUE || Source->BootTransferFrozen!=TRUE ||
     Source->BootProof.AckCompleted!=TRUE || Source->BootProof.QueueEmpty!=TRUE || Source->BootProof.DeviceHalted!=TRUE ||
     Source->BootProof.DmaFreed!=TRUE || Source->BootProof.DispatchFrozen!=TRUE || Source->BootProof.AckBytes!=4 ||
     Source->BootProof.DmaBuffersFreed!=9)return EFI_NOT_READY;
  S->InTake=TRUE;
  EFI_STATUS Status=PianoFastbootDownloadBlobBind(&S->Adapter,Source,S,Quiet,gBS->FreePool,&S->Underlying);
  if(Status==EFI_SUCCESS)Status=S->Underlying.Take(S->Underlying.Context,&S->CapturedToken);
  S->InTake=FALSE;
  if(S->CapturedToken!=NULL){S->Captured=TRUE;*Token=S->CapturedToken;}
  return Status;
}
// DWC has already handed ownership to the adapter. Launch takes it once from
// this carrier; it must not invoke the original download Take a second time.
STATIC EFI_STATUS CarrierTake(VOID *Context,VOID **Owner) {
  SESSION *S=Context;if(Owner==NULL)return EFI_INVALID_PARAMETER;*Owner=NULL;
  if(S!=&mSession || !S->Captured || S->Delivered || !S->ControllerClean)return EFI_NOT_READY;
  S->Delivered=TRUE;*Owner=S->CapturedToken;return EFI_SUCCESS;
}
STATIC BOOLEAN CarrierOwned(SESSION *S,VOID *Owner) {return S==&mSession && S->Captured && S->Delivered && Owner==S->CapturedToken;}
STATIC EFI_STATUS CarrierRead(VOID *C,VOID *O,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  SESSION *S=C;return CarrierOwned(S,O)?S->Underlying.Read(S->Underlying.Context,O,Offset,Bytes,Buffer):EFI_NOT_READY;
}
STATIC EFI_STATUS CarrierBorrow(VOID *C,VOID *O,PIANO_BOOT_RANGE R,CONST VOID **V,VOID **L) {
  SESSION *S=C;return CarrierOwned(S,O)?S->Underlying.BorrowView(S->Underlying.Context,O,R,V,L):EFI_NOT_READY;
}
STATIC EFI_STATUS CarrierUnborrow(VOID *C,VOID *O,VOID *L) {
  SESSION *S=C;return CarrierOwned(S,O)?S->Underlying.Unborrow(S->Underlying.Context,O,L):EFI_NOT_READY;
}
STATIC EFI_STATUS CarrierRestore(VOID *C,VOID *O) {(VOID)C;(VOID)O;return EFI_ACCESS_DENIED;}
STATIC EFI_STATUS CarrierRelease(VOID *C,VOID *O) {
  SESSION *S=C;return CarrierOwned(S,O)?S->Underlying.ZeroRelease(S->Underlying.Context,O):EFI_NOT_READY;
}
STATIC EFI_STATUS ShutdownAll(VOID *Context) {
  SESSION *S=Context;return S==&mSession && S->ControllerClean && S->Retire.Clean && !S->Retire.Retained &&
    S->Retire.Attempted && S->Retire.Returned && S->Retire.Result==EFI_SUCCESS && S->Retire.ClockReleaseMask==0xff &&
    S->Retire.DomainFreed && S->Retire.ClocksReleased && S->Retire.UfsAbsent && S->Retire.UsbAbsent &&
    S->Retire.OtherStreamsStable &&
    S->Retire.DeviceHalted && S->Retire.DmaFreed?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC BOOLEAN Alive(VOID *Context) {SESSION *S=Context;return S==&mSession && !S->EbsObserved;}
STATIC VOID EFIAPI EbsNotify(EFI_EVENT Event,VOID *Context) {(VOID)Event;((SESSION *)Context)->EbsObserved=TRUE;}
STATIC VOID FailStop(VOID *Context,EFI_STATUS Status) {
  // This also runs after a possible EBS signal. Store CPU-only evidence; no
  // DebugLib, SerialPortLib, allocation or Boot/Runtime Services invocation.
  (VOID)Context;mLastFailure=Status;
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
  CpuDeadLoop();
}
STATIC EFI_STATUS CloseFence(EFI_STATUS Status) {
  if(!Alive(&mSession))FailStop(&mSession,EFI_ABORTED);
  if(mSession.EbsEvent!=NULL) {
    EFI_STATUS Close=gBS->CloseEvent(mSession.EbsEvent);
    if(!Alive(&mSession))FailStop(&mSession,EFI_ABORTED);
    if(Close!=EFI_SUCCESS)FailStop(&mSession,EFI_ERROR(Close)?Close:EFI_DEVICE_ERROR);
    mSession.EbsEvent=NULL;
  }
  return Status;
}
STATIC UINT32 RecordCrc(CONST VOID *Buffer,UINTN Bytes) {
  CONST UINT8 *P=Buffer;UINT32 Value=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I){Value^=P[I];for(UINTN J=0;J<8;++J)Value=(Value>>1)^((Value&1)?0xedb88320U:0);}
  return ~Value;
}
STATIC EFI_STATUS ReadProbeRecord(EFI_HANDLE Parent) {
  EFI_GUID Guid=PIANO_RAM_BOOT_PROBE_GUID;PIANO_RAM_BOOT_PROBE_RECORD Record;
  UINTN Bytes=sizeof(Record);UINT32 Attributes=0;
  EFI_STATUS Status=gRT->GetVariable(PIANO_RAM_BOOT_PROBE_NAME,&Guid,&Attributes,&Bytes,&Record);
  if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
  if(Bytes!=sizeof(Record) || Attributes!=EFI_VARIABLE_BOOTSERVICE_ACCESS || Record.Signature!=PIANO_RAM_BOOT_PROBE_SIGNATURE ||
     Record.Revision!=1 || Record.Bytes!=sizeof(Record) || Record.ParentHandle!=(UINTN)Parent ||
     (Record.Flags&(BIT0|BIT1|BIT2))!=(BIT0|BIT1|BIT2) || Record.LoadedStatus!=EFI_SUCCESS || Record.CurrentEl!=4 ||
     Record.ImageBase==0 || Record.ImageBytes==0 || !mSession.Launch.ImageIdentityKnown ||
     Record.ImageBase!=(UINTN)mSession.Launch.ImageBaseIdentity || Record.ImageBytes!=mSession.Launch.ImageSizeIdentity)
    return EFI_COMPROMISED_DATA;
  UINT32 Crc=Record.Crc32;Record.Crc32=0;
  if(RecordCrc(&Record,sizeof(Record))!=Crc)return EFI_CRC_ERROR;
  DEBUG((DEBUG_WARN,"SUNUEFI_RAM_BOOT_PROBE_VERIFIED current_el=%lx image_base=%lx image_bytes=%lu flags=%x console=%lx crc=%08x volatile_only=1\n",
    Record.CurrentEl,Record.ImageBase,Record.ImageBytes,Record.Flags,Record.ConsoleStatus,Crc));
  return EFI_SUCCESS;
}
EFI_STATUS PianoRunUsbRamBoot(CONST VOID *Fdt,EFI_HANDLE Parent) {
  if(Parent==NULL || mSession.Prepared)return EFI_ALREADY_STARTED;
  mSession.Prepared=TRUE;
  EFI_STATUS Fence=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,EbsNotify,&mSession,&gEfiEventExitBootServicesGuid,&mSession.EbsEvent);
  if(Fence!=EFI_SUCCESS)return CloseFence(EFI_ERROR(Fence)?Fence:EFI_DEVICE_ERROR);
  if(mSession.EbsEvent==NULL)FailStop(&mSession,EFI_COMPROMISED_DATA);
  if(!Alive(&mSession))FailStop(&mSession,EFI_ABORTED);
  PIANO_FB_BOOT Backend={&mSession,0x100000,FALSE,Ready,Validate,TakeAfterAck,FALSE};
  PIANO_FB_BOOT_ACTION Action;BOOLEAN Reboot=FALSE;
  EFI_STATUS Status=PianoUsbControllerRunForRamBoot(Fdt,&Backend,&Action,&mSession.Retire,&Reboot);
  if(!Alive(&mSession))FailStop(&mSession,EFI_ABORTED);
  DEBUG((DEBUG_WARN,"SUNUEFI_RAM_BOOT_CONTROLLER status=%r clean=%u captured=%u action=%u retained=%u clocks=%x\n",
    Status,mSession.Retire.Clean,mSession.Captured,Action.Taken,mSession.Retire.Retained,mSession.Retire.ClockReleaseMask));
  if(Status!=EFI_SUCCESS || !mSession.Retire.Clean) {
    if(mSession.Captured || mSession.Retire.Retained)FailStop(&mSession,Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Status);
    return CloseFence(Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Status);
  }
  if(Reboot) {
    if(mSession.Captured || Action.Taken || Action.Retained || mSession.Retire.Retained || !mSession.Retire.Attempted ||
       !mSession.Retire.Returned || mSession.Retire.Result!=EFI_SUCCESS || !mSession.Retire.DomainFreed ||
       !mSession.Retire.ClocksReleased || mSession.Retire.ClockReleaseMask!=0xff || !mSession.Retire.UfsAbsent ||
       !mSession.Retire.UsbAbsent || !mSession.Retire.OtherStreamsStable)FailStop(&mSession,EFI_COMPROMISED_DATA);
    CloseFence(EFI_SUCCESS);gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);CpuDeadLoop();
  }
  if(!Action.Taken)return CloseFence(EFI_NOT_FOUND);
  if(Action.Retained || !mSession.Captured || Action.Context!=&mSession || Action.Token!=mSession.CapturedToken)
    FailStop(&mSession,EFI_COMPROMISED_DATA);
  mSession.ControllerClean=TRUE;
  PIANO_LAUNCH_BLOB Blob={&mSession,mSession.Underlying.Bytes,CarrierTake,CarrierRead,CarrierBorrow,CarrierUnborrow,CarrierRestore,CarrierRelease};
  PIANO_LAUNCH_ENV Env={.Context=&mSession,.Services=gBS,.ParentImage=Parent,.MaxSourceBytes=PIANO_FASTBOOT_MAX_DOWNLOAD,
    .MaxImageBytes=0x100000,.ShutdownAll=ShutdownAll,.BootServicesAlive=Alive,.FailStop=FailStop,.RestoreOnFailure=FALSE};
  Status=PianoFastbootLaunchInit(&mSession.Launch);
  if(Status!=EFI_SUCCESS)FailStop(&mSession,Status);
  if(Status==EFI_SUCCESS)Status=PianoFastbootLaunchRun(&mSession.Launch,&Env,&Blob,NULL,0);
  DEBUG((DEBUG_WARN,"SUNUEFI_RAM_BOOT_EXECUTION status=%r started=%u returned=%u unloaded=%u source_released=%u retained=%u\n",
    Status,mSession.Launch.Result.StartInvoked,mSession.Launch.Result.AppReturned,mSession.Launch.Result.ImageUnloaded,
    mSession.Launch.Result.BlobZeroReleased,mSession.Launch.Result.ResourcesRetained));
  if(mSession.Launch.Result.ResourcesRetained)FailStop(&mSession,Status);
  if(Status==EFI_SUCCESS)Status=ReadProbeRecord(Parent);
  if(!Alive(&mSession))FailStop(&mSession,EFI_ABORTED);
  Status=CloseFence(Status);
  DEBUG((DEBUG_WARN,"SUNUEFI_RAM_BOOT_RESULT status=%r\n",Status));return Status;
}
