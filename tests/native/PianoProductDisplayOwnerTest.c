// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual coordinator+Owners+Guard. Lease/Reader are explicit service boundaries,
// not evidence of native identity/MMIO readiness (their own source tests do that).
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include "PianoProductDisplayOwner.h"
#include "PianoDisplayClockRead.h"
#include "PianoDisplayRailObserve.h"
#include "PianoDisplayNonGdscClock.h"
#include "PianoNativeImages.h"
#include "PianoFastbootBlockRead.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Protocol/Cpu.h>
#include <Guid/EventGroup.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
static EFI_BOOT_SERVICES Bs;static EFI_DXE_SERVICES Ds;static EFI_CPU_ARCH_PROTOCOL Cpu;
static EFI_CPU_INTERRUPT_HANDLER Handlers[4];static EFI_EVENT_NOTIFY GuardExit;static VOID *GuardContext;
static BOOLEAN ServicesLive=TRUE;static EFI_TPL Tpl=TPL_APPLICATION;
static UINT32 Case,Loads,ReaderStarts,AcquireCalls,ReleaseCalls,ReaderCloses,HandlerCloses,Logs,EvidenceCalls;
static UINT32 RailInits,RailObservations,RailCloses;
static UINT32 ChildStarts,ChildStops;
static UINT32 Counter=100;static UINT32 Ahb=0x88000002;
static PIANO_DISPLAY_CLOCK_READ *Reader;static PIANO_DISPLAY_CLOCK_LEASE *Lease;
static PIANO_PRODUCT_OWNERS Owners;static PIANO_BOOT_POLICY_REPORT Policy;
static PIANO_DWC3_SERVICE_STATUS Usb;static PIANO_UFS_RESET_REPORT Ufs;
static PIANO_PRODUCT_RUNTIME_PROTOCOL Runtime;static PIANO_FB_STORAGE Storage;static BOOLEAN Bridge=TRUE;
static CHAR8 Order[100];static UINT32 Steps;
static VOID Step(CHAR8 C){assert(Steps<99);Order[Steps++]=C;Order[Steps]=0;}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT64 EFIAPI DivU64x32Remainder(UINT64 D,UINT32 V,UINT32 *R){if(R)*R=(UINT32)(D%V);return D/V;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINT16 EFIAPI ReadUnaligned16(CONST VOID *P){UINT16 V;memcpy(&V,P,2);return V;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,4);return V;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"Print assert%s:%lu %s\n",F,(unsigned long)L,D);abort();}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){assert(ServicesLive&&L);CHAR8 B[256];VA_LIST A;VA_START(A,Fmt);UINTN N=AsciiVSPrint(B,sizeof(B),Fmt,A);VA_END(A);assert(N&&N<180&&B[N-1]=='\n');Logs++;}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"unexpected fatal fixture fault");abort();}
static BOOLEAN EFIAPI Alive(VOID){return ServicesLive;}
static VOID Lost(VOID){Lease->BorrowToken=17;PianoProductDisplayFenceExit();assert(!Lease->BorrowToken);ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
static EFI_TPL EFIAPI Raise(EFI_TPL N){assert(ServicesLive);EFI_TPL Old=Tpl;Tpl=N;return Old;}
static VOID EFIAPI Restore(EFI_TPL N){assert(ServicesLive);Tpl=N;}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){assert(ServicesLive&&P==&Cpu&&(T==0||T==3));if(H){assert(!Handlers[T]);Handlers[T]=H;}else{assert(Handlers[T]);Handlers[T]=NULL;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){assert(ServicesLive&&!R);*P=G==&gEfiCpuArchProtocolGuid?(VOID *)&Cpu:&Runtime;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL Tp,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){assert(ServicesLive&&T==EVT_NOTIFY_SIGNAL&&Tp==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);if(C==&Owners){*E=(VOID *)0x987;}else{GuardExit=N;GuardContext=(VOID *)C;*E=(VOID *)77;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(ServicesLive);if(E==(VOID *)0x987){Step('M');assert(Lease->Report.Released&&ReaderCloses==1);}else{assert(E==(VOID *)77);HandlerCloses++;if(Case==16)return EFI_WARN_STALE_DATA;}return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *A,UINT64 *B){assert(ServicesLive);*A=0;*B=MAX_UINT64;return 1000000;}
UINT64 EFIAPI GetPerformanceCounter(VOID){assert(ServicesLive);return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){UINT64 State[]={4,1,0x480803514ULL,0xd7200000,0,0xff4400};memcpy(P,State,sizeof(State));return EFI_SUCCESS;}
UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *P){assert(A==0x127000);*P=A;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){assert(ServicesLive&&A==0x127000);*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,.Attributes=EFI_MEMORY_UC};return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN A){assert(ServicesLive&&(A==0x127004||A==0x127008));Loads++;if(Case==15&&Loads==1){EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=0x1000,.FAR=A,.ESR=0x96000010,.SPSR=5};EFI_SYSTEM_CONTEXT Context={.SystemContextAArch64=&C};Handlers[0](0,Context);assert(C.ELR==0x1004);}return A==0x127004?Ahb:0x08200001;}
EFI_STATUS PianoDisplayClockReadInitialize(PIANO_DISPLAY_CLOCK_READ *S,CONST PIANO_DISPLAY_CLOCK_READ_ENV *E){assert(ServicesLive&&E->Services==gBS&&E->DxeServices==gDS&&E->Lease);ReaderStarts++;Reader=S;Lease=E->Lease;if(Case==1)return EFI_UNSUPPORTED;S->Signature=1;S->Env=*E;S->Report.Revision=1;S->PinnedCopy=(VOID *)0x500;S->PinnedBytes=256;return EFI_SUCCESS;}
EFI_STATUS PianoDisplayClockReadCpu(VOID *Context,UINT64 A,UINTN N,VOID *D){assert(Context==Reader&&A&&N&&D&&ServicesLive);ZeroMem(D,N);return EFI_SUCCESS;}
EFI_STATUS PianoNativeGetLoadedImage(CONST EFI_GUID *Guid,EFI_HANDLE *Out){
  assert(Guid&&Out&&ServicesLive);*Out=NULL;if(Case<18)return EFI_NOT_FOUND;
  assert(Guid->Data1==0xcb29f4d1||Guid->Data1==0x8bd3b475);*Out=(VOID *)(UINTN)Guid->Data1;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayRailInit(PIANO_DISPLAY_RAIL_OBSERVER *S,CONST PIANO_DISPLAY_RAIL_ENV *E){
  assert(ServicesLive&&E->ClockReader==Reader&&E->NpaHandle&&E->VcsHandle);RailInits++;
  S->Signature=1;S->Env=*E;S->Report.Initialized=Case!=20&&Case!=21;return Case==20||Case==21?EFI_NOT_READY:EFI_SUCCESS;
}
EFI_STATUS PianoDisplayRailObserve(PIANO_DISPLAY_RAIL_OBSERVER *S,CONST CHAR8 *Phase){
  assert(S->Report.Initialized&&ServicesLive&&Phase);RailObservations++;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayRailReemit(PIANO_DISPLAY_RAIL_OBSERVER *S){assert(S->Signature&&ServicesLive);return EFI_SUCCESS;}
BOOLEAN PianoDisplayRailRetained(CONST PIANO_DISPLAY_RAIL_OBSERVER *S){return S->Report.Retained||S->Report.ServicesLost;}
EFI_STATUS PianoDisplayRailClose(PIANO_DISPLAY_RAIL_OBSERVER *S){
  assert(ServicesLive&&S->Signature&&!ReleaseCalls&&!ReaderCloses);RailCloses++;
  if(Case!=20&&Case!=21)assert(ChildStops==1);
  if(Case==19||Case==21)return EFI_WARN_STALE_DATA;
  S->Report.Initialized=FALSE;S->Report.Close=EFI_SUCCESS;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayNonGdscClockAcquire(PIANO_NON_GDSC_CLOCK *S,CONST PIANO_NON_GDSC_CLOCK_ENV *E){
  assert(E->Gcc==Lease&&E->Reader==Reader&&E->Rail->Report.Initialized&&RailObservations==1);ChildStarts++;
  S->Signature=1;S->Env=*E;S->Report.Held=TRUE;S->Report.OwnedReferences=1;S->Exit=(VOID *)0xdef;
  if(Case==26){S->Report.Held=FALSE;S->Report.OwnedReferences=0;S->Report.Released=TRUE;S->Exit=NULL;ChildStops++;return EFI_NOT_READY;}
  if(Case==23){S->Report.Retained=TRUE;return EFI_DEVICE_ERROR;}return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayNonGdscClockRelease(PIANO_NON_GDSC_CLOCK *S){
  assert(ServicesLive&&S->Signature&&!RailCloses&&!ReleaseCalls&&!ReaderCloses);ChildStops++;
  if(Case==22)return EFI_WARN_STALE_DATA;
  if(Case==24)return EFI_SUCCESS; // incomplete report cannot authorize parent cleanup
  if(Case==25){S->Report.ServicesLost=S->Report.Retained=TRUE;return EFI_ABORTED;}
  S->Report.Held=FALSE;S->Report.OwnedReferences=0;S->Report.Released=TRUE;S->Exit=NULL;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockReadSnapshotClock(PIANO_DISPLAY_CLOCK_READ *S,PIANO_DISPLAY_CLOCK_SELECTOR Selector,PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *Out){
  assert(S==Reader&&ServicesLive&&Selector==PianoClockSelectNonGdscAhb&&Out&&Lease->Report.Held);
  // Real selector paths are tested separately. An unavailable clean diagnostic
  // must not undo the established GCC owner or pretend a power vote exists.
  return EFI_NOT_READY;
}
EFI_STATUS PianoDisplayClockReadFailureEvidence(VOID *Context,UINT64 A,UINTN N,EFI_STATUS Status,PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE *Out){
  assert(Context==Reader&&A&&N&&Status==EFI_NOT_READY&&Out&&ServicesLive);EvidenceCalls++;
  // The controlled reader supplies no proof; actual Reader+Lease tests own the
  // fresh refusal proof. Unsupported must leave the caller's output untouched.
  return EFI_UNSUPPORTED;
}
VOID PianoDisplayClockReadFenceExit(PIANO_DISPLAY_CLOCK_READ *S){if(S)S->Report.Retained=S->Report.ServicesLost=TRUE;}
EFI_STATUS PianoDisplayClockReadClose(PIANO_DISPLAY_CLOCK_READ *S){assert(ServicesLive&&S==Reader);ReaderCloses++;if(Case==5||Case==8){S->Report.Retained=TRUE;return EFI_DEVICE_ERROR;}if(Case==9){Lost();return EFI_ABORTED;}S->PinnedCopy=NULL;S->PinnedBytes=0;return EFI_SUCCESS;}
EFI_STATUS PianoDisplayClockLeaseAcquire(PIANO_DISPLAY_CLOCK_LEASE *S,CONST PIANO_DISPLAY_CLOCK_LEASE_ENV *E){
  assert(ServicesLive&&S==Lease&&E->Context==Reader&&E->ReadGcc&&E->ReadCpu&&E->GetReadFailureEvidence==PianoDisplayClockReadFailureEvidence);AcquireCalls++;S->Signature=1;S->Env=*E;S->Report.Revision=1;
  if(Case==2||Case==3||Case==5){if(Case==3)S->PinnedCopy=(VOID *)0x501;return S->Report.Status=EFI_NOT_READY;}
  EFI_STATUS Q=E->ReadGcc(E->Context,&S->Report.BeforeGcc);if(Q!=EFI_SUCCESS){S->Report.Retained=S->Report.BeforeGcc.Retained;return S->Report.Status=Q;}
  S->Report.AcquireAttempted=S->Report.Held=TRUE;S->Report.OwnedReferences=1;S->Exit=(VOID *)0xaa;S->Report.ClockId=0x10001;S->Report.NativeBase=0xcf100000;
  if(Case==4){S->Report.Retained=TRUE;return S->Report.Status=EFI_DEVICE_ERROR;}
  Ahb=0x88000003;S->Report.Baseline=(PIANO_DISPLAY_CLOCK_LEASE_REFS){.MatchingSnapshots=2,.Total={5,2},.PerClient={2,1}};
  S->Report.Acquired=(PIANO_DISPLAY_CLOCK_LEASE_REFS){.MatchingSnapshots=2,.Total={6,2},.PerClient={3,1}};
  S->Report.CounterStatus=EFI_SUCCESS;
  Q=E->ReadGcc(E->Context,&S->Report.AfterGcc);S->Report.Status=Q;return Q;
}
EFI_STATUS PianoDisplayClockLeaseRelease(PIANO_DISPLAY_CLOCK_LEASE *S){
  assert(S==Lease);if(!S->Report.Held||S->Report.Retained||S->Report.Released)return EFI_ACCESS_DENIED;assert(ServicesLive);ReleaseCalls++;if(Steps)Step('D');S->Report.ReleaseAttempted=TRUE;
  if(Case==6){S->Report.Retained=TRUE;return S->Report.Status=EFI_DEVICE_ERROR;}if(Case==7){Lost();return S->Report.Status=EFI_ABORTED;}
  S->Report.ReleaseBefore=(PIANO_DISPLAY_CLOCK_LEASE_REFS){.MatchingSnapshots=2,.Total={10,4},.PerClient={7,3}};
  S->Report.Retired=(PIANO_DISPLAY_CLOCK_LEASE_REFS){.MatchingSnapshots=2,.Total={9,4},.PerClient={6,3}};
  EFI_STATUS Q=S->Env.ReadGcc(S->Env.Context,&S->Report.ReleaseGcc);S->Report.ReleaseReadbackStatus=Q;if(Q!=EFI_SUCCESS){S->Report.Retained=TRUE;return S->Report.Status=Q;}
  S->Report.Held=FALSE;S->Report.Released=TRUE;S->Report.OwnedReferences=0;S->Exit=NULL;return S->Report.Status=EFI_SUCCESS;
}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &Policy;}
EFI_STATUS PianoBootPolicyStop(VOID){assert(Tpl==TPL_APPLICATION);Step('P');Policy.ProtocolInstalled=FALSE;return EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *S){*S=Usb;return EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerValidateStartupFailureProof(CONST VOID *F,CONST PIANO_SMMU_RETIRED_USB_PROOF *P){(VOID)F;(VOID)P;assert(!"running-display fixture must not authorize USB absence");return EFI_ACCESS_DENIED;}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Why,PIANO_USB_SERVICE_RETIRE_REPORT *R){assert(Why==EFI_SUCCESS);Step('U');*R=(PIANO_USB_SERVICE_RETIRE_REPORT){.Revision=1,.Attempted=TRUE,.Clean=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DomainFreed=TRUE,.ClocksReleased=TRUE,.DmaBuffersFreed=9,.ClockReleaseMask=255};return EFI_SUCCESS;}
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *A){(VOID)A;assert(FALSE);return EFI_UNSUPPORTED;}
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *F,PIANO_SMMU_RETIRED_USB_PROOF *P){assert(F==(VOID *)1);Step('R');P->Valid=TRUE;return EFI_SUCCESS;}
static EFI_STATUS Ready(VOID *C){(VOID)C;return EFI_SUCCESS;}
static EFI_STATUS InfoStorage(VOID *C,CONST CHAR8 *N,PIANO_FB_PARTITION_INFO *R){(VOID)C;(VOID)N;(VOID)R;assert(FALSE);return EFI_UNSUPPORTED;}
static EFI_STATUS ReadStorage(VOID *C,CONST PIANO_FB_PARTITION_INFO *I,UINT64 A,UINTN N,VOID *P){(VOID)C;(VOID)I;(VOID)A;(VOID)N;(VOID)P;assert(FALSE);return EFI_UNSUPPORTED;}
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID){return Bridge?&Storage:NULL;}
VOID PianoFastbootBlockReadStop(VOID){Step('X');Bridge=FALSE;}
EFI_STATUS PianoUfsAcceptRetiredUsb(CONST PIANO_SMMU_RETIRED_USB_PROOF *P){assert(P->Valid);Step('A');return EFI_SUCCESS;}
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID){assert(Tpl==TPL_CALLBACK);Step('p');return EFI_SUCCESS;}
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID){assert(Tpl==TPL_CALLBACK);Step('s');return EFI_SUCCESS;}
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID){return &Ufs;}
static EFI_STATUS InputStop(VOID *C,PIANO_PRODUCT_INPUT_RETIRE_REPORT *R){(VOID)C;assert(Tpl==TPL_APPLICATION);Step('I');*R=(PIANO_PRODUCT_INPUT_RETIRE_REPORT){.Revision=1,.Started=TRUE,.Returned=TRUE,.Clean=TRUE};return EFI_SUCCESS;}
static EFI_STATUS EFIAPI RuntimeRequest(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 A){assert(P==&Runtime&&A==PIANO_PRODUCT_ACTION_RETURN_CORE);return EFI_SUCCESS;}
static VOID Setup(UINT32 N){Case=N;gBS=&Bs;gDS=&Ds;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
  Policy.Initialized=Policy.ProtocolInstalled=TRUE;Runtime.Revision=1;Runtime.RequestAction=RuntimeRequest;Usb=(PIANO_DWC3_SERVICE_STATUS){.Revision=1,.Started=TRUE,.Phase=PianoUsbServiceListening};Storage=(PIANO_FB_STORAGE){NULL,Ready,InfoStorage,ReadStorage};
  Ufs=(PIANO_UFS_RESET_REPORT){.Started=TRUE,.Prepared=TRUE,.Returned=TRUE,.Clean=TRUE,.TplHeld=TRUE,.DmaFreed=3,.Disconnected=6,.ProtocolsRemoved=7};
}
static VOID Run(UINT32 N){Setup(N);PIANO_PRODUCT_DISPLAY_STARTUP_REPORT Start={0};PIANO_PRODUCT_DISPLAY_RETIRE_REPORT Stop={0};
  if(N==14){assert(PianoProductDisplayStartup(&Start)==EFI_NOT_STARTED&&!Start.Revision);return;}
  EFI_STATUS S=PianoProductDisplayStart(Alive);PianoProductDisplayStartup(&Start);
  if(N==21){assert(S!=EFI_SUCCESS&&Start.Retained&&Start.Held&&RailCloses==1&&!ReleaseCalls);return;}
  if(N==23){assert(S==EFI_DEVICE_ERROR&&Start.Retained&&Start.Held&&ChildStarts==1&&!ChildStops&&!RailCloses&&!ReleaseCalls);return;}
  if(N==0||N==6||N==7||N==8||N==9||N==10||N==11||N==12||N==13||N==17){assert(S==EFI_SUCCESS&&Start.Held&&Start.OwnedReferences==1&&!Start.KnownNoSideEffects&&Start.AcquireBeforeTotal[0]==5&&Start.AcquireAfterTotal[0]==6&&Loads==8&&HandlerCloses==2);}
  if(N==1||N==2||N==5||N==15){assert(S!=EFI_SUCCESS&&!Start.Held&&!Start.AcquireAttempted);if(N==5)assert(Start.Retained&&!Start.KnownNoSideEffects);else assert(Start.KnownNoSideEffects&&!Start.Retained);return;}
  if(N==3||N==4||N==16){assert(S!=EFI_SUCCESS&&Start.Retained&&!Start.KnownNoSideEffects&&!ReaderCloses);return;}
  if(N==10){assert(PianoProductDisplayStop((VOID *)3,&Stop)==EFI_INVALID_PARAMETER&&!ReleaseCalls);return;}
  if(N==17){PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE Evidence,Before;memset(&Evidence,0xa5,sizeof(Evidence));Before=Evidence;
    assert(Lease->Env.GetReadFailureEvidence(Lease->Env.Context,Lease->Report.NativeBase+0x1000,256,EFI_NOT_READY,&Evidence)==EFI_UNSUPPORTED);
    assert(EvidenceCalls==1&&!memcmp(&Evidence,&Before,sizeof(Evidence))&&!Lease->Report.CleanSourceRefusal&&!ReleaseCalls&&Loads==8);
    Reader->Report.EfiMap=(PIANO_DISPLAY_CLOCK_EFI_MAP_DIAGNOSTIC){.Status=EFI_NOT_READY,.GetMapStatus=EFI_SUCCESS,.Reason=PianoClockEfiMapCache,
      .DescriptorIndex=MAX_UINT32,.DescriptorType=EfiBootServicesCode,.DescriptorBase=0xcf100000,.DescriptorPages=0x44,
      .DescriptorAttributes=EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT,.ConflictIndex=MAX_UINT32,.Cursor=0xcf101000,
      .MapBytes=sizeof(Reader->Map),.DescriptorBytes=256,.DescriptorVersion=EFI_MEMORY_DESCRIPTOR_VERSION};
    UINT32 BeforeLogs=Logs;assert(PianoProductDisplayReplay()==EFI_SUCCESS&&Logs>BeforeLogs&&Loads==8&&EvidenceCalls==1);return;}
  if(N==12||N==13){PIANO_DISPLAY_CLOCK_LEASE Before=*Lease;if(N==12){assert(PianoProductDisplayStartup((VOID *)Start.LeaseContext)==EFI_INVALID_PARAMETER);assert(PianoProductDisplayStop(Start.LeaseContext,(VOID *)Start.LeaseContext)==EFI_INVALID_PARAMETER);}else assert(PianoProductDisplayStartup((VOID *)(MAX_UINTN-16))==EFI_INVALID_PARAMETER);
    assert(!memcmp(&Before,Lease,sizeof(Before))&&!ReleaseCalls);return;}
  if(N==0){PIANO_PRODUCT_OWNERS_CONFIG C={.Revision=1,.Fdt=(VOID *)1,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,.StartedOwnerMask=PIANO_OWNER_SUPPORTED_MASK,.AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO,.Runtime=&Runtime,.StopInput=InputStop,.DisplayContext=Start.LeaseContext,.StopDisplay=PianoProductDisplayStop,.DisplayStartup=Start};
    assert(PianoProductOwnersInitialize(&Owners,&C)==EFI_SUCCESS);Usb.Phase=PianoUsbServiceStopRequested;Usb.Action=PianoUsbServiceActionReboot;assert(PianoProductOwnersObserveUsbAction(&Owners)==EFI_SUCCESS);
    EFI_STATUS Retire=PianoProductOwnersRetire(&Owners);
    if(Retire!=EFI_SUCCESS)fprintf(stderr,"joint retire=%lx order=%s display=%lx release=%lx count=%lx gcc=%lx/%lx reads=%u pages=%u cleanup=%lx started=%u returned=%u clean=%u held=%u released=%u owned=%u/%u snapshots=%u/%u\n",(unsigned long)Retire,Order,(unsigned long)Owners.Report.Display.Status,(unsigned long)Owners.Report.Display.Release,(unsigned long)Owners.Report.Display.CounterStatus,(unsigned long)Owners.Report.Display.GccReadback,(unsigned long)Owners.Report.Display.GccReadbackEnd,Owners.Report.Display.GccReads,Owners.Report.Display.GccPages,(unsigned long)Owners.Report.Display.Cleanup,Owners.Report.Display.Started,Owners.Report.Display.Returned,Owners.Report.Display.Clean,Owners.Report.Display.HeldAfter,Owners.Report.Display.Released,Owners.Report.Display.OwnedReferencesBefore,Owners.Report.Display.OwnedReferencesAfter,Owners.Report.Display.ReleaseBeforeSnapshots,Owners.Report.Display.ReleaseAfterSnapshots);
    assert(Retire==EFI_SUCCESS&&Owners.Report.Clean&&Owners.Report.DisplayStopped&&Owners.Report.Display.ReleaseBeforeTotal[0]==10&&Owners.Report.Display.ReleaseAfterTotal[0]==9&&!strcmp(Order,"PURXApsIDM")&&Loads==12&&ReleaseCalls==1&&ReaderCloses==1);return;}
  S=PianoProductDisplayStop(Start.LeaseContext,&Stop);
  if(N>=18&&N<=20){
    assert(RailInits==1&&RailCloses==1&&RailObservations==(N<20?1U:0U)&&ChildStarts==(N<20?1U:0U)&&ChildStops==(N<20?1U:0U));
    if(N==19)assert(S==EFI_DEVICE_ERROR&&Stop.Retained&&Stop.HeldAfter&&!ReleaseCalls&&!ReaderCloses);
    else assert(S==EFI_SUCCESS&&Stop.Clean&&ReleaseCalls==1&&ReaderCloses==1);
  }
  if(N==22||N==24||N==25)assert(EFI_ERROR(S)&&Stop.Retained&&!Stop.Clean&&ChildStops==1&&!RailCloses&&!ReleaseCalls&&!ReaderCloses);
  if(N==25)assert(S==EFI_ABORTED&&Stop.ServicesLost);
  if(N==26)assert(S==EFI_SUCCESS&&Stop.Clean&&ChildStarts==1&&ChildStops==1&&RailCloses==1&&ReleaseCalls==1&&ReaderCloses==1);
  if(N==11){assert(S==EFI_SUCCESS&&Stop.Clean&&Stop.ExitClosed&&Stop.OwnedReferencesBefore==1&&!Stop.OwnedReferencesAfter&&Stop.GccReads==4&&Stop.GccPages==1&&Stop.ReleaseBeforeSnapshots==2&&Stop.ReleaseAfterSnapshots==2);assert(PianoProductDisplayStop(Start.LeaseContext,&Stop)==EFI_ACCESS_DENIED&&ReleaseCalls==1);}
  if(N==6)assert(S==EFI_DEVICE_ERROR&&Stop.Retained&&!Stop.Clean&&!ReaderCloses&&Stop.HeldAfter);
  if(N==7||N==9)assert(S==EFI_ABORTED&&Stop.Retained&&Stop.ServicesLost&&!Stop.Clean);
  if(N==8)assert(S==EFI_DEVICE_ERROR&&Stop.Retained&&Stop.Released&&!Stop.HeldAfter&&Stop.Cleanup==EFI_DEVICE_ERROR&&!Stop.Clean);
}
int main(VOID){for(UINT32 I=0;I<27;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"display coordinator case%u failed\n",I);return 1;}}puts("Actual display coordinator+Owners+GCC Guard:27 cases; child-first/rail/parent boundaries and verified false-readback rollback; no native hardware executed");return 0;}
