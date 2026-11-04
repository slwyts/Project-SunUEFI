// SPDX-License-Identifier: BSD-2-Clause-Patent
// One resident product parent owns all applications and service lifetimes.
#include "PianoProductCore.h"
#include "PianoProductPayload.h"
#include "PianoProductOwners.h"
#include "PianoRawLinuxBoot.h"
#include "Components/os-boot/PianoEspBootSource.h"
#include "PianoFastbootBlockRead.h"
#include "PianoKeysLifecycle.h"
#include "PianoRamPartition.h"
#include "PianoUfsProductVolume.h"
#include "PianoProductSmem.h"
#include "PianoReset.h"
#include "PianoNvFile.h"
#include "PianoProductBootObjects.h"
#include "PianoProductMemory.h"
#include "PianoProductBootLog.h"
#include "PianoProductDisplayObserve.h"
#include "PianoDisplaySmmuObserve.h"
#include "PianoFrameBufferMappingObserve.h"
#include "PianoDisplayClockObserve.h"
#include "PianoProductDisplayOwner.h"
#include "LateHandoff/PianoLateHandoff.h"
#include "LateHandoff/PianoDeviceTreeFixup.h"
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
#include <Library/FdtLib.h>
VOID PianoProbeFoundation(VOID);
VOID PianoNativeSetObserver(VOID (*Observer)(CONST CHAR8 *,BOOLEAN));
VOID PianoProbeUfs(CONST VOID *Fdt);
VOID PianoUfsSetProbeAction(EFI_STATUS (*Action)(CONST VOID *));
EFI_STATUS PianoUfsReadOnlyDmaExperiment(CONST VOID *Fdt);
CONST CHAR8 *PianoUfsStartupStage(VOID);
CONST CHAR8 *PianoUfsStartupDiagnostic(VOID);
EFI_STATUS PianoStartKeys(CONST VOID *Fdt);
STATIC PIANO_PRODUCT_OWNERS mOwners;
STATIC PIANO_LATE_HANDOFF mLateHandoff;
STATIC PIANO_FV_APPLICATION mSystemdApplication;
STATIC EFI_HANDLE mSystemdEsp;
STATIC BOOLEAN mSystemdActive,mSystemdEntering;
STATIC CONST CHAR8 *mEfiImageStage="not-started";
STATIC EFI_STATUS mEfiImageStatus=EFI_NOT_STARTED;
STATIC BOOLEAN mUfsAttempted,mUfsStarted,mInputStarted;
STATIC EFI_STATUS mUfsStatus=EFI_NOT_STARTED;
STATIC UINT64 mCounterFrequency,mCounterStart,mCounterEnd;
STATIC PIANO_RAM_PARTITION_REPORT mRamInventory;
STATIC PIANO_UFS_PRODUCT_VOLUME mProductVolume;
STATIC PIANO_PRODUCT_DISPLAY_STARTUP_REPORT mDisplayStartup;
STATIC PIANO_SMMU_RETIRED_USB_PROOF mStartupUsbProof;
STATIC UINT32 mStartupTarget;
STATIC volatile BOOLEAN mBootLogExited;
STATIC EFI_EVENT mBootLogExitEvent;
STATIC BOOLEAN mBootLogEnabled,mBootLogCounterDown;
STATIC UINT64 mBootLogStart;
STATIC CONST CHAR8 *mBootLogStage="PAYLOAD";
STATIC VOID BootLogReturned(VOID);
STATIC VOID FailStop(EFI_STATUS Status);
EFI_STATUS PianoRegisterLinuxBootOption(VOID);
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
STATIC EFI_STATUS ProductReportStatus(VOID *Context,PIANO_FB_SEND Send,VOID *SendContext) {
  if(Context!=NULL || Send==NULL)return EFI_INVALID_PARAMETER;
  if(!BootLogAlive())return EFI_ABORTED;

  // Export saved startup evidence independently of the ramoops console. These
  // getters do not reread SMEM, probe devices, or authorize a Linux handoff.
  PIANO_PRODUCT_MEMORY_DIAGNOSTIC Memory;
  PIANO_DEVICE_TREE_FIXUP_DIAGNOSTIC Fixup;
  PianoProductMemoryGetDiagnostic(&Memory);
  PianoDeviceTreeFixupGetDiagnostic(&Fixup);

  // Fastboot INFO payloads are at most 64 bytes, including the INFO prefix.
  // The USB service has space for these rows plus its transport/log status.
  CHAR8 Lines[24][64];
  UINTN Count=0;
#define STATUS_LINE(...) AsciiSPrint(Lines[Count++],sizeof(Lines[0]),__VA_ARGS__)
  STATUS_LINE("INFOfixup stage=%a calls=%u",Fixup.Stage,Fixup.Calls);
  STATUS_LINE("INFOfixup status=%016lx flags=%x",Fixup.Status,Fixup.Flags);
  STATUS_LINE("INFOmem capture=%a",Memory.CaptureStage);
  STATUS_LINE("INFOmem status=%016lx index=%lx",Memory.CaptureStatus,Memory.CaptureIndex);
  STATUS_LINE("INFOdtb stage=%a",Memory.DtbStage);
  STATUS_LINE("INFOdtb status=%016lx index=%lx",Memory.DtbStatus,Memory.DtbIndex);
  STATUS_LINE("INFOcold hob=%016lx state=%016lx",Memory.ColdHobStatus,Memory.ColdStatus);
  STATUS_LINE("INFOcold publish=%016lx",Memory.ColdPublishStatus);
  STATUS_LINE("INFOcold native=%u rows=%u",Memory.ColdNativeCount,Memory.ColdCount);
  STATUS_LINE("INFOcold ram=%u reserved=%u",Memory.ColdRamCount,Memory.ColdReservedCount);
  STATUS_LINE("INFOcold bytes=%lx",Memory.ColdDramBytes);
  STATUS_LINE("INFOcold epoch=%lx input=%lx",Memory.ColdEpoch,Memory.ColdInputFingerprint);
  STATUS_LINE("INFOobjects hob=%016lx valid=%016lx",Memory.ObjectsHobStatus,Memory.ObjectsValidateStatus);
  STATUS_LINE("INFOobjects state=%016lx publish=%016lx",Memory.ObjectsStatus,Memory.ObjectsPublishStatus);
  STATUS_LINE("INFOobjects reason=%u count=%u loads=%u",Memory.ObjectsReason,Memory.ObjectsCount,Memory.ObjectsLoads);
  STATUS_LINE("INFOobjects epoch=%lx usecs=%lu",Memory.ObjectsEpoch,Memory.ObjectsElapsedUsecs);
  STATUS_LINE("INFOobjects handoff=%016lx dtb=%016lx",Memory.ObjectsHandoffStatus,Memory.ObjectsDtbStatus);
  STATUS_LINE("INFOearly hob=%016lx state=%016lx",Memory.EarlyHobStatus,Memory.EarlyStatus);
  STATUS_LINE("INFOearly publish=%016lx smem=%016lx",Memory.EarlyPublishStatus,Memory.SmemStatus);
  STATUS_LINE("INFOflags op=%u oc=%u ef=%u ep=%u ec=%u",Memory.ObjectsPublished,Memory.ObjectsCoherent,Memory.EarlyFinished,Memory.EarlyPublished,Memory.EarlyColdStateVerified);
  STATUS_LINE("INFOearly payload=%u faults=%u",Memory.EarlyRawPayloadBytes,Memory.EarlyRecoveredFaults);
  STATUS_LINE("INFOsmem version=%u banks=%u entries=%u",Memory.SmemRamVersion,Memory.SmemBankCount,Memory.SmemRawEntryCount);
  STATUS_LINE("INFOsmem reason=%u parsed=%u payload=%u",Memory.SmemReason,Memory.SmemParsed,Memory.RawPayloadCoherent);
  STATUS_LINE("INFOsmem metadata=%u repeated=%u early=%016lx",Memory.SmemRepeatedMetadataEqual,Memory.SmemRepeatedPayloadEqual,PianoProductEarlySmemStatus());
#undef STATUS_LINE

  for(UINTN Index=0;Index<Count;++Index) {
    if(!BootLogAlive())return EFI_ABORTED;
    EFI_STATUS Status=Send(SendContext,Lines[Index],AsciiStrLen(Lines[Index]));
    if(!BootLogAlive())return EFI_ABORTED;
    if(Status!=EFI_SUCCESS)return Status;
  }
  return EFI_SUCCESS;
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
  // Replay the saved handoff checks last so a bounded RAM-log tail retains
  // the actual refusal even after the startup inventories above are repeated.
  DEBUG((DEBUG_WARN,"PIANO_EFI_IMAGE_SAVED stage=%a status=%r\n",mEfiImageStage,mEfiImageStatus));
  PianoProductMemoryReemitDiagnostic();
  PianoLateHandoffReemitDiagnostic(&mLateHandoff);
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
  if(AsciiStrCmp(Name,"ClockDxe")==0)
    (VOID)PianoProductDisplayCheckpoint(Before?"BEFORE CLOCK":"AFTER CLOCK",Before?0:1);
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
  STATIC CONST CHAR8 *Names[]={"touchscreen","pogo_keyboard_touchpad","usb_host","runtime_nv_write","ufs_blockio_write","download_1GiB","general_os_boot"};
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
     !PianoProductDisplayOwnerRetained()){
    BootLogStage(mBootLogStage,Status);
    CONST CHAR16 *Detail=L"启动菜单退出失败";
    if(mOwners.Report.Retained){
      if(EFI_ERROR(mOwners.Report.NvStatus)&&mOwners.Report.NvStatus!=EFI_NOT_STARTED)Detail=L"设置未能保存";
      else if(EFI_ERROR(mOwners.Report.FilesystemsStatus))Detail=L"仍有文件或驱动没有关闭";
      else Detail=mOwners.Report.Ufs.Failed?L"存储驱动未能安全退出":L"系统交接失败";
    }else if(!AsciiStrnCmp(mBootLogStage,"UFS",3))Detail=L"存储初始化失败";
    else if(!AsciiStrCmp(mBootLogStage,"USB"))Detail=L"USB 初始化失败";
    else if(!AsciiStrCmp(mBootLogStage,"INPUT"))Detail=L"输入初始化失败";
    else if(!AsciiStrCmp(mBootLogStage,"PAYLOAD"))Detail=L"引导文件不可用";
    CHAR16 StorageDetail[112];
    if(!mUfsStarted && !AsciiStrnCmp(mBootLogStage,"UFS",3)){
      CONST CHAR8 *Diagnostic=PianoUfsStartupDiagnostic();
      if(Diagnostic && Diagnostic[0]){
        UINTN I=0,J=0;
        while(Detail[I] && I<ARRAY_SIZE(StorageDetail)-3){StorageDetail[I]=Detail[I];++I;}
        StorageDetail[I++]=' ';StorageDetail[I++]=' ';
        while(Diagnostic[J] && I<ARRAY_SIZE(StorageDetail)-1)StorageDetail[I++]=(UINT8)Diagnostic[J++];
        StorageDetail[I]=0;Detail=StorageDetail;
      }
    }
    if(BootLogAlive())(VOID)PianoProductBootLogMessage(L"未能启动系统",Detail,Status,TRUE);
    DEBUG((DEBUG_ERROR,"PIANO_START_FAILED stage=%a status=%r recovery=hold-power-reboot-volume-up-Android\n",mBootLogStage,Status));
  }
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
// The same installed-image and USB paths retain the existing retirement order.
STATIC EFI_STATUS CompleteCoreAction(EFI_HANDLE Image,PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime,EFI_HANDLE ExitImage) {
  EFI_STATUS Status=PianoProductOwnersResolveReturnedAction(&mOwners);
  if(ExitImage!=Image && Status==EFI_SUCCESS && mOwners.Report.Origin!=PianoProductRequestFile)return EFI_ABORTED;
  BOOLEAN FileLoaded=FALSE;
  if(Status==EFI_NOT_READY&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionBoot) {
    (VOID)PianoProductBootLogMessage(L"正在读取启动镜像",L"请稍候",EFI_SUCCESS,FALSE);
    VOID *FileContext=NULL,*FileToken=NULL;
    EFI_STATUS Load=PianoRawLinuxLoadStable(Runtime,&FileContext,&FileToken);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_ESP_STABLE_LOAD status=%r path=EFI/Piano/stable/boot.img\n",Load));
    if(PianoRawLinuxStableRetained())FailStop(Load);
    FileLoaded=Load==EFI_SUCCESS;
    // Loading pumps the real USB service. A host action may now take priority.
    Status=PianoProductOwnersResolveReturnedAction(&mOwners);
    if(Status==EFI_NOT_READY&&FileLoaded)Status=PianoProductOwnersRequestFileBoot(&mOwners,FileContext,FileToken);
    if(Status==EFI_NOT_READY&&!FileLoaded) {
      return Load;
    }
  }
  if(Status!=EFI_SUCCESS)FailStop(Status);
  if(FileLoaded&&mOwners.Report.Origin!=PianoProductRequestFile) {
    Status=PianoRawLinuxDiscardStable();if(Status!=EFI_SUCCESS)FailStop(Status);FileLoaded=FALSE;
  }
  if(ExitImage!=Image && mOwners.Report.Origin!=PianoProductRequestFile)return EFI_ABORTED;
  (VOID)PianoProductBootLogMessage(L"正在交接系统",L"正在保存设置并关闭存储",EFI_SUCCESS,FALSE);
  Status=PianoProductOwnersRetire(&mOwners);
  if(Status==EFI_NOT_READY&&FileLoaded&&mOwners.Report.Origin==PianoProductRequestUsb) {
    Status=PianoRawLinuxDiscardStable();if(Status!=EFI_SUCCESS)FailStop(Status);
    if(ExitImage!=Image)FailStop(EFI_ABORTED);
    Status=PianoProductOwnersRetire(&mOwners);
  }
  if(Status!=EFI_SUCCESS || !mOwners.Report.Clean)FailStop(Status);
  if(mOwners.Report.AllowedAction==PianoUsbServiceActionReboot || mOwners.Report.AllowedAction==PianoUsbServiceActionContinue) {
    Status=PianoLateHandoffArmReset(&mLateHandoff,Image);
    if(Status==EFI_SUCCESS)Status=PianoResetExecute(&mOwners,Image);
    FailStop(Status);
  }
  if(mOwners.Report.AllowedAction==PianoUsbServiceActionBoot){
    CONST PIANO_RAW_LINUX_REPORT *Raw=NULL;
    Status=PianoRawLinuxPrepare(&mOwners,Image,ExitImage,&Raw);
    if(Status==EFI_SUCCESS)Status=PianoLateHandoffArmRaw(&mLateHandoff,ExitImage,Raw);
    if(Status==EFI_SUCCESS)Status=PianoRawLinuxEnter(Raw);
    FailStop(Status==EFI_SUCCESS?EFI_ABORTED:Status);
  }
  FailStop(EFI_UNSUPPORTED);
  return EFI_ABORTED;
}
STATIC EFI_STATUS StartInstalledLinux(VOID *Context,PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime,EFI_HANDLE Child) {
  if(!mSystemdActive)return EFI_NOT_STARTED;
  if(mSystemdEntering || !Child || !mSystemdApplication.StartCalled || mSystemdApplication.StartReturned ||
     mSystemdApplication.Retained || !mSystemdApplication.Image || !mSystemdEsp)return EFI_ACCESS_DENIED;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  EFI_STATUS Status=gBS->HandleProtocol(Child,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(Status!=EFI_SUCCESS)return Status;
  if(!Loaded || Loaded->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION ||
     Loaded->ParentHandle!=mSystemdApplication.Image || Loaded->DeviceHandle!=mSystemdEsp ||
     Loaded->SystemTable!=gST || !Loaded->ImageBase || !Loaded->ImageSize ||
     Loaded->ImageSize>MAX_UINTN-(UINTN)Loaded->ImageBase ||
     Loaded->ImageCodeType!=EfiLoaderCode || Loaded->ImageDataType!=EfiLoaderData)return EFI_ACCESS_DENIED;
  mSystemdEntering=TRUE;
  Status=CompleteCoreAction((EFI_HANDLE)Context,Runtime,Child);
  // Only a failure before retirement can return to the still-running manager.
  mSystemdEntering=FALSE;return Status;
}
STATIC BOOLEAN EfiLoadedValid(CONST EFI_LOADED_IMAGE_PROTOCOL *L){
  return L && L->Revision>=EFI_LOADED_IMAGE_PROTOCOL_REVISION && L->SystemTable==gST &&
    L->ImageBase && L->ImageSize && L->ImageSize<=MAX_UINTN-(UINTN)L->ImageBase &&
    L->ImageCodeType==EfiLoaderCode && L->ImageDataType==EfiLoaderData;
}
STATIC EFI_STATUS ValidateEfiDeviceTree(VOID){
  if(gST->NumberOfTableEntries>1024 || (gST->NumberOfTableEntries&&!gST->ConfigurationTable))return EFI_COMPROMISED_DATA;
  CONST VOID *Tree=NULL;
  for(UINTN I=0;I<gST->NumberOfTableEntries;++I){
    if(!CompareMem(&gST->ConfigurationTable[I].VendorGuid,&gFdtTableGuid,sizeof(EFI_GUID))){
      if(Tree)return EFI_COMPROMISED_DATA;Tree=gST->ConfigurationTable[I].VendorTable;
    }
  }
  if(!Tree||((UINTN)Tree&7)||FdtCheckHeader(Tree)||FdtTotalSize(Tree)>0x200000)return EFI_NOT_READY;
  return PianoProductMemoryValidateDtb(Tree,FdtTotalSize(Tree));
}
STATIC EFI_STATUS EfiImageResult(CONST CHAR8 *Stage,EFI_STATUS Status){
  mEfiImageStage=Stage;mEfiImageStatus=Status;
  return Status;
}
STATIC EFI_STATUS ValidateInstalledEfiImage(VOID *Context,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *Identity){
  if(!mSystemdActive || mSystemdEntering || !Image || Image==Context || Image==mSystemdApplication.Image ||
     !mSystemdEsp || !mSystemdApplication.Busy || !mSystemdApplication.StartCalled ||
     mSystemdApplication.StartReturned || mSystemdApplication.EbsObserved || mSystemdApplication.Retained)
    return EfiImageResult("manager-state",EFI_ACCESS_DENIED);
  CONST PIANO_BOOT_POLICY_REPORT *Policy=PianoBootPolicyReport();
  if(!Policy || Policy->RequestedCoreAction!=PianoUsbServiceActionBoot || Policy->Retained || Policy->ServicesLost ||
     Policy->Dispatching || Policy->Pumping || Policy->ActiveAction!=PIANO_PRODUCT_ACTION_NONE)return EfiImageResult("policy",EFI_NOT_READY);
  EFI_LOADED_IMAGE_PROTOCOL *Manager=NULL,*Current=NULL;
  EFI_STATUS Status=gBS->HandleProtocol(mSystemdApplication.Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Manager);
  if(!BootLogAlive())return EFI_ABORTED;if(Status!=EFI_SUCCESS)return EfiImageResult("manager-protocol",Status);
  if(!EfiLoadedValid(Manager) || Manager!=mSystemdApplication.LoadedIdentity ||
     Manager->ImageBase!=mSystemdApplication.BaseIdentity || Manager->ImageSize!=mSystemdApplication.SizeIdentity ||
     Manager->ParentHandle!=(EFI_HANDLE)Context || Manager->DeviceHandle!=mSystemdEsp)return EfiImageResult("manager-identity",EFI_ACCESS_DENIED);
  EFI_HANDLE Seen[4];EFI_HANDLE CurrentImage=Image;
  // BLS loads the kernel directly. A UKI may additionally load its embedded
  // kernel from memory; only that inner frame may lack a filesystem handle.
  for(UINTN Depth=0;Depth<ARRAY_SIZE(Seen);++Depth){
    for(UINTN N=0;N<Depth;++N)if(Seen[N]==CurrentImage)return EfiImageResult("parent-cycle",EFI_ACCESS_DENIED);
    Seen[Depth]=CurrentImage;
    Status=gBS->HandleProtocol(CurrentImage,&gEfiLoadedImageProtocolGuid,(VOID **)&Current);
    if(!BootLogAlive())return EFI_ABORTED;if(Status!=EFI_SUCCESS)return EfiImageResult("child-protocol",Status);
    if(!EfiLoadedValid(Current) || (!Depth && Current!=Identity) ||
       (Current->DeviceHandle && Current->DeviceHandle!=mSystemdEsp))return EfiImageResult("child-identity",EFI_ACCESS_DENIED);
    if(Current->ParentHandle==mSystemdApplication.Image)
      return EfiImageResult("device-tree",Current->DeviceHandle==mSystemdEsp?ValidateEfiDeviceTree():EFI_ACCESS_DENIED);
    if(!Current->ParentHandle || Current->ParentHandle==(EFI_HANDLE)Context)return EfiImageResult("parent",EFI_ACCESS_DENIED);
    CurrentImage=Current->ParentHandle;
  }
  return EfiImageResult("parent-depth",EFI_ACCESS_DENIED);
}
EFI_STATUS EFIAPI PianoProductCoreEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable) {
  if(Image==NULL || SystemTable==NULL)return EFI_INVALID_PARAMETER;
  BootLogStart();
  // Obtain a genuinely validated handoff, not an application-supplied FDT.
  PIANO_PRODUCT_PAYLOAD_VIEW Source={0};CONST VOID *Fdt=NULL;
  EFI_STATUS Status=PianoProductAcquireSimpleInit(&Source);
  if(Status==EFI_SUCCESS)Status=PianoProductPayloadGetFdt(&Source,&Fdt);
  if(Status==EFI_SUCCESS)mStartupTarget=PianoProductPayloadBootTarget();
  if(Source.Lease!=NULL){EFI_STATUS Release=PianoProductReleaseSimpleInit(&Source);if(Release!=EFI_SUCCESS)FailStop(Release);}
  if(Status==EFI_SUCCESS && Fdt==NULL)Status=EFI_COMPROMISED_DATA;
  BootLogStage("PAYLOAD",Status);
  if(Status!=EFI_SUCCESS){BootLogReturned();return Status;}
  ReportRequiredBackends();
  Status=PianoResetInitialize(Fdt);
  DEBUG((DEBUG_WARN,"PIANO_RESET_LAYOUT status=%r\n",Status));
  (VOID)PianoProductBootObjectsReemit(BootLogAlive);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  ObserveDisplay("before-foundation");
  if(mStartupTarget==1)(VOID)PianoProductDisplayCheckpoint("BEFORE DRIVERS",0);
  PianoNativeSetObserver(ObserveNative);
  PianoProbeFoundation();
  PianoNativeSetObserver(NULL);
  if(!BootLogAlive())FailStop(EFI_ABORTED);
  ObserveDisplay("after-foundation");
  if(mStartupTarget==1)(VOID)PianoProductDisplayCheckpoint("AFTER DRIVERS",2);
  mDisplayStartup=(PIANO_PRODUCT_DISPLAY_STARTUP_REPORT){.Revision=1,.KnownNoSideEffects=TRUE,.Status=EFI_NOT_STARTED};
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_DISPLAY_START status=%r inherited_gop=1 display_owner_refs=0 unused_cesta_media_startup_deferred=1 dcd_startup_dependencies_deferred=1 runtime_clock_api_preserved=1\n",mDisplayStartup.Status));
  ObserveDisplay("after-display-lease");
  // SEC already captured SMEM before enabling translation. Keep that report
  // for diagnostics; a second DXE read neither authorizes RAM nor gates boot.
  (VOID)PianoProductCaptureEarlySmem();
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
  if(mStartupTarget==1)(VOID)PianoProductDisplayCheckpoint("AFTER UFS",3);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_UFS_START attempted=%u status=%r started=%u original_media_readonly=1\n",mUfsAttempted,mUfsStatus,mUfsStarted));
  if(!mUfsAttempted || !mUfsStarted){mBootLogStage=PianoUfsStartupStage();FailStop(mUfsStatus);}
  PIANO_UFS_WINDOW_IO ProductStorageIo={0};
  mBootLogStage="UFS VOLUME";
  Status=PianoUfsProductTransportIo(&mProductVolume,&ProductStorageIo);
  if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoUfsProductVolumeOpen(&mProductVolume,&ProductStorageIo);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_STORAGE_DISCOVERY status=%r provisioned=%u readonly_originals=1 format_performed=0\n",
    Status,mProductVolume.State.Provisioned));
  if(mProductVolume.State.Quarantined || mProductVolume.State.NeedsRecovery)FailStop(Status);
  if(Status==EFI_SUCCESS) {
    Status=PianoUfsProductTransportPublish(&mProductVolume);
    if(Status!=EFI_SUCCESS)FailStop(Status);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_STORAGE_FAT_PUBLISHED writable=1 container_headers_excluded=1 nv_slots_private=1\n"));
  } else if(Status!=EFI_NOT_FOUND)FailStop(Status);
  mBootLogStage="UFS";
  Status=PianoNvFileStart();
  DEBUG((DEBUG_WARN,"PIANO_NV_FILE_BACKEND status=%r\n",Status));
  Status=PianoFastbootBlockReadInit();if(Status!=EFI_SUCCESS)FailStop(Status);
  CONST PIANO_FB_STORAGE *Storage=PianoFastbootBlockReadStorage();
  if(Storage==NULL || Storage->Ready(Storage->Context)!=EFI_SUCCESS)FailStop(EFI_NOT_READY);
  mCounterFrequency=GetPerformanceCounterProperties(&mCounterStart,&mCounterEnd);
  if(!mCounterFrequency || mCounterStart==mCounterEnd)FailStop(EFI_UNSUPPORTED);
  PIANO_DWC3_SERVICE_CONFIG UsbConfig={.Context=NULL,.NowUs=NowUs,.Storage=Storage,
    .BeforeRamlog=ProductDebugReplay,.ReportStatus=ProductReportStatus};
  BootLogStage("USB",EFI_NOT_STARTED);
  Status=PianoRawLinuxRegister(Image,SystemTable);if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoUsbControllerServiceStart(Fdt,&UsbConfig);BootLogStage("USB",Status);
  BOOLEAN UsbStarted=Status==EFI_SUCCESS;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_USB_START status=%r resident_service=%u foreground_loop=0\n",Status,UsbStarted));
  if(!UsbStarted){
    EFI_STATUS Proof=Status==EFI_TIMEOUT?PianoUsbControllerGetStartupFailureProof(Fdt,&mStartupUsbProof):EFI_NOT_READY;
    if(Proof!=EFI_SUCCESS || !mStartupUsbProof.Valid)FailStop(Status);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_USB_STARTUP_ROLLBACK start_status=%r proof_status=%r esp_boot_available=1 usb_available=0\n",Status,Proof));
  }
  ObserveDisplay("after-usb");
  BootLogStage("MENU",EFI_NOT_STARTED);
  Status=PianoBootPolicyInitialize(Image);if(Status!=EFI_SUCCESS)FailStop(Status);
  (VOID)PianoRegisterLinuxBootOption();
  // Fdt was validated against the BootShim handoff and mapped input at PAYLOAD.
  INT32 Chosen=FdtPathOffset(Fdt,"/chosen"),BootArgsBytes=0;
  CONST CHAR8 *BootArgs=Chosen<0?NULL:FdtGetProp(Fdt,Chosen,"bootargs",&BootArgsBytes);
  Status=PianoBootPolicySetEntryBootArgs(BootArgs,BootArgsBytes>0?(UINTN)BootArgsBytes:0);
  if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoBootPolicySetStartupTarget(mStartupTarget);if(Status!=EFI_SUCCESS)FailStop(Status);
  if(!UsbStarted){Status=PianoBootPolicySetUsbStartupFailure(Fdt,&mStartupUsbProof);if(Status!=EFI_SUCCESS)FailStop(Status);}
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime=NULL;
  Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Runtime);
  if(Status!=EFI_SUCCESS || Runtime==NULL || Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION)FailStop(EFI_NOT_READY);
  // Extra owner bits are explicitly unstarted in this integration. Their
  // hardware backend is still incomplete; the manifest must disclose that.
  PIANO_PRODUCT_OWNERS_CONFIG Config={.Revision=1,.Fdt=Fdt,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,
    .StartedOwnerMask=(PIANO_OWNER_CORE_MASK&~(UsbStarted?0:PIANO_OWNER_USB))|(mDisplayStartup.Held?PIANO_OWNER_DISPLAY:0),
    .AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO|(UsbStarted?0:PIANO_OWNER_USB)|(mDisplayStartup.Held?0:PIANO_OWNER_DISPLAY),
    .StartupUsbProof=UsbStarted?NULL:&mStartupUsbProof,
    .Runtime=Runtime,.InputContext=NULL,.StopInput=StopInput,
    .DisplayContext=mDisplayStartup.LeaseContext,.StopDisplay=PianoProductDisplayStop,.DisplayStartup=mDisplayStartup};
  Status=PianoProductOwnersInitialize(&mOwners,&Config);if(Status!=EFI_SUCCESS)FailStop(Status);
  PIANO_LATE_HANDOFF_ENV Late={.Context=Image,.Services=gBS,.SystemTable=SystemTable,
    .ParentImage=Image,.Owners=&mOwners,.BootServicesAlive=LateServicesAlive,
    .CheckMemory=PianoProductMemoryCheck,.ValidateMemory=PianoProductMemoryValidate,.FailStop=LateFailStop,
    .ValidateRaw=PianoRawLinuxPrepared,.ValidateEfiImage=ValidateInstalledEfiImage};
  Status=PianoLateHandoffInitialize(&mLateHandoff,&Late);if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoDeviceTreeFixupInstall(BootLogAlive,PianoProductMemoryValidateDtb);if(Status!=EFI_SUCCESS)FailStop(Status);
  Status=PianoBootPolicySetLinuxEntry(StartInstalledLinux,Image);if(Status!=EFI_SUCCESS)FailStop(Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_LATE_EXIT provider_bound=1 phase=unarmed standard_efi=1\n"));
  Status=PianoBootPolicyStartupWindow(3000);
  if(Status!=EFI_SUCCESS)FailStop(Status);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_BOOT_WINDOW status=%r budget_ms=3000 f12_setup=1 esc_boot_menu=1 volume_boot_menu=%u usb_pumped=%u entry_hint=%u default_action=%u target=%u\n",Status,
    (UINT32)(PianoBootPolicyReport()->Entry==PianoBootEntryRecovery),UsbStarted,(UINT32)PianoBootPolicyReport()->Entry,PianoBootPolicyReport()->StartupDefaultAction,mStartupTarget));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_CORE_READY one_shared_core=1 auto_simpleinit=%u auto_stable=%u f12_setup=1 usb_background=%u\n",
    (UINT32)(PianoBootPolicyReport()->StartupDefaultAction==PIANO_PRODUCT_ACTION_SIMPLEINIT),(UINT32)(PianoBootPolicyReport()->StartupDefaultAction==PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE),UsbStarted));
  BootLogStage("MENU",EFI_SUCCESS);
  if(mStartupTarget==1)(VOID)PianoProductDisplayCheckpoint("BEFORE SIMPLEINIT",4);
  for(;;) {
    Status=PianoBootPolicyRun();
    PianoDwc3ServiceReplayStartup();
    if(Status==EFI_END_OF_FILE) {
      Status=PianoProductOwnersResolveReturnedAction(&mOwners);
      if(Status==EFI_NOT_READY && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionBoot) {
        (VOID)PianoProductBootLogMessage(L"正在启动系统",L"请稍候",EFI_SUCCESS,FALSE);
        Status=PianoEspBootFind(&mSystemdEsp);
        if(Status==EFI_SUCCESS) {
          mSystemdActive=TRUE;
          Status=PianoApplicationRunFile(Image,mSystemdEsp,L"\\EFI\\BOOT\\BOOTAA64.EFI",NULL,0x400000,&mSystemdApplication);
          mSystemdActive=FALSE;
          if(mSystemdApplication.Retained)FailStop(Status);
          if(mLateHandoff.Retained || mLateHandoff.RetireCalls || mLateHandoff.Phase==PianoExitClean)FailStop(Status);
          if(mLateHandoff.Phase==PianoExitArmed){
            EFI_STATUS Disarm=PianoLateHandoffDisarm(&mLateHandoff);if(Disarm!=EFI_SUCCESS)FailStop(Disarm);
          }
        }
        DEBUG((DEBUG_WARN,"PIANO_SYSTEMD_BOOT_RETURN status=%r\n",Status));
        mSystemdEsp=NULL;
        // A returned manager never authorized retirement. Preserve a real
        // host request that arrived while its files or menu were in use.
        Status=PianoProductOwnersResolveReturnedAction(&mOwners);
        if(Status==EFI_NOT_READY) {
          Status=PianoBootPolicyCancelStable();if(Status!=EFI_SUCCESS)FailStop(Status);
          continue;
        }
      }
      if(Status!=EFI_SUCCESS)FailStop(Status);
      Status=CompleteCoreAction(Image,Runtime,Image);
      FailStop(Status==EFI_SUCCESS?EFI_ABORTED:Status);
    }
    if(Status!=EFI_SUCCESS && Status!=EFI_ABORTED)FailStop(Status);
    Status=Runtime->Pump(Runtime,PIANO_PRODUCT_PUMP_APP,1000);
    if(Status!=EFI_SUCCESS && Status!=EFI_NOT_READY)FailStop(Status);
    gBS->Stall(1000);
  }
}
