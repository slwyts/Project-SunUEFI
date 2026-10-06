// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Guard/collector and raw formatter. No DISPCC or DPU load is allowed.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#define PIANO_GUARDED_HOST_TEST 1
#include "../bootprofiles/guarded-read/PianoGuardedRead.c"
#include "../bootprofiles/uefi-app/PianoDisplayClockObserve.c"
#include <Library/PrintLib.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_DXE_SERVICES Ds;STATIC EFI_CPU_ARCH_PROTOCOL Cpu;
STATIC EFI_CPU_INTERRUPT_HANDLER Handlers[4];STATIC EFI_TPL Tpl=TPL_APPLICATION;
STATIC EFI_EVENT_NOTIFY Exit;STATIC VOID *ExitContext;STATIC BOOLEAN ServicesLive=TRUE;
STATIC UINT32 Case,Calls,Loads,Closes,Unregisters,Logs,AhbReads,HfReads;STATIC UINT64 Counter=100;STATIC jmp_buf FatalJump;
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
VOID EFIAPI CpuDeadLoop(VOID){assert(Case==8);longjmp(FatalJump,1);}
STATIC VOID Check(VOID){assert(ServicesLive);Calls++;}
STATIC BOOLEAN EFIAPI Alive(VOID){return ServicesLive;}
STATIC VOID Lost(VOID){Exit((VOID *)77,ExitContext);ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){Check();EFI_TPL Old=Tpl;Tpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){Check();Tpl=Old;}
STATIC VOID EFIAPI Foreign(EFI_EXCEPTION_TYPE T,EFI_SYSTEM_CONTEXT C){(VOID)T;(VOID)C;assert(FALSE);}
STATIC EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){Check();assert(P==&Cpu&&(T==0||T==3));if(H){if(Handlers[T])return EFI_ALREADY_STARTED;Handlers[T]=H;}else{assert(Handlers[T]==Exception);Unregisters++;if(Case==9&&T==3)return EFI_WARN_STALE_DATA;Handlers[T]=NULL;}return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){Check();assert(G==&gEfiCpuArchProtocolGuid&&!R);*P=&Cpu;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){Check();assert(T==EVT_NOTIFY_SIGNAL&&P==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);Exit=N;ExitContext=(VOID *)C;*E=(VOID *)77;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){Check();assert(E==(VOID *)77);Closes++;return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *A,UINT64 *B){assert(ServicesLive);*A=0;*B=MAX_UINT64;return 1000000;}
UINT64 EFIAPI GetPerformanceCounter(VOID){assert(ServicesLive);return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){*(CPU_STATE *)P=(CPU_STATE){4,1,0x480803514ULL,0xd7200000,0,0xff4400};return EFI_SUCCESS;}
UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *P){assert(A==0x127000||A==0xaf08000||A==0xaf09000);*P=A;if(Case==5&&A==0x127000)*P|=1;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){Check();assert(A==0x127000||A==0xaf08000||A==0xaf09000);*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,.Attributes=EFI_MEMORY_UC};if((Case==4&&A==0x127000)||(Case==6&&A>=0xaf08000))D->GcdMemoryType=EfiGcdMemoryTypeReserved;return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN A){
  assert(m.Armed&&(A==0x127004||A==0x127008));Loads++;
  if((Case==7||Case==8)&&Loads==1){EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=0x1000,.FAR=A,.ESR=0x96000010,.SPSR=5};EFI_SYSTEM_CONTEXT Context={.SystemContextAArch64=&C};Handlers[Case==8?3:0](Case==8?3:0,Context);assert(C.ELR==0x1004);}
  if(Case==10)Lost();
  if(A==0x127004){AhbReads++;return Case==1?0:Case==2&&AhbReads>1?0:1;}
  HfReads++;return Case==3&&HfReads>1?3:2;
}
STATIC VOID Setup(UINT32 N){Case=N;gBS=&Bs;gDS=&Ds;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;if(N==11)Handlers[0]=Foreign;if(N==12)Handlers[3]=Foreign;}
STATIC VOID Run(UINT32 N){Setup(N);
  if(N==8){if(!setjmp(FatalJump)){PianoDisplayClockObserve("pre:ClockDxe",Alive);assert(!"SError must halt");}assert(m.Report.Fatal&&m.Report.Retained&&!Unregisters&&!Closes);return;}
  if(N==14){for(UINT32 I=0;I<32;++I)assert(PianoDisplayClockObserve("phase",Alive)==EFI_NOT_READY);UINT32 C=Calls;assert(PianoDisplayClockObserve("overflow",Alive)==EFI_OUT_OF_RESOURCES&&Calls==C);return;}
  if(N==15){assert(PianoDisplayClockObserve(NULL,Alive)==EFI_INVALID_PARAMETER&&PianoDisplayClockObserve("phase",NULL)==EFI_INVALID_PARAMETER&&PianoDisplayClockObserve("12345678901234567890123456789012",Alive)==EFI_INVALID_PARAMETER&&!Calls);return;}
  EFI_STATUS Status=PianoDisplayClockObserve("1234567890123456789012345678901",Alive);CONST PIANO_DISPLAY_CLOCK_REPORT *R=PianoDisplayClockGetReport();assert(R->Count==1);CONST PIANO_DISPLAY_CLOCK_SNAPSHOT *S=&R->Snapshot[0];assert(Status==S->Status&&!S->ControllerBusHeld&&!S->DpuDomainClockHeld);
  for(UINT32 I=2;I<16;++I)assert(!S->Register[I].Attempts&&S->Register[I].ReadStatus[0]==EFI_NOT_STARTED&&S->Register[I].ReadStatus[1]==EFI_NOT_STARTED);
  for(UINT32 I=10;I<16;++I)assert(S->Register[I].Skip==PianoClockDpuLeaseMissing);
  if(N==0||N==13||N==16)assert(Status==EFI_NOT_READY&&S->GccCoherent&&S->GccAhbEnabled&&S->DispccMappingQualified&&Loads==4&&Closes==2&&Unregisters==4&&!R->Retained&&S->DispccGuard.Reads==0);
  if(N==1)assert(Status==EFI_NOT_READY&&S->GccCoherent&&!S->GccAhbEnabled&&Loads==4&&Closes==1&&S->Register[2].Skip==PianoClockGccAhbDisabled);
  if(N==2||N==3)assert(Status==EFI_NOT_READY&&!S->GccCoherent&&Loads==4&&Closes==1&&S->Register[2].Skip==PianoClockGccUnstable);
  if(N==4||N==5)assert(Status==EFI_NOT_READY&&!Loads&&Closes==1&&!R->Retained);
  if(N==6)assert(Status==EFI_NOT_READY&&Loads==4&&Closes==2&&!S->DispccMappingQualified&&S->Register[2].Skip==PianoClockMappingUnavailable&&!R->Retained);
  if(N==7)assert(Status==EFI_NOT_READY&&Loads==1&&S->GccGuard.RecoveredFaults==1&&Closes==1&&!R->Retained);
  if(N==9)assert(Status==EFI_DEVICE_ERROR&&Loads==4&&R->Retained&&!Closes);
  if(N==10)assert(Status==EFI_ABORTED&&Loads==1&&R->Retained&&R->ServicesLost&&!Unregisters&&!Closes);
  if(N==11||N==12)assert(Status==EFI_ALREADY_STARTED&&!Loads&&!R->Retained&&Closes==1&&Handlers[N==11?0:3]==Foreign);
  if(N==13){UINT32 C=Calls,L=Loads;gBS=(VOID *)1;gDS=(VOID *)1;memset(&Cpu,0xa5,sizeof(Cpu));assert(PianoDisplayClockReemit(Alive)==EFI_SUCCESS&&Calls==C&&Loads==L);}
  if(N==16){PIANO_DISPLAY_CLOCK_SNAPSHOT *W=(VOID *)S;W->Status=W->GccBegin=W->GccEnd=W->DispccBegin=W->DispccEnd=EFI_INCOMPATIBLE_VERSION;for(UINT32 I=0;I<16;++I){W->Register[I].ReadStatus[0]=W->Register[I].ReadStatus[1]=EFI_INCOMPATIBLE_VERSION;W->Register[I].Value[0]=W->Register[I].Value[1]=MAX_UINT32;}assert(PianoDisplayClockReemit(Alive)==EFI_SUCCESS);}
  if(R->Retained){UINT32 C=Calls;assert(PianoDisplayClockObserve("retry",Alive)==EFI_NOT_READY&&Calls==C);}
}
int main(VOID){for(UINT32 I=0;I<17;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"display clock case%u failed\n",I);return 1;}}puts("Actual Guard/clock collector:17 cases, GCC double reads, zero-load DISPCC qualification, all8+6 unproven safety skipped, no ready claim");return 0;}
