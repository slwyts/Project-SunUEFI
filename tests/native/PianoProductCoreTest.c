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
STATIC UINT32 FileLoads;
STATIC UINTN Restricts,DiagnosticPumps,DiagnosticStalls,RuntimePumps;
STATIC UINT32 RetainedObservers;
STATIC EFI_STATUS RestrictStatus;
STATIC BOOLEAN ExitDuringDiagnosticPump;

STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Interface){
  assert(Guid==&gEfiLoadedImageProtocolGuid);++Handles;
  if(Image==MANAGER)*Interface=&ManagerLoaded;
  else if(Image==INNER)*Interface=&InnerLoaded;
  else {assert(Image==CHILD);*Interface=&ChildLoaded;}
  return HandleStatus;
}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
INT32 EFIAPI FdtCheckHeader(CONST VOID *Tree){assert(Tree==DeviceTree);return 0;}
EFI_STATUS PianoProductMemoryValidateDtb(CONST VOID *Tree,UINTN Bytes){assert(Tree==DeviceTree&&Bytes==sizeof(DeviceTree));return EFI_SUCCESS;}
BOOLEAN PianoUsbControllerHostBlocked(VOID){return FALSE;}
VOID PianoProductMemoryGetDiagnostic(PIANO_PRODUCT_MEMORY_DIAGNOSTIC *Report){memset(Report,0,sizeof(*Report));Report->CaptureStatus=Report->DtbStatus=EFI_NOT_STARTED;}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &Policy;}
EFI_STATUS PianoEspBootFind(EFI_HANDLE *Esp){*Esp=EspStatus==EFI_SUCCESS?ESP:NULL;return EspStatus;}
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable){(VOID)Enable;return FALSE;}
EFI_STATUS PianoApplicationRunFile(EFI_HANDLE Parent,EFI_HANDLE Device,CONST CHAR16 *Path,
  CONST CHAR16 *Options,UINT64 Limit,PIANO_FV_APPLICATION *Application){
  STATIC CONST CHAR16 Expected[]=L"\\EFI\\Linux\\sunuefi.efi";
  assert(Parent==CORE&&Device==ESP&&!memcmp(Path,Expected,sizeof(Expected))&&!Options);
  assert(Limit==0x10000000&&Application==&mLinuxApplication&&mLinuxActive);
  ++FileLoads;return LoadStatus;
}
EFI_STATUS PianoProductOwnersResolveReturnedAction(PIANO_PRODUCT_OWNERS *Owners){assert(Owners==&mOwners);return HostStatus;}
EFI_STATUS PianoLateHandoffDisarm(PIANO_LATE_HANDOFF *Late){assert(Late==&mLateHandoff);Late->Phase=PianoExitUnarmed;return EFI_SUCCESS;}
BOOLEAN PianoProductDisplayRetained(VOID){return (RetainedObservers&BIT0)!=0;}
BOOLEAN PianoDisplaySmmuRetained(VOID){return (RetainedObservers&BIT1)!=0;}
BOOLEAN PianoFrameBufferMappingRetained(VOID){return (RetainedObservers&BIT2)!=0;}
BOOLEAN PianoDisplayClockRetained(VOID){return (RetainedObservers&BIT3)!=0;}
BOOLEAN PianoProductDisplayOwnerRetained(VOID){return (RetainedObservers&BIT4)!=0;}
EFI_STATUS PianoUsbControllerRestrictToDiagnostics(VOID){
  ++Restricts;assert(!mFailureDiagnostics&&!Count&&!RuntimePumps);return RestrictStatus;
}
EFI_STATUS PianoUsbControllerPumpDiagnostics(UINTN BudgetUs){
  assert(BudgetUs==1000&&Restricts==1&&mFailureDiagnostics&&!Count&&!RuntimePumps);
  ++DiagnosticPumps;
  if(ExitDuringDiagnosticPump){mBootLogExited=TRUE;System.BootServices=NULL;return EFI_SUCCESS;}
  return DiagnosticPumps==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
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
  assert(Title&&Detail);if(Failed)FailureStatus=Status;return EFI_SUCCESS;
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
  mLinuxActive=TRUE;mLinuxEsp=ESP;
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
  EspStatus=LoadStatus=EFI_SUCCESS;HostStatus=EFI_NOT_READY;FileLoads=0;
  mHandoffFailure=NULL;mHandoffStage="not-started";
  Restricts=DiagnosticPumps=DiagnosticStalls=RuntimePumps=RetainedObservers=0;
  RestrictStatus=EFI_NOT_READY;ExitDuringDiagnosticPump=FALSE;mFailureDiagnostics=FALSE;mFailureStatus=EFI_NOT_STARTED;
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
  BAD_EFI(ChildLoaded.DeviceHandle=OTHER);BAD_EFI(ChildLoaded.ParentHandle=CORE);
  BAD_EFI(ChildLoaded.ParentHandle=CHILD);BAD_EFI(ChildLoaded.Revision=0);
  BAD_EFI(ChildLoaded.ParentHandle=INNER;InnerLoaded.ParentHandle=CHILD);
  BAD_EFI(ChildLoaded.ParentHandle=INNER;InnerLoaded.DeviceHandle=OTHER);
  BAD_EFI(System.NumberOfTableEntries=0);BAD_EFI(Policy.RequestedCoreAction=PianoUsbServiceActionReboot);
#undef BAD_EFI
}
STATIC VOID UkiLoadFailure(VOID){
  CONST EFI_STATUS Errors[]={EFI_NOT_FOUND,EFI_TIMEOUT,EFI_LOAD_ERROR,EFI_SUCCESS};
  for(UINTN I=0;I<ARRAY_SIZE(Errors);++I){
    Reset();mLinuxActive=FALSE;LoadStatus=Errors[I];
    if(!setjmp(Escape)){(VOID)StartInstalledLinux(CORE);assert(!"failed UKI must not return to a countdown");}
    assert(FileLoads==1&&!mLinuxActive&&!mLinuxEsp&&mHandoffFailure);
    assert(!strcmp(mHandoffStage,"linux-uki"));
    assert(FailureStatus==(Errors[I]==EFI_SUCCESS?EFI_ABORTED:Errors[I]));
    assert(!mOwners.Report.RetiredMask&&!mLateHandoff.RetireCalls);
  }
  Reset();EspStatus=EFI_NOT_FOUND;
  if(!setjmp(Escape)){(VOID)StartInstalledLinux(CORE);assert(!"missing ESP must not retry");}
  assert(!FileLoads&&FailureStatus==EFI_NOT_FOUND);
}
STATIC VOID PrepareErrorDiagnostics(VOID){
  Reset();Services.Stall=DiagnosticStall;RestrictStatus=EFI_SUCCESS;
  mOwners.Report.Initialized=TRUE;mOwners.Report.Phase=PianoProductOwnersRunning;
  mOwners.Config.StartedOwnerMask=PIANO_OWNER_USB|PIANO_OWNER_UFS;
}
STATIC VOID ErrorDiagnostics(VOID){
  PrepareErrorDiagnostics();
  if(!setjmp(Escape)){FailStop(EFI_LOAD_ERROR);assert(!"error screen ends at the CPU halt");}
  assert(Restricts==1&&DiagnosticPumps==3&&DiagnosticStalls==2&&!mFailureDiagnostics);
  assert(mFailureStatus==EFI_LOAD_ERROR&&FailureStatus==EFI_LOAD_ERROR&&!Count&&!Handles&&!RuntimePumps);
  assert(!mOwners.Report.RetiredMask&&!mOwners.Report.Clean&&!mLateHandoff.RetireCalls);

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
  UNSAFE_DIAGNOSTICS(mOwners.Report.Busy=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Retained=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Clean=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.ServicesLost=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.PolicyStopped=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.UsbStopped=TRUE);
  UNSAFE_DIAGNOSTICS(mOwners.Report.RetiredMask=PIANO_OWNER_UFS);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Phase=PianoProductOwnersRetiring);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Phase=PianoProductOwnersClean);
  UNSAFE_DIAGNOSTICS(mOwners.Report.Phase=PianoProductOwnersRetained);
  UNSAFE_DIAGNOSTICS(mLateHandoff.Busy=TRUE);
  UNSAFE_DIAGNOSTICS(mLateHandoff.Retained=TRUE);
  UNSAFE_DIAGNOSTICS(mLateHandoff.RetireCalls=1);
  UNSAFE_DIAGNOSTICS(mLateHandoff.Phase=PianoExitRetiring);
  UNSAFE_DIAGNOSTICS(mLateHandoff.Phase=PianoExitClean);
  UNSAFE_DIAGNOSTICS(mLateHandoff.Phase=PianoExitRetained);
  for(UINTN I=0;I<5;++I){UNSAFE_DIAGNOSTICS(RetainedObservers=1U<<I);}
  UNSAFE_DIAGNOSTICS(mBootLogExited=TRUE);
  UNSAFE_DIAGNOSTICS(System.BootServices=NULL);
  UNSAFE_DIAGNOSTICS(gST=NULL);
  UNSAFE_DIAGNOSTICS(gBS=NULL);
  UNSAFE_DIAGNOSTICS(Services.Stall=NULL);
#undef UNSAFE_DIAGNOSTICS
}
int main(int Argc,char **Argv){
  assert(Argc==2);
  if(!strcmp(Argv[1],"efi-admission"))EfiAdmission();
  else if(!strcmp(Argv[1],"uki-load-failure"))UkiLoadFailure();
  else if(!strcmp(Argv[1],"error-diagnostics"))ErrorDiagnostics();
  else assert(!"unknown test case");
  puts("Actual resident Core UKI admission and failure diagnostics passed");return 0;
}
