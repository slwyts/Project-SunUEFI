// SPDX-License-Identifier: BSD-2-Clause-Patent
// One resident product parent owns all applications and service lifetimes.
#include "PianoProductCore.h"
#include "PianoProductPayload.h"
#include "PianoProductOwners.h"
#include "PianoFastbootBlockRead.h"
#include "PianoKeysLifecycle.h"
#include "PianoRamPartition.h"
#include "PianoUfsProductVolume.h"
#include "PianoProductStorageBaseline.h"
#include "PianoProductSmem.h"
#include "PianoProductBootObjects.h"
#include "PianoProductBootLog.h"
#include "PianoProductDisplayObserve.h"
#include "PianoDisplaySmmuObserve.h"
#include "PianoFrameBufferMappingObserve.h"
#include "PianoDisplayClockObserve.h"
#include "PianoProductDisplayOwner.h"
#include "LateHandoff/PianoLateHandoff.h"
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
VOID PianoProbeFoundation(VOID);
VOID PianoNativeSetObserver(VOID (*Observer)(CONST CHAR8 *,BOOLEAN));
VOID PianoProbeUfs(CONST VOID *Fdt);
VOID PianoUfsSetProbeAction(EFI_STATUS (*Action)(CONST VOID *));
EFI_STATUS PianoUfsReadOnlyDmaExperiment(CONST VOID *Fdt);
EFI_STATUS PianoStartKeys(CONST VOID *Fdt);
STATIC PIANO_PRODUCT_OWNERS mOwners;
STATIC PIANO_LATE_HANDOFF mLateHandoff;
STATIC BOOLEAN mUfsAttempted,mUfsStarted,mInputStarted;
STATIC EFI_STATUS mUfsStatus=EFI_NOT_STARTED;
STATIC UINT64 mCounterFrequency,mCounterStart,mCounterEnd;
STATIC PIANO_RAM_PARTITION_REPORT mRamInventory;
STATIC PIANO_UFS_PRODUCT_VOLUME mProductVolume;
STATIC PIANO_PRODUCT_DISPLAY_STARTUP_REPORT mDisplayStartup;
STATIC volatile BOOLEAN mBootLogExited;
STATIC EFI_EVENT mBootLogExitEvent;
STATIC BOOLEAN mBootLogEnabled,mBootLogCounterDown;
STATIC UINT64 mBootLogStart;
STATIC CONST CHAR8 *mBootLogStage="PAYLOAD";
STATIC VOID BootLogReturned(VOID);
STATIC VOID FailStop(EFI_STATUS Status);
STATIC BOOLEAN EFIAPI BootLogAlive(VOID) {
  return !mBootLogExited && !mOwners.Report.ServicesLost && gST!=NULL && gST->BootServices==gBS;
}
STATIC VOID EFIAPI BootLogExit(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mBootLogExited=TRUE;PianoProductDisplayFenceExit();
}
STATIC VOID BootLogStage(CONST CHAR8 *Name,EFI_STATUS Status) {
  mBootLogStage=Name;
  if(!mBootLogEnabled || !BootLogAlive())return;
  UINT64 Counter=GetPerformanceCounter();
  UINT64 Delta=mBootLogCounterDown?mBootLogStart-Counter:Counter-mBootLogStart;
  EFI_STATUS Paint=PianoProductBootLogStage(Name,Status,GetTimeInNanoSecond(Delta)/1000000);
  if(!BootLogAlive())CpuDeadLoop();
  if(Paint!=EFI_SUCCESS) {
    mBootLogEnabled=FALSE;
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_BOOTLOG status=%r stage=%a painting_disabled=1\n",Paint,Name));
  }
}
STATIC VOID BootLogStart(VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;UINT64 First,Last;
  if(!BootLogAlive())CpuDeadLoop();
  UINT64 Frequency=GetPerformanceCounterProperties(&First,&Last);
  if(!Frequency || First==Last)return;
  mBootLogCounterDown=First>Last;mBootLogStart=GetPerformanceCounter();
  EFI_STATUS Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,BootLogExit,NULL,
    &gEfiEventExitBootServicesGuid,&mBootLogExitEvent);
  if(!BootLogAlive())CpuDeadLoop();
  if(Status!=EFI_SUCCESS || mBootLogExitEvent==NULL){BootLogReturned();return;}
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(!BootLogAlive())CpuDeadLoop();
  if(Status==EFI_SUCCESS)Status=PianoProductBootLogInitialize(Gop,BootLogAlive);
  if(!BootLogAlive())CpuDeadLoop();
  mBootLogEnabled=Status==EFI_SUCCESS;
  BootLogStage("DISPLAY",Status);BootLogStage("PAYLOAD",EFI_NOT_STARTED);
}
STATIC VOID BootLogReturned(VOID) {
  // A returned application can be unloaded by BDS; its event callback must
  // not outlive that image. Retained/fail-stop and EBS paths never return.
  mBootLogEnabled=FALSE;
  if(mBootLogExitEvent!=NULL) {
    if(!BootLogAlive())CpuDeadLoop();
    EFI_STATUS Status=gBS->CloseEvent(mBootLogExitEvent);
    if(!BootLogAlive())CpuDeadLoop();
    if(Status!=EFI_SUCCESS)CpuDeadLoop();
    mBootLogExitEvent=NULL;
  }
}
STATIC EFI_STATUS ProductDebugReplay(VOID *Context) {
  if(Context!=NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS Memory=PianoProductSmemReemit(NULL);
  EFI_STATUS Display=PianoProductDisplayReemit(BootLogAlive);
  EFI_STATUS Translation=PianoDisplaySmmuReemit(BootLogAlive);
  EFI_STATUS CpuMapping=PianoFrameBufferMappingReemit(BootLogAlive);
  EFI_STATUS Clock=PianoDisplayClockReemit(BootLogAlive);
  EFI_STATUS DisplayOwner=PianoProductDisplayReplay();
  (VOID)PianoProductBootObjectsReemit(BootLogAlive);
  if(!BootLogAlive())return EFI_ABORTED;
  if(PianoProductDisplayRetained())return Display==EFI_SUCCESS?EFI_COMPROMISED_DATA:Display;
  if(PianoDisplaySmmuRetained())return Translation==EFI_SUCCESS?EFI_COMPROMISED_DATA:Translation;
  if(PianoFrameBufferMappingRetained())return CpuMapping==EFI_SUCCESS?EFI_COMPROMISED_DATA:CpuMapping;
  if(PianoDisplayClockRetained())return Clock==EFI_SUCCESS?EFI_COMPROMISED_DATA:Clock;
  if(PianoProductDisplayOwnerRetained())return DisplayOwner==EFI_SUCCESS?EFI_COMPROMISED_DATA:DisplayOwner;
  // Unavailable GOP inventory remains diagnostic output. It must not prevent
  // export of a valid saved memory report or create a readiness claim.
  return Memory;
}
STATIC VOID ObserveDisplay(CONST CHAR8 *Phase) {
  EFI_STATUS Status=PianoProductDisplayObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoProductDisplayRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  Status=PianoDisplaySmmuObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoDisplaySmmuRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  Status=PianoFrameBufferMappingObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoFrameBufferMappingRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  Status=PianoProductDisplayClockObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoDisplayClockRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  if(PianoProductDisplayOwnerRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
}
STATIC VOID ObserveNative(CONST CHAR8 *Name,BOOLEAN Before) {
  CHAR8 Phase[32];
  if(Name==NULL)FailStop(EFI_INVALID_PARAMETER);
  AsciiSPrint(Phase,sizeof(Phase),"%a:%a",Before?"pre":"post",Name);
  EFI_STATUS Status=PianoDisplaySmmuObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoDisplaySmmuRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  Status=PianoFrameBufferMappingObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoFrameBufferMappingRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
  Status=PianoDisplayClockObserve(Phase,BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  if(PianoDisplayClockRetained())FailStop(Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
}
STATIC VOID ReportRequiredBackends(VOID) {
  // Required remains true. Missing real hardware/startup is visible rather
  // than converted into a silent build-time feature switch or fake Ready.
  STATIC CONST CHAR8 *Names[]={"touchscreen","pogo_keyboard_touchpad","usb_host","persistent_variables","ufs_blockio_write","download_1GiB","general_os_boot"};
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I)
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_BACKEND name=%a required=1 initialized=0 status=%r release_ready=0\n",Names[I],EFI_NOT_READY));
}
STATIC EFI_STATUS InitUfs(CONST VOID *Fdt) {
  mUfsAttempted=TRUE;mUfsStatus=PianoUfsReadOnlyDmaExperiment(Fdt);
  mUfsStarted=mUfsStatus==EFI_SUCCESS;return mUfsStatus;
}
STATIC VOID FailStop(EFI_STATUS Status) {
  // A retained observer may still own an exception handler or have lost its
  // service lifetime. Do not make another display/protocol call on that path.
  if(!PianoProductDisplayRetained() && !PianoDisplaySmmuRetained() &&
     !PianoFrameBufferMappingRetained() && !PianoDisplayClockRetained() &&
     !PianoProductDisplayOwnerRetained())BootLogStage(mBootLogStage,Status);
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
  CpuDeadLoop();
}
STATIC BOOLEAN LateServicesAlive(VOID *Context) {
  (VOID)Context;
  return !mOwners.Report.ServicesLost && gST!=NULL && gST->BootServices==gBS;
}
STATIC VOID LateFailStop(VOID *Context,EFI_STATUS Status) {
  (VOID)Context;FailStop(Status);
}
STATIC EFI_STATUS LateMemoryUnavailable(VOID *Context,PIANO_LINUX_MEMORY_PROOF *Report) {
  (VOID)Context;
  if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->Status=EFI_NOT_READY;
  // The actual cold observer is bound, but the full-DDR ownership authority
  // and dynamic-reservation placements have not been established on-device.
  // A late exit provider cannot turn those diagnostic snapshots into permission.
  return EFI_NOT_READY;
}
STATIC EFI_STATUS LateValidateMemory(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Report) {
  (VOID)Context;(VOID)Report;return EFI_NOT_READY;
}
EFI_STATUS PianoProductLateArm(VOID *Context,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *Identity) {
  (VOID)Context;
  if(Image==NULL || Identity==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=PianoLateHandoffArm(&mLateHandoff,Image);
  if(Status!=EFI_SUCCESS)return Status;
  if(mLateHandoff.Identity!=Identity) {
    Status=PianoLateHandoffDisarm(&mLateHandoff);
    if(Status!=EFI_SUCCESS)FailStop(Status);
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoProductLateDisarm(VOID *Context,EFI_HANDLE Image) {
  (VOID)Context;
  if(Image==NULL || mLateHandoff.Image!=Image)return EFI_ACCESS_DENIED;
  return PianoLateHandoffDisarm(&mLateHandoff);
}
STATIC UINT64 NowUs(VOID *Context) {
  (VOID)Context;UINT64 Counter=GetPerformanceCounter();
  UINT64 Ticks=mCounterEnd>=mCounterStart?Counter-mCounterStart:mCounterStart-Counter;
  UINT64 Whole=Ticks/mCounterFrequency,Part=Ticks%mCounterFrequency;
  if(Whole>MAX_UINT64/1000000 || Part>MAX_UINT64/1000000)return MAX_UINT64;
  return Whole*1000000+Part*1000000/mCounterFrequency;
}
STATIC EFI_STATUS StopInput(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report) {
  (VOID)Context;if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->Started=mInputStarted;
  if(!mInputStarted)return Report->Status=EFI_NOT_READY;
  PIANO_KEYS_RETIRE_REPORT Keys;EFI_STATUS Status=PianoStopKeysForProduct(&Keys);
  Report->Returned=Keys.Returned;Report->Clean=Keys.Clean;Report->Retained=Keys.Retained;
  Report->Timer=Keys.TimerCancel!=EFI_SUCCESS?Keys.TimerCancel:Keys.TimerClose;
  Report->Protocols=Keys.Disconnect!=EFI_SUCCESS?Keys.Disconnect:Keys.Uninstall!=EFI_SUCCESS?Keys.Uninstall:Keys.WaitClose;
  Report->Status=Status;if(Status==EFI_SUCCESS && Keys.Clean)mInputStarted=FALSE;return Status;
}
EFI_STATUS EFIAPI PianoProductCoreEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable) {
  if(Image==NULL || SystemTable==NULL)return EFI_INVALID_PARAMETER;
  BootLogStart();
  // Obtain a genuinely validated handoff, not an application-supplied FDT.
  PIANO_PRODUCT_PAYLOAD_VIEW Source={0};CONST VOID *Fdt=NULL;
  EFI_STATUS Status=PianoProductAcquireSimpleInit(&Source);
  if(Status==EFI_SUCCESS)Status=PianoProductPayloadGetFdt(&Source,&Fdt);
  if(Source.Lease!=NULL){EFI_STATUS Release=PianoProductReleaseSimpleInit(&Source);if(Release!=EFI_SUCCESS)FailStop(Release);}
  if(Status==EFI_SUCCESS && Fdt==NULL)Status=EFI_COMPROMISED_DATA;
  BootLogStage("PAYLOAD",Status);
  if(Status!=EFI_SUCCESS){BootLogReturned();return Status;}
  ReportRequiredBackends();
  (VOID)PianoProductBootObjectsReemit(BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  ObserveDisplay("before-foundation");
  PianoNativeSetObserver(ObserveNative);
  PianoProbeFoundation();
  PianoNativeSetObserver(NULL);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  ObserveDisplay("after-foundation");
  Status=PianoProductDisplayStart(BootLogAlive);
  PianoProductDisplayStartup(&mDisplayStartup);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_DISPLAY_START status=%r held=%u owned=%u known_absent=%u required=1\n",
    Status,mDisplayStartup.Held,mDisplayStartup.OwnedReferences,mDisplayStartup.KnownNoSideEffects));
  if(PianoProductDisplayOwnerRetained()||mDisplayStartup.ServicesLost)FailStop(Status);
  if(Status!=EFI_SUCCESS&&!mDisplayStartup.KnownNoSideEffects)FailStop(Status);
  ObserveDisplay("after-display-lease");
  // Real protected SMEM observations precede product DMA owners. Failure with
  // exact handler cleanup leaves data unknown; retained ownership cannot be
  // carried into UFS/USB bring-up. DXE evidence never changes the early map.
  BootLogStage("SMEM",EFI_NOT_STARTED);
  Status=PianoProductObserveSmem();BootLogStage("SMEM",Status);
  if(PianoProductSmemRetained())FailStop(Status);
  // Bind the exact native Env implementation before calling its audited ABI.
  // This is a DDR/preloaded inventory, never permission to map or allocate RAM.
  Status=PianoRamPartitionInventory(FALSE,&mRamInventory);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_RAM_ABI status=%r present=%u identity=%u abi=%u native_get_calls=0\n",
    Status,mRamInventory.Present,mRamInventory.IdentityVerified,mRamInventory.AbiVerified));
  if(mRamInventory.Retained || Status==EFI_ABORTED)FailStop(Status);
  if(Status==EFI_SUCCESS && mRamInventory.IdentityVerified && mRamInventory.AbiVerified) {
    Status=PianoRamPartitionInventory(TRUE,&mRamInventory);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_RAM_INVENTORY status=%r valid=%u banks=%u preloaded=%lu fallback=%u ownership=0 high_allocation=0\n",
      Status,mRamInventory.DataValid,mRamInventory.BankCount,mRamInventory.PreloadedCount,mRamInventory.PotentialFallback));
    if(mRamInventory.Retained || Status==EFI_ABORTED)FailStop(Status);
    if(Status==EFI_SUCCESS && mRamInventory.DataValid) {
      for(UINTN I=0;I<mRamInventory.BankCount;++I)
        DEBUG((DEBUG_WARN,"PIANO_PRODUCT_RAM_BANK index=%u base=%lx bytes=%lx ownership=0\n",
          (UINT32)I,mRamInventory.Banks[I].Base,mRamInventory.Banks[I].AvailableLength));
      for(UINTN I=0;I<mRamInventory.PreloadedCount;++I)
        DEBUG((DEBUG_WARN,"PIANO_PRODUCT_RAM_PRELOADED index=%u base=%lx bytes=%lx raw_type=%u\n",
          (UINT32)I,mRamInventory.Preloaded[I].Base,mRamInventory.Preloaded[I].Size,mRamInventory.Preloaded[I].RawType));
    }
  }
  BootLogStage("INPUT",EFI_NOT_STARTED);
  Status=PianoStartKeys(Fdt);mInputStarted=Status==EFI_SUCCESS;BootLogStage("INPUT",Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_INPUT_START status=%r started=%u\n",Status,mInputStarted));
  if(Status!=EFI_SUCCESS)FailStop(Status);
  // The actual probe owns the clock/GDSC bring-up before the persistent UFS
  // action. Product never relies on an inherited ABL clock being sufficient.
  BootLogStage("UFS",EFI_NOT_STARTED);
  PianoUfsSetProbeAction(InitUfs);PianoProbeUfs(Fdt);BootLogStage("UFS",mUfsStatus);
  ObserveDisplay("after-ufs");
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_UFS_START attempted=%u status=%r started=%u original_media_readonly=1\n",mUfsAttempted,mUfsStatus,mUfsStarted));
  if(!mUfsAttempted || !mUfsStarted)FailStop(mUfsStatus);
  PIANO_UFS_WINDOW_IO ProductStorageIo={0};
  Status=PianoUfsProductTransportIo(&mProductVolume,&ProductStorageIo);
  if(Status!=EFI_SUCCESS)FailStop(Status);
  CONST PIANO_UFS_PRODUCT_ORIGINAL_GPT Original={
    {mProductStorageOriginalPrimary,sizeof(mProductStorageOriginalPrimary)},
    {mProductStorageOriginalEntries,sizeof(mProductStorageOriginalEntries)},
    {mProductStorageOriginalBackup,sizeof(mProductStorageOriginalBackup)}};
  Status=PianoUfsProductVolumeOpen(&mProductVolume,&Original,&ProductStorageIo);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_STORAGE_DISCOVERY status=%r provisioned=%u readonly_originals=1 format_performed=0\n",
    Status,mProductVolume.State.Provisioned));
  if(mProductVolume.State.Quarantined || mProductVolume.State.NeedsRecovery)FailStop(Status);
  if(Status==EFI_SUCCESS) {
    Status=PianoUfsProductTransportPublish(&mProductVolume);
    if(Status!=EFI_SUCCESS)FailStop(Status);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_STORAGE_FAT_PUBLISHED writable=1 container_headers_excluded=1 nv_slots_private=1\n"));
  } else if(Status!=EFI_NOT_FOUND)FailStop(Status);
  Status=PianoFastbootBlockReadInit();if(Status!=EFI_SUCCESS)FailStop(Status);
  CONST PIANO_FB_STORAGE *Storage=PianoFastbootBlockReadStorage();
  if(Storage==NULL || Storage->Ready(Storage->Context)!=EFI_SUCCESS)FailStop(EFI_NOT_READY);
  mCounterFrequency=GetPerformanceCounterProperties(&mCounterStart,&mCounterEnd);
  if(!mCounterFrequency || mCounterStart==mCounterEnd)FailStop(EFI_UNSUPPORTED);
  PIANO_DWC3_SERVICE_CONFIG UsbConfig={.Context=NULL,.NowUs=NowUs,.Storage=Storage,
    .BeforeRamlog=ProductDebugReplay};
  BootLogStage("USB",EFI_NOT_STARTED);
  Status=PianoUsbControllerServiceStart(Fdt,&UsbConfig);BootLogStage("USB",Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_USB_START status=%r resident_service=1 foreground_loop=0\n",Status));
  if(Status!=EFI_SUCCESS)FailStop(Status);
  ObserveDisplay("after-usb");
  BootLogStage("MENU",EFI_NOT_STARTED);
  Status=PianoBootPolicyInitialize(Image);if(Status!=EFI_SUCCESS)FailStop(Status);
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime=NULL;
  Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Runtime);
  if(Status!=EFI_SUCCESS || Runtime==NULL || Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION)FailStop(EFI_NOT_READY);
  // Extra owner bits are explicitly unstarted in this integration. Their
  // hardware backend is still incomplete; the manifest must disclose that.
  PIANO_PRODUCT_OWNERS_CONFIG Config={.Revision=1,.Fdt=Fdt,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,
    .StartedOwnerMask=PIANO_OWNER_CORE_MASK|(mDisplayStartup.Held?PIANO_OWNER_DISPLAY:0),
    .AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO|(mDisplayStartup.Held?0:PIANO_OWNER_DISPLAY),
    .Runtime=Runtime,.InputContext=NULL,.StopInput=StopInput,
    .DisplayContext=mDisplayStartup.LeaseContext,.StopDisplay=PianoProductDisplayStop,.DisplayStartup=mDisplayStartup};
  Status=PianoProductOwnersInitialize(&mOwners,&Config);if(Status!=EFI_SUCCESS)FailStop(Status);
  PIANO_LATE_HANDOFF_ENV Late={.Context=NULL,.Services=gBS,.SystemTable=SystemTable,
    .ParentImage=Image,.Owners=&mOwners,.BootServicesAlive=LateServicesAlive,
    .CheckMemory=LateMemoryUnavailable,.ValidateMemory=LateValidateMemory,.FailStop=LateFailStop};
  Status=PianoLateHandoffInitialize(&mLateHandoff,&Late);if(Status!=EFI_SUCCESS)FailStop(Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_LATE_EXIT provider_bound=1 phase=unarmed full_ddr_ready=0\n"));
  Status=PianoBootPolicyStartupWindow(3000);
  if(Status!=EFI_SUCCESS)FailStop(Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_BOOT_WINDOW status=%r budget_ms=3000 f12_setup=1 esc_boot_menu=1 usb_pumped=1\n",Status));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_CORE_READY one_shared_core=1 auto_simpleinit=1 f12_setup=1 usb_background=1\n"));
  BootLogStage("MENU",EFI_SUCCESS);
  for(;;) {
    Status=PianoBootPolicyRun();
    if(Status==EFI_END_OF_FILE) {
      // An acknowledged host action can arrive while a UI return is pending.
      // Observe its real service ledger before using an older UI reason.
      Status=PianoProductOwnersResolveReturnedAction(&mOwners);
      if(Status!=EFI_SUCCESS)FailStop(Status);
      Status=PianoProductOwnersRetire(&mOwners);
      if(Status!=EFI_SUCCESS || !mOwners.Report.Clean)FailStop(Status);
      if(mOwners.Report.AllowedAction==PianoUsbServiceActionReboot || mOwners.Report.AllowedAction==PianoUsbServiceActionContinue) {
        gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);FailStop(EFI_ABORTED);
      }
      // Boot token handoff needs the generalized OS loader. Never relabel a
      // retained token or a clean Stop as successful execution of that image.
      FailStop(EFI_UNSUPPORTED);
    }
    if(Status!=EFI_SUCCESS && Status!=EFI_ABORTED)FailStop(Status);
    Status=Runtime->Pump(Runtime,PIANO_PRODUCT_PUMP_APP,1000);
    if(Status!=EFI_SUCCESS && Status!=EFI_NOT_READY)FailStop(Status);
    gBS->Stall(1000);
  }
}
