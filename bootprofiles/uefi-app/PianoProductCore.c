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
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
VOID PianoProbeFoundation(VOID);
VOID PianoProbeUfs(CONST VOID *Fdt);
VOID PianoUfsSetProbeAction(EFI_STATUS (*Action)(CONST VOID *));
EFI_STATUS PianoUfsReadOnlyDmaExperiment(CONST VOID *Fdt);
EFI_STATUS PianoStartKeys(CONST VOID *Fdt);
STATIC PIANO_PRODUCT_OWNERS mOwners;
STATIC BOOLEAN mUfsAttempted,mUfsStarted,mInputStarted;
STATIC EFI_STATUS mUfsStatus=EFI_NOT_STARTED;
STATIC UINT64 mCounterFrequency,mCounterStart,mCounterEnd;
STATIC PIANO_RAM_PARTITION_REPORT mRamInventory;
STATIC PIANO_UFS_PRODUCT_VOLUME mProductVolume;
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
  (VOID)Status;
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
  CpuDeadLoop();
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
  // Obtain a genuinely validated handoff, not an application-supplied FDT.
  PIANO_PRODUCT_PAYLOAD_VIEW Source={0};CONST VOID *Fdt=NULL;
  EFI_STATUS Status=PianoProductAcquireSimpleInit(&Source);
  if(Status==EFI_SUCCESS)Status=PianoProductPayloadGetFdt(&Source,&Fdt);
  if(Source.Lease!=NULL){EFI_STATUS Release=PianoProductReleaseSimpleInit(&Source);if(Release!=EFI_SUCCESS)FailStop(Release);}
  if(Status!=EFI_SUCCESS || Fdt==NULL)return Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status;
  ReportRequiredBackends();
  PianoProbeFoundation();
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
  Status=PianoStartKeys(Fdt);mInputStarted=Status==EFI_SUCCESS;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_INPUT_START status=%r started=%u\n",Status,mInputStarted));
  if(Status!=EFI_SUCCESS)FailStop(Status);
  // The actual probe owns the clock/GDSC bring-up before the persistent UFS
  // action. Product never relies on an inherited ABL clock being sufficient.
  PianoUfsSetProbeAction(InitUfs);PianoProbeUfs(Fdt);
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
  PIANO_DWC3_SERVICE_CONFIG UsbConfig={.Context=NULL,.NowUs=NowUs,.Storage=Storage};
  Status=PianoUsbControllerServiceStart(Fdt,&UsbConfig);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_USB_START status=%r resident_service=1 foreground_loop=0\n",Status));
  if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoBootPolicyInitialize(Image);if(Status!=EFI_SUCCESS)FailStop(Status);
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime=NULL;
  Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Runtime);
  if(Status!=EFI_SUCCESS || Runtime==NULL || Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION)FailStop(EFI_NOT_READY);
  // Extra owner bits are explicitly unstarted in this integration. Their
  // hardware backend is still incomplete; the manifest must disclose that.
  PIANO_PRODUCT_OWNERS_CONFIG Config={.Revision=1,.Fdt=Fdt,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,
    .StartedOwnerMask=PIANO_OWNER_CORE_MASK,.AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO,
    .Runtime=Runtime,.InputContext=NULL,.StopInput=StopInput};
  Status=PianoProductOwnersInitialize(&mOwners,&Config);if(Status!=EFI_SUCCESS)FailStop(Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_CORE_READY one_shared_core=1 auto_simpleinit=1 f12_setup=1 usb_background=1\n"));
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
