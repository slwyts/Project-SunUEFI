// SPDX-License-Identifier: BSD-2-Clause-Patent
// Include the actual Core; GC removes startup hardware paths from this host test.
#define MDEPKG_NDEBUG
#include "../../uefi/core/product/PianoProductCore.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1};
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_SYSTEM_TABLE System;
STATIC EFI_LOADED_IMAGE_PROTOCOL ChildLoaded;
STATIC EFI_LOADED_IMAGE_PROTOCOL ManagerLoaded,InnerLoaded;
STATIC UINT32 DeviceTree[10];
STATIC EFI_CONFIGURATION_TABLE FdtTable;
EFI_GUID gFdtTableGuid={.Data1=2};
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL Runtime;
STATIC PIANO_BOOT_POLICY_REPORT Policy;
STATIC jmp_buf Escape;
STATIC UINTN CoreId,ManagerId,ChildId,InnerId,EspId,OtherId;
#define CORE ((EFI_HANDLE)&CoreId)
#define MANAGER ((EFI_HANDLE)&ManagerId)
#define CHILD ((EFI_HANDLE)&ChildId)
#define INNER ((EFI_HANDLE)&InnerId)
#define ESP ((EFI_HANDLE)&EspId)
#define OTHER ((EFI_HANDLE)&OtherId)
STATIC UINT32 Count,Handles;
STATIC EFI_STATUS HandleStatus,FailureStatus;
STATIC EFI_STATUS EspStatus,LoadStatus,HostStatus;
STATIC EFI_STATUS DisarmStatus;
STATIC BOOLEAN UsbHostActive;
STATIC EFI_DEVICE_PATH_PROTOCOL BootPath;
STATIC BOOLEAN MenuLaunch,CheckFallback;
STATIC UINTN HostLeaves,BootCancels,Disarms;
STATIC PIANO_USB_SERVICE_ACTION BootUsbAction;
STATIC EFI_STATUS CodeStatus;
STATIC struct {MEMMAP_DEVICE_PATH Memory;EFI_DEVICE_PATH_PROTOCOL End;} KernelPath;
STATIC UINT8 UkiBytes[8192];
STATIC UINT8 KernelBytes[8192];
STATIC UINT32 FileLoads;
STATIC UINTN Restricts,DiagnosticPumps,DiagnosticStalls,RuntimePumps;
STATIC UINT32 RetainedObservers;
STATIC EFI_STATUS RestrictStatus;
STATIC BOOLEAN ExitDuringDiagnosticPump;
STATIC BOOLEAN ResetDuringDiagnosticPump,ResetReached;
STATIC EFI_STATUS RetireStatus;
STATIC UINT32 Retires,ResetArms;
STATIC PIANO_USB_SERVICE_ACTION DiagnosticAction;
STATIC BOOLEAN DiagnosticResetBlocked;
STATIC CONST CHAR16 *FailureDetail;

STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Interface){
  assert(Guid==&gEfiLoadedImageProtocolGuid);++Handles;
  if(Image==MANAGER)*Interface=&ManagerLoaded;
  else if(Image==INNER)*Interface=&InnerLoaded;
  else {assert(Image==CHILD);*Interface=&ChildLoaded;}
  return HandleStatus;
}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
INT32 EFIAPI FdtCheckHeader(CONST VOID *Tree){assert(Tree==DeviceTree);return 0;}
EFI_STATUS PianoProductMemoryValidateDtb(CONST VOID *Tree,UINTN Bytes){assert(Tree==DeviceTree&&Bytes==sizeof(DeviceTree));return EFI_SUCCESS;}
EFI_STATUS PianoProductMemoryValidateCode(CONST VOID *Base,UINT64 Bytes){assert((Base==ManagerLoaded.ImageBase&&Bytes==ManagerLoaded.ImageSize)||(Base==mLinuxApplication.BaseIdentity&&Bytes==mLinuxApplication.SizeIdentity));return CodeStatus;}
BOOLEAN PianoUsbControllerHostFailed(VOID){return FALSE;}
VOID PianoProductMemoryGetDiagnostic(PIANO_PRODUCT_MEMORY_DIAGNOSTIC *Report){memset(Report,0,sizeof(*Report));Report->CaptureStatus=Report->DtbStatus=EFI_NOT_STARTED;}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &Policy;}
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable){(VOID)Enable;return FALSE;}
EFI_STATUS PianoUsbControllerServicePrepareBoot(CONST EFI_DEVICE_PATH_PROTOCOL *Path){
  assert(Path==&BootPath);++HostLeaves;return EFI_SUCCESS;
}
VOID PianoUsbControllerServiceFinishBoot(VOID){}
EFI_STATUS PianoBootPolicyCancelExit(VOID){++BootCancels;return EFI_SUCCESS;}
EFI_STATUS PianoStartBootOption(EFI_HANDLE Parent,EFI_HANDLE *Device,PIANO_FV_APPLICATION *Application,
  PIANO_BOOT_DISPATCH *Dispatch){
  assert(Parent==CORE&&Device==&mLinuxBootDevice&&Application==&mLinuxApplication&&!mLinuxActive);
  Application->StartCalled=FALSE;
  if(MenuLaunch){Dispatch->StartedMenu=TRUE;*Device=NULL;return LoadStatus;}
  *Device=EspStatus==EFI_SUCCESS?ESP:NULL;
  if(EspStatus!=EFI_SUCCESS)return EspStatus;
  assert(Dispatch->Selection==Policy.BootSelection && Dispatch->PrepareSystem(&BootPath)==EFI_SUCCESS && mLinuxActive);
  Application->StartCalled=Dispatch->StartedSystem=TRUE;
  if(CheckFallback)assert(!Dispatch->CanTryNextSystem(LoadStatus));
  ++FileLoads;return LoadStatus;
}
EFI_STATUS PianoProductOwnersResolveReturnedAction(PIANO_PRODUCT_OWNERS *Owners){assert(Owners==&mOwners);return HostStatus;}
EFI_STATUS PianoLateHandoffDisarm(PIANO_LATE_HANDOFF *Late){
  assert(Late==&mLateHandoff&&Late->Phase==PianoExitArmed);++Disarms;
  if(DisarmStatus==EFI_SUCCESS)Late->Phase=PianoExitUnarmed;
  return DisarmStatus;
}
BOOLEAN PianoProductDisplayRetained(VOID){return (RetainedObservers&BIT0)!=0;}
BOOLEAN PianoDisplaySmmuRetained(VOID){return (RetainedObservers&BIT1)!=0;}
BOOLEAN PianoFrameBufferMappingRetained(VOID){return (RetainedObservers&BIT2)!=0;}
BOOLEAN PianoDisplayClockRetained(VOID){return (RetainedObservers&BIT3)!=0;}
BOOLEAN PianoProductDisplayOwnerRetained(VOID){return (RetainedObservers&BIT4)!=0;}
EFI_STATUS PianoUsbControllerRestrictToDiagnostics(BOOLEAN AllowReboot){
  ++Restricts;assert(!Count&&!RuntimePumps);
  if(!AllowReboot)DiagnosticResetBlocked=TRUE;
  return RestrictStatus;
}
EFI_STATUS PianoUsbControllerPumpDiagnostics(UINTN BudgetUs){
  assert(BudgetUs==1000&&Restricts>=1&&mFailureDiagnostics&&!Count&&!RuntimePumps);
  ++DiagnosticPumps;
  if(ExitDuringDiagnosticPump){mBootLogExited=TRUE;System.BootServices=NULL;return EFI_SUCCESS;}
  if(ResetDuringDiagnosticPump && !DiagnosticResetBlocked)return EFI_SUCCESS;
  return DiagnosticPumps==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Usb){
  memset(Usb,0,sizeof(*Usb));Usb->Revision=1;Usb->Started=TRUE;Usb->HostActive=UsbHostActive;
  Usb->Phase=ResetDuringDiagnosticPump&&!DiagnosticResetBlocked?PianoUsbServiceStopRequested:PianoUsbServiceListening;
  Usb->Action=ResetDuringDiagnosticPump&&!DiagnosticResetBlocked?DiagnosticAction:PianoUsbServiceActionNone;
  if(BootUsbAction!=PianoUsbServiceActionNone){
    Usb->Phase=PianoUsbServiceStopRequested;Usb->Action=BootUsbAction;
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoProductOwnersRetire(PIANO_PRODUCT_OWNERS *Owners){
  assert(Owners==&mOwners&&!mFailureDiagnostics);++Retires;
  if(RetireStatus!=EFI_SUCCESS){Owners->Report.Retained=TRUE;return RetireStatus;}
  Owners->Report.Clean=TRUE;Owners->Report.AllowedAction=DiagnosticAction;
  return EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffArmReset(PIANO_LATE_HANDOFF *Late,EFI_HANDLE Image){
  assert(Late==&mLateHandoff&&Image==CORE&&Retires==1&&mOwners.Report.Clean);++ResetArms;return EFI_SUCCESS;
}
EFI_STATUS PianoResetExecute(CONST PIANO_PRODUCT_OWNERS *Owners,EFI_HANDLE Image){
  assert(Owners==&mOwners&&Image==CORE&&ResetArms==1&&Owners->Report.Clean);
  ResetReached=TRUE;longjmp(Escape,1);return EFI_ABORTED;
}
STATIC EFI_STATUS EFIAPI DiagnosticStall(UINTN Microseconds){
  assert(Microseconds==1000&&mFailureDiagnostics&&BootLogAlive());++DiagnosticStalls;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI UnexpectedRuntimePump(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Reason,UINTN BudgetUs){
  (VOID)This;(VOID)Reason;(VOID)BudgetUs;++RuntimePumps;assert(!"error screen must not dispatch runtime owners or actions");return EFI_DEVICE_ERROR;
}
CONST CHAR8 *PianoUfsStartupDiagnostic(VOID){return NULL;}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
UINT64 EFIAPI GetPerformanceCounter(VOID){assert(!"disabled boot log must not sample time");return 0;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks){(VOID)Ticks;return 0;}
EFI_STATUS PianoProductBootLogStage(CONST CHAR8 *Name,EFI_STATUS Status,UINT64 Elapsed){
  (VOID)Name;(VOID)Status;(VOID)Elapsed;assert(!"boot log is disabled");return EFI_SUCCESS;
}
EFI_STATUS PianoProductBootLogMessage(CONST CHAR16 *Title,CONST CHAR16 *Detail,EFI_STATUS Status,BOOLEAN Failed){
  assert(Title&&Detail);if(Failed){FailureStatus=Status;FailureDetail=Detail;}return EFI_SUCCESS;
}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(Escape,1);}

STATIC VOID Reset(VOID){
  memset(&Services,0,sizeof(Services));memset(&System,0,sizeof(System));
  memset(&mOwners,0,sizeof(mOwners));memset(&mLateHandoff,0,sizeof(mLateHandoff));
  memset(&mLinuxApplication,0,sizeof(mLinuxApplication));memset(&Policy,0,sizeof(Policy));
  memset(&Runtime,0,sizeof(Runtime));Runtime.Pump=UnexpectedRuntimePump;mOwners.Config.Runtime=&Runtime;
  memset(&ChildLoaded,0,sizeof(ChildLoaded));
  Services.HandleProtocol=Handle;gBS=&Services;gST=&System;System.BootServices=gBS;
  mBootLogEnabled=mBootLogExited=FALSE;mBootLogStage="MENU";
  mLinuxActive=TRUE;mLinuxBootDevice=ESP;
  mLinuxApplication.Image=MANAGER;mLinuxApplication.StartCalled=TRUE;
  ChildLoaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,
    .ParentHandle=MANAGER,.DeviceHandle=NULL,.SystemTable=&System,.ImageBase=&ChildId,
    .ImageSize=4096,.ImageCodeType=EfiLoaderCode,.ImageDataType=EfiLoaderData};
  ManagerLoaded=ChildLoaded;ManagerLoaded.ParentHandle=CORE;ManagerLoaded.DeviceHandle=ESP;
  ManagerLoaded.ImageBase=&ManagerId;
  mLinuxApplication.LoadedIdentity=&ManagerLoaded;mLinuxApplication.BaseIdentity=ManagerLoaded.ImageBase;
  mLinuxApplication.SizeIdentity=ManagerLoaded.ImageSize;mLinuxApplication.Busy=TRUE;
  InnerLoaded=ChildLoaded;InnerLoaded.ImageBase=&InnerId;
  memset(DeviceTree,0,sizeof(DeviceTree));DeviceTree[1]=__builtin_bswap32(sizeof(DeviceTree));
  FdtTable=(EFI_CONFIGURATION_TABLE){gFdtTableGuid,DeviceTree};System.ConfigurationTable=&FdtTable;System.NumberOfTableEntries=1;
  Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
  Count=Handles=0;HandleStatus=EFI_SUCCESS;
  FailureStatus=EFI_NOT_STARTED;
  FailureDetail=NULL;
  EspStatus=LoadStatus=EFI_SUCCESS;HostStatus=EFI_NOT_READY;FileLoads=0;
  MenuLaunch=CheckFallback=FALSE;HostLeaves=BootCancels=Disarms=0;
  DisarmStatus=EFI_SUCCESS;BootUsbAction=PianoUsbServiceActionNone;UsbHostActive=FALSE;
  mHandoffFailure=NULL;mHandoffStage="not-started";
  Restricts=DiagnosticPumps=DiagnosticStalls=RuntimePumps=RetainedObservers=0;
  RestrictStatus=EFI_NOT_READY;ExitDuringDiagnosticPump=FALSE;mFailureDiagnostics=FALSE;mFailureStatus=EFI_NOT_STARTED;
  ResetDuringDiagnosticPump=ResetReached=FALSE;RetireStatus=EFI_SUCCESS;Retires=ResetArms=0;
  DiagnosticAction=PianoUsbServiceActionReboot;
  DiagnosticResetBlocked=FALSE;
  CodeStatus=EFI_SUCCESS;
  memset(&KernelPath,0,sizeof(KernelPath));
  KernelPath.Memory.Header=(EFI_DEVICE_PATH_PROTOCOL){HARDWARE_DEVICE_PATH,HW_MEMMAP_DP,{sizeof(MEMMAP_DEVICE_PATH),0}};
  KernelPath.Memory.MemoryType=EfiLoaderData;
  KernelPath.Memory.StartingAddress=(UINTN)mLinuxApplication.BaseIdentity;
  KernelPath.Memory.EndingAddress=(UINTN)mLinuxApplication.BaseIdentity+mLinuxApplication.SizeIdentity-1;
  KernelPath.End=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
}
STATIC VOID Put16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
STATIC VOID Put32(UINT8 *P,UINT32 V){Put16(P,(UINT16)V);Put16(P+2,(UINT16)(V>>16));}
STATIC VOID DirectKernel(VOID){
  memset(UkiBytes,0,sizeof(UkiBytes));
  Put16(UkiBytes,0x5a4d);Put32(UkiBytes+60,64);Put32(UkiBytes+64,0x4550);
  Put16(UkiBytes+68,0xaa64);Put16(UkiBytes+70,1);Put16(UkiBytes+84,112);
  UINT8 *Opt=UkiBytes+88;Put16(Opt,0x20b);Put32(Opt+56,sizeof(UkiBytes));Put32(Opt+60,512);Put16(Opt+68,10);
  UINT8 *Section=Opt+112;memcpy(Section,".linux",6);Put32(Section+8,1024);Put32(Section+12,4096);
  UINT8 *Kernel=UkiBytes+4096;
  Put16(Kernel,0x5a4d);Put32(Kernel+60,64);Put32(Kernel+64,0x4550);
  Put16(Kernel+68,0xaa64);Put16(Kernel+70,1);Put16(Kernel+84,112);Put16(Kernel+86,2);
  Opt=Kernel+88;Put16(Opt,0x20b);Put32(Opt+16,4096);Put32(Opt+32,4096);Put32(Opt+36,512);
  Put32(Opt+56,8192);Put32(Opt+60,512);Put16(Opt+68,10);
  Section=Opt+112;memcpy(Section,".text",5);Put32(Section+8,512);Put32(Section+12,4096);
  Put32(Section+16,512);Put32(Section+20,512);Put32(Section+36,0x60000020);
  mLinuxApplication.BaseIdentity=UkiBytes;mLinuxApplication.SizeIdentity=sizeof(UkiBytes);
  KernelPath.Memory.StartingAddress=(UINTN)Kernel;KernelPath.Memory.EndingAddress=(UINTN)Kernel+1023;
  memset(KernelBytes,0,sizeof(KernelBytes));memcpy(KernelBytes+4096,Kernel+512,512);
  ManagerLoaded.ImageBase=KernelBytes;ManagerLoaded.ImageSize=sizeof(KernelBytes);
  ManagerLoaded.FilePath=&KernelPath.Memory.Header;
}
STATIC VOID EfiAdmission(VOID){
  Reset();assert(ValidateInstalledEfiImage(CORE,CHILD,&ChildLoaded)==EFI_SUCCESS&&!Count);
  Reset();ChildLoaded.ParentHandle=INNER;ChildLoaded.DeviceHandle=NULL;
  assert(ValidateInstalledEfiImage(CORE,CHILD,&ChildLoaded)==EFI_SUCCESS&&!Count);
#define BAD_EFI(Change) do{Reset();Change;assert(ValidateInstalledEfiImage(CORE,CHILD,&ChildLoaded)!=EFI_SUCCESS&&!Count);}while(0)
  BAD_EFI(mLinuxActive=FALSE);BAD_EFI(mLinuxApplication.StartReturned=TRUE);
  BAD_EFI(mLinuxApplication.Busy=FALSE);BAD_EFI(mLinuxApplication.EbsObserved=TRUE);
  BAD_EFI(ManagerLoaded.ParentHandle=OTHER);BAD_EFI(ManagerLoaded.DeviceHandle=OTHER);
  BAD_EFI(mLinuxApplication.LoadedIdentity=&ChildLoaded);BAD_EFI(ManagerLoaded.ImageSize++);
  BAD_EFI(ChildLoaded.ParentHandle=CORE);
  BAD_EFI(ChildLoaded.ParentHandle=CHILD);BAD_EFI(ChildLoaded.Revision=0);
  BAD_EFI(ChildLoaded.ParentHandle=INNER;InnerLoaded.ParentHandle=CHILD);
  BAD_EFI(System.NumberOfTableEntries=0);BAD_EFI(Policy.RequestedCoreAction=PianoUsbServiceActionReboot);
#undef BAD_EFI
  Reset();ChildLoaded.DeviceHandle=OTHER;
  assert(ValidateInstalledEfiImage(CORE,CHILD,&ChildLoaded)==EFI_SUCCESS);
  Reset();DirectKernel();assert(ValidateInstalledEfiImage(CORE,MANAGER,&ManagerLoaded)==EFI_SUCCESS);
#define BAD_DIRECT(Change) do{Reset();DirectKernel();Change;assert(ValidateInstalledEfiImage(CORE,MANAGER,&ManagerLoaded)!=EFI_SUCCESS);}while(0)
  BAD_DIRECT(ManagerLoaded.ParentHandle=OTHER);BAD_DIRECT(ManagerLoaded.DeviceHandle=OTHER);
  BAD_DIRECT(ManagerLoaded.FilePath=NULL);BAD_DIRECT(KernelPath.Memory.MemoryType=EfiConventionalMemory);
  BAD_DIRECT(KernelPath.Memory.StartingAddress--);BAD_DIRECT(KernelPath.Memory.EndingAddress+=2);
  BAD_DIRECT(ManagerLoaded.ImageSize++);BAD_DIRECT(UkiBytes[4096]=0);
  BAD_DIRECT(UkiBytes[200]='x');BAD_DIRECT(Put32(UkiBytes+208,8192));
  BAD_DIRECT(KernelBytes[4096]^=1);
  BAD_DIRECT(KernelPath.End.Type=HARDWARE_DEVICE_PATH);BAD_DIRECT(CodeStatus=EFI_ACCESS_DENIED);
#undef BAD_DIRECT
}
STATIC VOID UkiLoadFailure(VOID){
  CONST EFI_STATUS Errors[]={EFI_NOT_FOUND,EFI_TIMEOUT,EFI_LOAD_ERROR,EFI_SUCCESS};
  for(UINTN I=0;I<ARRAY_SIZE(Errors);++I){
    Reset();mLinuxActive=FALSE;LoadStatus=Errors[I];
    if(!setjmp(Escape)){(VOID)StartInstalledLinux(CORE);assert(!"failed UKI must not return to a countdown");}
    assert(FileLoads==1&&!mLinuxActive&&!mLinuxBootDevice&&mHandoffFailure);
    assert(!strcmp(mHandoffStage,"linux-boot-manager"));
    assert(FailureStatus==(Errors[I]==EFI_SUCCESS?EFI_ABORTED:Errors[I]));
    assert(!mOwners.Report.RetiredMask&&!mLateHandoff.RetireCalls);
  }
  Reset();EspStatus=EFI_NOT_FOUND;
  if(!setjmp(Escape)){(VOID)StartInstalledLinux(CORE);assert(!"missing ESP must not retry");}
  assert(!FileLoads&&FailureStatus==EFI_NOT_FOUND);
  Reset();MenuLaunch=TRUE;Policy.BootSelection=PianoBootAutomatic;
  assert(StartInstalledLinux(CORE)==EFI_NOT_READY&&!FileLoads&&!HostLeaves&&!BootCancels);
  Reset();EspStatus=EFI_NOT_FOUND;Policy.BootSelection=PianoBootAutomatic;
  assert(StartInstalledLinux(CORE)==EFI_NOT_READY&&!FileLoads&&!HostLeaves&&BootCancels==1);
}
STATIC VOID PrepareFallback(VOID){
  Reset();mLateHandoff.Phase=PianoExitArmed;
  mOwners.Report.Initialized=TRUE;mOwners.Report.Phase=PianoProductOwnersRunning;
  mOwners.Report.RegisteredStartedMask=PIANO_OWNER_USB;
  Policy.BootSelection=PianoBootAutomatic;Policy.Initialized=Policy.ProtocolInstalled=TRUE;
  mLinuxApplication.Busy=FALSE;mLinuxApplication.Image=NULL;
}
STATIC VOID BootFallback(VOID){
  PrepareFallback();
  assert(CanTryNextInstalledSystem(EFI_LOAD_ERROR)&&!mLinuxActive&&Disarms==1);
  assert(mLateHandoff.Phase==PianoExitUnarmed&&mLinuxApplication.StartCalled);

  PrepareFallback();UsbHostActive=TRUE;
  assert(CanTryNextInstalledSystem(EFI_LOAD_ERROR)&&!mLinuxActive&&Disarms==1);

  PrepareFallback();Policy.PendingAction=PIANO_PRODUCT_ACTION_SIMPLEINIT;
  assert(!CanTryNextInstalledSystem(EFI_LOAD_ERROR)&&Disarms==1);
  PrepareFallback();mOwners.Report.RegisteredStartedMask=0;HostStatus=EFI_ACCESS_DENIED;
  assert(!CanTryNextInstalledSystem(EFI_LOAD_ERROR)&&Disarms==1);

  PrepareFallback();BootUsbAction=PianoUsbServiceActionReboot;HostStatus=EFI_SUCCESS;
  CheckFallback=TRUE;LoadStatus=EFI_LOAD_ERROR;
  assert(StartInstalledLinux(CORE)==EFI_SUCCESS&&Disarms==1);
  assert(FileLoads==1&&HostLeaves==1&&!BootCancels&&!mLinuxActive&&!mLinuxBootDevice);

  PrepareFallback();DisarmStatus=EFI_DEVICE_ERROR;
  if(!setjmp(Escape)){(VOID)CanTryNextInstalledSystem(EFI_LOAD_ERROR);assert(!"failed disarm must prevent fallback");}
  assert(Disarms==1&&FailureStatus==EFI_DEVICE_ERROR);
  PrepareFallback();mLinuxApplication.EbsObserved=TRUE;mBootLogExited=TRUE;
  if(!setjmp(Escape)){(VOID)CanTryNextInstalledSystem(EFI_LOAD_ERROR);assert(!"cannot return after ExitBootServices");}
  assert(!Disarms&&mFailureStatus==EFI_LOAD_ERROR);
}
STATIC VOID PrepareErrorDiagnostics(VOID){
  Reset();Services.Stall=DiagnosticStall;RestrictStatus=EFI_SUCCESS;
  mOwners.Report.Initialized=TRUE;mOwners.Report.Phase=PianoProductOwnersRunning;
  mOwners.Config.StartedOwnerMask=PIANO_OWNER_USB|PIANO_OWNER_UFS;
}
STATIC VOID ErrorDiagnostics(VOID){
  PrepareErrorDiagnostics();
  if(!setjmp(Escape)){InstalledSystemWatchdogExpired(3000000000ULL);assert(FALSE);}
  assert(FailureStatus==EFI_TIMEOUT&&!Restricts&&!Retires&&!ResetReached);
  PrepareErrorDiagnostics();
  if(!setjmp(Escape)){FailStop(EFI_LOAD_ERROR);assert(!"error screen ends at the CPU halt");}
  assert(Restricts==1&&DiagnosticPumps==3&&DiagnosticStalls==2&&!mFailureDiagnostics);
  assert(mFailureStatus==EFI_LOAD_ERROR&&FailureStatus==EFI_LOAD_ERROR&&!Count&&!Handles&&!RuntimePumps);
  assert(!mOwners.Report.RetiredMask&&!mOwners.Report.Clean&&!mLateHandoff.RetireCalls);

  PrepareErrorDiagnostics();ResetDuringDiagnosticPump=TRUE;HostStatus=EFI_SUCCESS;
  mLinuxActive=FALSE;mLinuxApplication.Busy=FALSE;mLateHandoff.Env.ParentImage=CORE;
  if(!setjmp(Escape)){FailStop(EFI_OUT_OF_RESOURCES);assert(!"acknowledged reset must leave failure diagnostics");}
  assert(ResetReached&&Retires==1&&ResetArms==1&&!RuntimePumps);

  PrepareErrorDiagnostics();ResetDuringDiagnosticPump=TRUE;HostStatus=EFI_SUCCESS;
  DiagnosticAction=PianoUsbServiceActionContinue;
  mLinuxActive=FALSE;mLinuxApplication.Busy=FALSE;mLateHandoff.Env.ParentImage=CORE;
  if(!setjmp(Escape))FailStop(EFI_OUT_OF_RESOURCES);
  assert(ResetReached&&Retires==1&&ResetArms==1&&!RuntimePumps);

  PrepareErrorDiagnostics();ResetDuringDiagnosticPump=TRUE;DiagnosticAction=PianoUsbServiceActionBoot;
  mLinuxActive=FALSE;mLinuxApplication.Busy=FALSE;mLateHandoff.Env.ParentImage=CORE;
  if(!setjmp(Escape))FailStop(EFI_OUT_OF_RESOURCES);
  assert(!ResetReached&&!Retires&&!ResetArms&&!RuntimePumps);

  PrepareErrorDiagnostics();ResetDuringDiagnosticPump=TRUE;HostStatus=EFI_SUCCESS;RetireStatus=EFI_DEVICE_ERROR;
  mLinuxActive=FALSE;mLinuxApplication.Busy=FALSE;mLateHandoff.Env.ParentImage=CORE;
  if(!setjmp(Escape)){FailStop(EFI_OUT_OF_RESOURCES);assert(!"failed cleanup must halt without reset");}
  assert(!ResetReached&&Retires==1&&!ResetArms&&mOwners.Report.Retained);

  PrepareErrorDiagnostics();RestrictStatus=EFI_NOT_READY;
  if(!setjmp(Escape))FailStop(EFI_LOAD_ERROR);
  assert(Restricts==1&&!DiagnosticPumps&&!DiagnosticStalls&&!RuntimePumps&&!Count);

  PrepareErrorDiagnostics();ExitDuringDiagnosticPump=TRUE;
  if(!setjmp(Escape))FailStop(EFI_LOAD_ERROR);
  assert(Restricts==1&&DiagnosticPumps==1&&!DiagnosticStalls&&!mFailureDiagnostics&&!Count&&!RuntimePumps);

#define UNSAFE_DIAGNOSTICS(Change) do{ \
  PrepareErrorDiagnostics();Change; \
  if(!setjmp(Escape)){FailStop(EFI_LOAD_ERROR);assert(!"unsafe error screen must halt");} \
  assert(!Restricts&&!DiagnosticPumps&&!DiagnosticStalls&&!RuntimePumps&&!Count&&!Handles&&!mFailureDiagnostics); \
  assert(mFailureStatus==EFI_LOAD_ERROR); \
}while(0)
  UNSAFE_DIAGNOSTICS(mOwners.Report.ServicesLost=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.UsbStopped=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Usb.Attempted=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.RetiredMask=PIANO_OWNER_USB);
  UNSAFE_DIAGNOSTICS(mOwners.Report.OuterTplHeld=TRUE);
  for(UINTN I=0;I<5;++I){UNSAFE_DIAGNOSTICS(RetainedObservers=1U<<I);}
  UNSAFE_DIAGNOSTICS(mBootLogExited=TRUE);
  UNSAFE_DIAGNOSTICS(System.BootServices=NULL);
  UNSAFE_DIAGNOSTICS(gST=NULL);
  UNSAFE_DIAGNOSTICS(gBS=NULL);
  UNSAFE_DIAGNOSTICS(Services.Stall=NULL);
#undef UNSAFE_DIAGNOSTICS
  for(UINTN I=0;I<2;++I){
    PrepareErrorDiagnostics();mOwners.Report.Retained=TRUE;
    if(I)mOwners.Report.Pogo.Started=TRUE;else mOwners.Report.Touch.Started=TRUE;
    if(!setjmp(Escape))FailStop(EFI_DEVICE_ERROR);
    CONST CHAR16 *Expected=I?L"键盘驱动未能安全退出":L"触屏驱动未能安全退出";
    UINTN Length=0;while(Expected[Length])++Length;
    assert(FailureDetail&&!memcmp(FailureDetail,Expected,(Length+1)*sizeof(CHAR16)));
    assert(Restricts==1&&DiagnosticPumps==3&&DiagnosticResetBlocked&&!RuntimePumps);
  }
  PrepareErrorDiagnostics();mOwners.Report.PolicyStopped=TRUE;mOwners.Report.RetiredMask=PIANO_OWNER_POLICY;
  mOwners.Report.Retained=TRUE;mOwners.Report.Phase=PianoProductOwnersRetained;
  mLateHandoff.Busy=mLateHandoff.Retained=TRUE;mLateHandoff.Phase=PianoExitRetained;mLateHandoff.RetireCalls=1;
  if(!setjmp(Escape))FailStop(EFI_DEVICE_ERROR);
  assert(Restricts==1&&DiagnosticPumps==3&&DiagnosticResetBlocked&&!Retires&&!ResetReached);
}
int main(int Argc,char **Argv){
  assert(Argc==2);
  if(!strcmp(Argv[1],"efi-admission"))EfiAdmission();
  else if(!strcmp(Argv[1],"uki-load-failure"))UkiLoadFailure();
  else if(!strcmp(Argv[1],"boot-fallback"))BootFallback();
  else if(!strcmp(Argv[1],"error-diagnostics"))ErrorDiagnostics();
  else assert(!"unknown test case");
  puts("Actual resident Core UKI admission and failure diagnostics passed");return 0;
}
