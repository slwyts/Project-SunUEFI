// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual observer + actual guarded LDR/exception lifecycle, host boundary ops.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#define PIANO_GUARDED_HOST_TEST 1
#include "../../uefi/components/guarded-read/PianoGuardedRead.c"
#include "../../uefi/core/PianoDisplaySmmuObserve.c"
#include <Library/PrintLib.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_DXE_SERVICES Ds;STATIC EFI_CPU_ARCH_PROTOCOL Cpu;
STATIC EFI_CPU_INTERRUPT_HANDLER Handlers[4];STATIC EFI_TPL Tpl=TPL_APPLICATION;
STATIC EFI_EVENT_NOTIFY Exit;STATIC VOID *ExitContext;
STATIC BOOLEAN ServicesLive=TRUE,ChangedCpu,Reentered;STATIC jmp_buf FatalJump;
STATIC UINT32 Case,Calls,Loads,BankLoads,Closes,Unregisters,Logs,RouteReads,AtCalls,GcdCalls;STATIC UINT64 Counter=100;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT64 EFIAPI DivU64x32Remainder(UINT64 D,UINT32 V,UINT32 *R){if(R)*R=(UINT32)(D%V);return D/V;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINT16 EFIAPI ReadUnaligned16(CONST VOID *P){UINT16 V;memcpy(&V,P,2);return V;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,4);return V;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"Print assert %s:%lu %s\n",F,(unsigned long)L,D);abort();}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){
  assert(ServicesLive&&L);CHAR8 Buffer[256];VA_LIST Args;VA_START(Args,Fmt);UINTN N=AsciiVSPrint(Buffer,sizeof(Buffer),Fmt,Args);VA_END(Args);
  assert(N&&N<180&&Buffer[N-1]=='\n');Logs++;
}
VOID EFIAPI CpuDeadLoop(VOID){assert(Case==10||Case==31);longjmp(FatalJump,1);}
STATIC VOID Check(VOID){assert(ServicesLive);Calls++;}
STATIC VOID Lost(VOID){assert(Exit);Exit((VOID *)77,ExitContext);ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
STATIC BOOLEAN EFIAPI Alive(VOID){if(Case==25&&!Reentered){Reentered=TRUE;assert(PianoDisplaySmmuObserve("nested",Alive)==EFI_ALREADY_STARTED);}return ServicesLive;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){Check();EFI_TPL Old=Tpl;Tpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){Check();Tpl=Old;}
STATIC VOID EFIAPI Foreign(EFI_EXCEPTION_TYPE T,EFI_SYSTEM_CONTEXT C){(VOID)T;(VOID)C;assert(FALSE);}
STATIC EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){
  Check();assert(P==&Cpu&&(T==0||T==3));
  if(H){if(Handlers[T])return EFI_ALREADY_STARTED;Handlers[T]=H;}
  else{Unregisters++;assert(Handlers[T]==Exception);if(Case==15&&T==3)return EFI_WARN_STALE_DATA;Handlers[T]=NULL;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){Check();assert(G==&gEfiCpuArchProtocolGuid&&!R);*Out=&Cpu;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL Tp,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){Check();assert(T==EVT_NOTIFY_SIGNAL&&Tp==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);Exit=N;ExitContext=(VOID *)C;*E=(VOID *)77;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){Check();assert(E==(VOID *)77);Closes++;return Case==16?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *End){assert(ServicesLive);*First=0;*End=MAX_UINT64;return 1000000;}
UINT64 EFIAPI GetPerformanceCounter(VOID){assert(ServicesLive);return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){*(CPU_STATE *)P=(CPU_STATE){4,1,0x480803514ULL,0xd7fff000,0,0xff44};if(ChangedCpu)((CPU_STATE *)P)->Mair^=1;return EFI_SUCCESS;}
UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *Par){assert(A==BASE||A==BASE+0x1000||A==BANK);AtCalls++;*Par=A;if(Case==18)*Par|=1;if(Case==19)*Par|=4ULL<<56;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){Check();assert(A==BASE||A==BASE+0x1000||A==BANK);GcdCalls++;*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,.Attributes=EFI_MEMORY_UC};if(Case==17)D->Attributes=EFI_MEMORY_WB;if(Case==26)Counter=200000;return EFI_SUCCESS;}
STATIC VOID Inject(UINT32 Type,UINT64 Address){
  EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=0x1000,.FAR=Address+(Case==31?4:0),.ESR=0x96000010,.SPSR=5};EFI_SYSTEM_CONTEXT Context={.SystemContextAArch64=&C};
  assert(Handlers[Type]);Handlers[Type]((EFI_EXCEPTION_TYPE)Type,Context);assert(C.ELR==0x1004);
}
UINT32 PianoGuardedHostLoad(UINTN A){
  assert(m.Armed&&((A>=BASE&&A<BASE+0xe00)||(A>=BASE+0x1000&&A<BASE+0x2000)||(A>=BANK&&A<BANK+0x70)));
  Loads++;if((Case==9||Case==31)&&Loads==10)Inject(0,A);if(Case==10&&Loads==10)Inject(3,A);if(Case==11)ChangedCpu=TRUE;if(Case==12)Lost();
  if(A==BASE)return 0x290406;if(A==BASE+0x20)return Case==6?0:0x4c017e7f;if(A==BASE+0x24)return 0x60000053;if(A==BASE+0x28)return 0x5111;if(A==BASE+0x48)return 0;
  if(A>=BASE+0x800&&A<BASE+0xa00){UINT32 Slot=(UINT32)(A-BASE-0x800)/4;
    if(Case==1)return 0;if(Slot==2)return Case==3?0x80000800:Case==27?0x80030800:0x80020800;
    if(Case==2&&Slot==3)return 0x80000800;return 0;
  }
  if(A>=BASE+0xc00&&A<BASE+0xe00){UINT32 Slot=(UINT32)(A-BASE-0xc00)/4;
    if(Slot==2){RouteReads++;return Case==4?3:Case==5?0x10002:Case==7&&RouteReads>1?3:2;}return 0;
  }
  BankLoads++;
  if(A==BASE+0x1008)return 0x10000;if(A==BASE+0x1808)return 1;
  if(A==BANK)return Case==28?0:0x1e5;if(A==BANK+0x20)return Case==8&&RouteReads>1?0xd7300000:0xd7200000;
  if(A==BANK+0x30)return 0x802519;if(A==BANK+0x10)return 0x38001;
  if(A==BANK+0x38)return 0xff;if(A==BANK+0x58)return Case==29?0x402:0x400;
  return 0;
}
STATIC VOID Setup(UINT32 N){Case=N;gBS=&Bs;gDS=&Ds;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
  if(N==13)Handlers[0]=Foreign;if(N==14)Handlers[3]=Foreign;if(N==23)Tpl=TPL_CALLBACK;if(N==24){ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
}
STATIC VOID Run(UINT32 N){
  Setup(N);if(N==22){assert(PianoDisplaySmmuObserve(NULL,Alive)==EFI_INVALID_PARAMETER&&PianoDisplaySmmuObserve("phase",NULL)==EFI_INVALID_PARAMETER&&PianoDisplaySmmuObserve("12345678901234567890123456789012",Alive)==EFI_INVALID_PARAMETER&&!Calls);return;}
  if(N==20){for(UINT32 I=0;I<32;++I)assert(PianoDisplaySmmuObserve("before-ClockDxe",Alive)==EFI_SUCCESS);UINT32 C=Calls;assert(PianoDisplaySmmuObserve("overflow",Alive)==EFI_OUT_OF_RESOURCES&&Calls==C&&Closes==32&&Unregisters==64);return;}
  if(N==10||N==31){if(!setjmp(FatalJump)){PianoDisplaySmmuObserve("before-ClockDxe",Alive);assert(!"SError/unknown FAR must failstop");}assert(m.Report.Fatal&&m.Report.Retained&&!Unregisters&&!Closes);return;}
  EFI_STATUS S=PianoDisplaySmmuObserve("1234567890123456789012345678901",Alive);
  CONST PIANO_DISPLAY_SMMU_REPORT *R=PianoDisplaySmmuGetReport();assert(R->Count==1);CONST PIANO_DISPLAY_SMMU_SNAPSHOT *P=&R->Snapshot[0];assert(P->Status==S&&!P->Guard.MemoryOwnershipGranted);
  if(N==0||N==21||N==25||N==28||N==29){assert(S==EFI_SUCCESS&&P->Coherent&&P->BankRead&&P->Registers.Matches==1&&P->Registers.Slot==2&&P->Registers.Ttbr0==0xd7200000&&BankLoads==30&&P->EndStatus==EFI_SUCCESS&&Closes==1&&Unregisters==2&&!R->Retained);}
  if(N>=1&&N<=6){assert(S==EFI_NOT_READY&&!P->BankRead&&!BankLoads&&P->EndStatus==EFI_SUCCESS&&!R->Retained&&Closes==1);}
  if(N==7)assert(S==EFI_NOT_READY&&P->Reason==PianoDisplaySmmuChanged&&BankLoads==15&&!R->Retained);
  if(N==8)assert(S==EFI_NOT_READY&&P->Reason==PianoDisplaySmmuChanged&&BankLoads==30&&!R->Retained);
  if(N==9)assert(S==EFI_NOT_READY&&P->Guard.RecoveredFaults==1&&P->EndStatus==EFI_SUCCESS&&!R->Retained&&Closes==1);
  if(N==11||N==15||N==16)assert(S!=EFI_SUCCESS&&R->Retained);
  if(N==12)assert(S==EFI_ABORTED&&R->Retained&&R->ServicesLost&&!Unregisters&&!Closes);
  if(N==13||N==14)assert(S==EFI_ALREADY_STARTED&&!Loads&&!R->Retained&&Closes==1&&Handlers[N==13?0:3]==Foreign);
  if(N>=17&&N<=19)assert(S==EFI_NOT_READY&&!Loads&&!R->Retained&&Closes==1&&Unregisters==2);
  if(N==21){UINT32 C=Calls,L=Logs;gBS=(VOID *)1;gDS=(VOID *)1;memset(&Cpu,0xa5,sizeof(Cpu));assert(PianoDisplaySmmuReemit(Alive)==EFI_SUCCESS&&Calls==C&&Logs==L+6);}
  if(N==23)assert(S==EFI_UNSUPPORTED&&!Loads&&Calls==2&&!Closes&&!R->Retained);
  if(N==24)assert(S==EFI_ABORTED&&!Calls&&R->Retained&&R->ServicesLost);
  if(N==26)assert(S==EFI_TIMEOUT&&!Loads&&Closes==1&&!R->Retained);
  if(N==27)assert(S==EFI_NOT_READY&&P->Reason==PianoDisplaySmmuShape&&!BankLoads&&!R->Retained);
  if(N==28)assert(P->Registers.Sctlr==0);if(N==29)assert(P->Registers.Fsr==0x402);
  if(N==30){PIANO_DISPLAY_SMMU_SNAPSHOT *W=(VOID *)P;
    W->Status=W->BeginStatus=W->EndStatus=EFI_INCOMPATIBLE_VERSION;
    W->Registers.Ttbr0=W->Registers.Ttbr1=W->Registers.Far=MAX_UINT64;
    W->Guard.LastMappingPage=W->Guard.LastPar=W->Guard.LastGcdAttributes=MAX_UINT64;
    W->Guard.LastGcdType=MAX_UINT32;W->Guard.MappingStatus=EFI_INCOMPATIBLE_VERSION;
    UINT32 C=Calls;gBS=(VOID *)1;gDS=(VOID *)1;assert(PianoDisplaySmmuReemit(Alive)==EFI_SUCCESS&&Calls==C);
  }
  if(R->Retained){UINT32 C=Calls;assert(PianoDisplaySmmuObserve("retry",Alive)==EFI_NOT_READY&&Calls==C);}
}
int main(VOID){for(UINT32 I=0;I<32;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"display SMMU case%u failed\n",I);return 1;}}puts("Actual MDSS observer + Guard:32 fork cases, fixed ranges/double snapshot/foreign handlers/fault/EBS/exact End/AsciiVSPrint; no MMIO writes");return 0;}
