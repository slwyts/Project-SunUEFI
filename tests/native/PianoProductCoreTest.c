// SPDX-License-Identifier: BSD-2-Clause-Patent
// Include the actual Core; GC removes startup hardware paths from this host test.
#define MDEPKG_NDEBUG
#include "../../uefi/core/PianoProductCore.c"
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
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL Runtime;
STATIC PIANO_BOOT_POLICY_REPORT Policy;
STATIC PIANO_RAW_LINUX_REPORT Raw;
STATIC jmp_buf Escape;
STATIC UINTN CoreId,ManagerId,ChildId,EspId,OtherId,FileId,TokenId;
#define CORE ((EFI_HANDLE)&CoreId)
#define MANAGER ((EFI_HANDLE)&ManagerId)
#define CHILD ((EFI_HANDLE)&ChildId)
#define ESP ((EFI_HANDLE)&EspId)
#define OTHER ((EFI_HANDLE)&OtherId)
enum { RESOLVE=1,LOAD,REQUEST,DISCARD,RETIRE,PREPARE,ARM,ENTER };
STATIC UINT32 Calls[16],Count,Handles,Resolves,FailAt,UsbArrival;
STATIC EFI_STATUS LoadStatus,HandleStatus,StageStatus,FailureStatus;
STATIC BOOLEAN NullLoaded,RetainedFile,DirtyRetirement,EnterReturns;

STATIC VOID Record(UINT32 Call){assert(Count<ARRAY_SIZE(Calls));Calls[Count++]=Call;}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Interface){
  assert(Image==CHILD&&Guid==&gEfiLoadedImageProtocolGuid);++Handles;
  *Interface=NullLoaded?NULL:&ChildLoaded;return HandleStatus;
}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &Policy;}
EFI_STATUS PianoProductOwnersResolveReturnedAction(PIANO_PRODUCT_OWNERS *Owners){
  assert(Owners==&mOwners);Record(RESOLVE);
  ++Resolves;
  if((UsbArrival==1&&Resolves==1)||(UsbArrival==2&&Resolves==2)){
    Owners->Report.Origin=PianoProductRequestUsb;return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
EFI_STATUS PianoRawLinuxLoadStable(PIANO_PRODUCT_RUNTIME_PROTOCOL *R,VOID **Context,VOID **Token){
  assert(R==&Runtime&&mSystemdEntering);Record(LOAD);
  if(LoadStatus==EFI_SUCCESS){*Context=&FileId;*Token=&TokenId;}
  return LoadStatus;
}
BOOLEAN PianoRawLinuxStableRetained(VOID){return RetainedFile;}
EFI_STATUS PianoProductOwnersRequestFileBoot(PIANO_PRODUCT_OWNERS *Owners,VOID *Context,VOID *Token){
  assert(Owners==&mOwners&&Context==&FileId&&Token==&TokenId);Record(REQUEST);
  Owners->Report.Origin=PianoProductRequestFile;return EFI_SUCCESS;
}
EFI_STATUS PianoRawLinuxDiscardStable(VOID){Record(DISCARD);return FailAt==DISCARD?StageStatus:EFI_SUCCESS;}
EFI_STATUS PianoProductOwnersRetire(PIANO_PRODUCT_OWNERS *Owners){
  assert(Owners==&mOwners&&mSystemdEntering);Record(RETIRE);
  if(UsbArrival==3){Owners->Report.Origin=PianoProductRequestUsb;return EFI_NOT_READY;}
  Owners->Report.Clean=!DirtyRetirement;
  Owners->Report.AllowedAction=PianoUsbServiceActionBoot;
  return FailAt==RETIRE?StageStatus:EFI_SUCCESS;
}
EFI_STATUS PianoRawLinuxPrepare(PIANO_PRODUCT_OWNERS *Owners,EFI_HANDLE Image,EFI_HANDLE ExitImage,CONST PIANO_RAW_LINUX_REPORT **Out){
  assert(Owners==&mOwners&&Owners->Report.Clean&&Image==CORE&&ExitImage==CHILD);
  Record(PREPARE);*Out=&Raw;
  return FailAt==PREPARE?StageStatus:EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffArmRaw(PIANO_LATE_HANDOFF *Late,EFI_HANDLE Image,CONST PIANO_RAW_LINUX_REPORT *R){
  assert(Late==&mLateHandoff&&Image==CHILD&&R==&Raw);Record(ARM);
  return FailAt==ARM?StageStatus:EFI_SUCCESS;
}
EFI_STATUS PianoRawLinuxEnter(CONST PIANO_RAW_LINUX_REPORT *R){
  assert(R==&Raw);Record(ENTER);
  if(FailAt==ENTER)return StageStatus;
  if(EnterReturns)return EFI_SUCCESS;
  longjmp(Escape,2); // The real successful raw handoff never returns.
}
EFI_STATUS PianoLateHandoffArmReset(PIANO_LATE_HANDOFF *Late,EFI_HANDLE Image){
  (VOID)Late;(VOID)Image;assert(!"Linux entry must not arm a reset");return EFI_ABORTED;
}
EFI_STATUS PianoResetExecute(CONST PIANO_PRODUCT_OWNERS *Owners,EFI_HANDLE Image){
  (VOID)Owners;(VOID)Image;assert(!"Linux entry must not reset");return EFI_ABORTED;
}
BOOLEAN PianoProductDisplayRetained(VOID){return FALSE;}
BOOLEAN PianoDisplaySmmuRetained(VOID){return FALSE;}
BOOLEAN PianoFrameBufferMappingRetained(VOID){return FALSE;}
BOOLEAN PianoDisplayClockRetained(VOID){return FALSE;}
BOOLEAN PianoProductDisplayOwnerRetained(VOID){return FALSE;}
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
  memset(&mSystemdApplication,0,sizeof(mSystemdApplication));memset(&Policy,0,sizeof(Policy));
  memset(Calls,0,sizeof(Calls));memset(&ChildLoaded,0,sizeof(ChildLoaded));
  Services.HandleProtocol=Handle;gBS=&Services;gST=&System;System.BootServices=gBS;
  mBootLogEnabled=mBootLogExited=FALSE;mBootLogStage="MENU";
  mSystemdActive=TRUE;mSystemdEntering=FALSE;mSystemdEsp=ESP;
  mSystemdApplication.Image=MANAGER;mSystemdApplication.StartCalled=TRUE;
  ChildLoaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,
    .ParentHandle=MANAGER,.DeviceHandle=ESP,.SystemTable=&System,.ImageBase=&ChildId,
    .ImageSize=4096,.ImageCodeType=EfiLoaderCode,.ImageDataType=EfiLoaderData};
  Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
  Count=Handles=Resolves=FailAt=UsbArrival=0;LoadStatus=HandleStatus=StageStatus=EFI_SUCCESS;
  FailureStatus=EFI_NOT_STARTED;NullLoaded=RetainedFile=DirtyRetirement=EnterReturns=FALSE;
}
STATIC EFI_STATUS Start(VOID){return StartInstalledLinux(CORE,&Runtime,CHILD);}
STATIC VOID Sequence(CONST UINT32 *Expected,UINTN N){
  assert(Count==N&&!memcmp(Calls,Expected,N*sizeof(*Expected)));
}
STATIC VOID Admission(VOID){
  Reset();mSystemdActive=FALSE;assert(Start()==EFI_NOT_STARTED&&!Count&&!Handles);
#define DENIED(Change) do{Reset();Change;assert(Start()==EFI_ACCESS_DENIED&&!Count&&!Handles);}while(0)
  DENIED(mSystemdEntering=TRUE);DENIED(mSystemdApplication.StartCalled=FALSE);
  DENIED(mSystemdApplication.StartReturned=TRUE);DENIED(mSystemdApplication.Retained=TRUE);
  DENIED(mSystemdApplication.Image=NULL);DENIED(mSystemdEsp=NULL);
#undef DENIED
  Reset();assert(StartInstalledLinux(CORE,&Runtime,NULL)==EFI_ACCESS_DENIED&&!Handles);
  Reset();HandleStatus=EFI_NOT_FOUND;assert(Start()==EFI_NOT_FOUND&&Handles==1&&!Count);
#define BAD_ID(Change) do{Reset();Change;assert(Start()==EFI_ACCESS_DENIED&&Handles==1&&!Count);}while(0)
  BAD_ID(NullLoaded=TRUE);BAD_ID(ChildLoaded.Revision=0);
  BAD_ID(ChildLoaded.ParentHandle=CORE);BAD_ID(ChildLoaded.DeviceHandle=OTHER);
  BAD_ID(ChildLoaded.SystemTable=NULL);BAD_ID(ChildLoaded.ImageBase=NULL);
  BAD_ID(ChildLoaded.ImageSize=0);BAD_ID(ChildLoaded.ImageCodeType=EfiBootServicesCode);
  BAD_ID(ChildLoaded.ImageDataType=EfiBootServicesData);
#undef BAD_ID
}
STATIC VOID BeforeRetirement(VOID){
  Reset();LoadStatus=EFI_NOT_FOUND;
  assert(Start()==EFI_NOT_FOUND&&!mSystemdEntering&&mSystemdActive&&Handles==1);
  CONST UINT32 Expected[]={RESOLVE,LOAD,RESOLVE};Sequence(Expected,ARRAY_SIZE(Expected));
  assert(FailureStatus==EFI_NOT_STARTED);
}
STATIC VOID FatalFailures(VOID){
  Reset();LoadStatus=EFI_DEVICE_ERROR;RetainedFile=TRUE;
  if(!setjmp(Escape)){(VOID)Start();assert(!"retained load must not return");}
  assert(Count==2&&Calls[1]==LOAD&&FailureStatus==EFI_DEVICE_ERROR);
  CONST UINT32 Stages[]={RETIRE,PREPARE,ARM,ENTER};
  for(UINTN I=0;I<ARRAY_SIZE(Stages);++I){
    Reset();FailAt=Stages[I];StageStatus=EFI_DEVICE_ERROR;
    int Result=setjmp(Escape);
    if(!Result){(VOID)Start();assert(!"retirement/handoff failure must not return");}
    assert(Result==1&&Calls[Count-1]==Stages[I]&&mSystemdEntering&&FailureStatus==StageStatus);
  }
  Reset();DirtyRetirement=TRUE;
  if(!setjmp(Escape)){(VOID)Start();assert(!"unclean retirement must halt");}
  assert(Calls[Count-1]==RETIRE&&!mOwners.Report.Clean);
  Reset();EnterReturns=TRUE;
  if(!setjmp(Escape)){(VOID)Start();assert(!"unexpected successful return from raw enter must halt");}
  assert(Calls[Count-1]==ENTER&&FailureStatus==EFI_ABORTED);
}
STATIC VOID HandoffOrder(VOID){
  Reset();
  int Result=setjmp(Escape);
  if(!Result){(VOID)Start();assert(!"successful raw handoff must not return");}
  assert(Result==2&&mSystemdEntering&&FailureStatus==EFI_NOT_STARTED);
  CONST UINT32 Expected[]={RESOLVE,LOAD,RESOLVE,REQUEST,RETIRE,PREPARE,ARM,ENTER};
  Sequence(Expected,ARRAY_SIZE(Expected));
}
STATIC VOID HostPriority(VOID){
  Reset();UsbArrival=1;
  assert(Start()==EFI_ABORTED&&!mSystemdEntering&&FailureStatus==EFI_NOT_STARTED);
  CONST UINT32 Before[]={RESOLVE};Sequence(Before,ARRAY_SIZE(Before));
  Reset();UsbArrival=2;
  assert(Start()==EFI_ABORTED&&!mSystemdEntering&&FailureStatus==EFI_NOT_STARTED);
  CONST UINT32 During[]={RESOLVE,LOAD,RESOLVE,DISCARD};Sequence(During,ARRAY_SIZE(During));
  Reset();UsbArrival=2;FailAt=DISCARD;StageStatus=EFI_DEVICE_ERROR;
  if(!setjmp(Escape)){(VOID)Start();assert(!"failed file release must not return");}
  Sequence(During,ARRAY_SIZE(During));assert(FailureStatus==EFI_DEVICE_ERROR);
  Reset();UsbArrival=3;
  if(!setjmp(Escape)){(VOID)Start();assert(!"retirement-path USB race must not execute the host action as a child");}
  CONST UINT32 AtRetire[]={RESOLVE,LOAD,RESOLVE,REQUEST,RETIRE,DISCARD};
  Sequence(AtRetire,ARRAY_SIZE(AtRetire));assert(FailureStatus==EFI_ABORTED);
}
int main(int Argc,char **Argv){
  assert(Argc==2);
  if(!strcmp(Argv[1],"admission"))Admission();
  else if(!strcmp(Argv[1],"before-retirement"))BeforeRetirement();
  else if(!strcmp(Argv[1],"fatal-failures"))FatalFailures();
  else if(!strcmp(Argv[1],"handoff-order"))HandoffOrder();
  else if(!strcmp(Argv[1],"host-priority"))HostPriority();
  else assert(!"unknown test case");
  puts("Actual resident Core synchronous Linux entry passed");return 0;
}
