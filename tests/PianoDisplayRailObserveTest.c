// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual collector + actual Guard. Selector/native NPA/VCS execution is a
// controlled boundary; source PE hashes/code and protected CPU loads are real.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/sha.h>
#undef NULL
#define PIANO_GUARDED_HOST_TEST 1
#define Exact GuardExact
#define Span GuardSpan
#define Live GuardLive
#define App GuardApp
#include "../bootprofiles/guarded-read/PianoGuardedRead.c"
#undef Exact
#undef Span
#undef Live
#undef App
#include "../bootprofiles/display-rail/PianoDisplayRailObserve.c"
#include <Library/PrintLib.h>
#define NPA_BASE 0xcfe00000ULL
#define VCS_BASE 0xcff00000ULL
#define HEAP_BASE 0xd0000000ULL
#define MM_CLIENT (HEAP_BASE+0x104)
#define MX_CLIENT (HEAP_BASE+0x404)
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiCpuArchProtocolGuid={.Data1=2},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
static PIANO_DISPLAY_RAIL_OBSERVER State;static PIANO_DISPLAY_CLOCK_READ Reader;
static EFI_BOOT_SERVICES Bs;static EFI_DXE_SERVICES Ds;static EFI_CPU_ARCH_PROTOCOL Cpu;static EFI_LOADED_IMAGE_PROTOCOL LoadedImage[2];
static UINT8 *Source[2],*Code[2],*HeapBytes;static BOOLEAN Services=TRUE,Observing;
static UINT32 Case,Calls,Loads,Selects,Allocated,Freed,Logs,Maps,MmStarts,Nested;static UINT64 Counter=100;static EFI_TPL Tpl=TPL_APPLICATION;
static EFI_CPU_INTERRUPT_HANDLER Handlers[4];
typedef struct {EFI_EVENT_NOTIFY Fn;VOID *Context;BOOLEAN Active;} TEST_EVENT;
static TEST_EVENT Events[4096];static UINT32 EventCount,Closed;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *H){return SHA256(P,N,H)!=NULL;}
UINT64 EFIAPI DivU64x32Remainder(UINT64 D,UINT32 V,UINT32 *R){if(R)*R=(UINT32)(D%V);return D/V;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINT16 EFIAPI ReadUnaligned16(CONST VOID *P){UINT16 V;memcpy(&V,P,2);return V;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,4);return V;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"assert%s:%lu %s\n",F,(unsigned long)L,D);abort();}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){assert(Services&&L);CHAR8 B[256];VA_LIST A;VA_START(A,Fmt);UINTN N=AsciiVSPrint(B,sizeof(B),Fmt,A);VA_END(A);assert(N&&N<180&&B[N-1]=='\n');Logs++;}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"unexpected fatal fault");abort();}
static VOID Put32(UINT64 A,UINT32 V){memcpy((VOID *)(UINTN)A,&V,4);}static VOID Put64(UINT64 A,UINT64 V){memcpy((VOID *)(UINTN)A,&V,8);}
static VOID Boundary(VOID){assert(Services);Calls++;}
static VOID Reenter(VOID){assert(State.Report.Busy);assert(PianoDisplayRailInit(&State,&State.Env)==EFI_ALREADY_STARTED&&PianoDisplayRailClose(&State)==EFI_ALREADY_STARTED&&PianoDisplayRailObserve(&State,"nested")==EFI_ALREADY_STARTED&&PianoDisplayRailReemit(&State)==EFI_ALREADY_STARTED);Nested++;}
static BOOLEAN TestAlive(VOID *C){assert(C==&Reader);if(Case==31&&!Nested&&State.Signature==RAIL_SIGNATURE)Reenter();return Services;}
static VOID Lose(VOID){for(UINT32 I=0;I<EventCount;++I)if(Events[I].Active)Events[I].Fn(&Events[I],Events[I].Context);Services=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
static EFI_TPL EFIAPI Raise(EFI_TPL N){Boundary();EFI_TPL O=Tpl;Tpl=N;return O;}static VOID EFIAPI Restore(EFI_TPL N){Boundary();Tpl=N;}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){Boundary();assert(P==&Cpu&&(T==0||T==3));if(H){if(Handlers[T])return EFI_ALREADY_STARTED;Handlers[T]=H;}else{assert(Handlers[T]==Exception);Handlers[T]=NULL;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){Boundary();assert(!R);*P=G==&gEfiCpuArchProtocolGuid?(VOID *)&Cpu:(VOID *)(UINTN)(NPA_BASE+0x110b8);if(Case==3&&G!=&gEfiCpuArchProtocolGuid)*P=(VOID *)(UINTN)(NPA_BASE+0x110b0);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *R,UINTN *N,EFI_HANDLE **P){Boundary();assert(T==ByProtocol&&G==&gEfiLoadedImageProtocolGuid&&!R);*N=Case==4?1:Case==5?3:2;*P=malloc(*N*sizeof(**P));assert(*P);Allocated++;for(UINTN I=0;I<*N;++I)(*P)[I]=(VOID *)(I==2?2:I+1);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){Boundary();assert((H==(VOID *)1||H==(VOID *)2)&&G==&gEfiLoadedImageProtocolGuid);*P=&LoadedImage[(UINTN)H-1];if(Case==4&&H==(VOID *)2)return EFI_NOT_FOUND;if(Case==5&&H==(VOID *)2)*P=&LoadedImage[0];if(Case==13){Lose();return EFI_SUCCESS;}return Case==9?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI FreePool(VOID *P){Boundary();if(Case==29&&!Nested)Reenter();if(Case==11&&P==State.Image[0].Copy)return EFI_WARN_STALE_DATA;free(P);Freed++;if(Case==33&&!Observing)Lose();return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL Tp,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *P){Boundary();assert(T==EVT_NOTIFY_SIGNAL&&Tp==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid&&EventCount<4096);TEST_EVENT *E=&Events[EventCount++];*E=(TEST_EVENT){Fn,(VOID *)C,TRUE};*P=E;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT P){Boundary();TEST_EVENT *E=P;assert(E->Active);if((Case==10&&E->Fn==Exit)||(Case==26&&Observing&&E->Fn==ExitNotify))return EFI_WARN_STALE_DATA;E->Active=FALSE;Closed++;return EFI_SUCCESS;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 Type,UINTN Index,VOID **P,UINTN *N){Boundary();assert(Type==EFI_SECTION_PE32&&!Index);if(Case==28&&!Nested)Reenter();UINT32 I=G->Data1==mPin[0].Guid.Data1?0:1;*N=mPin[I].Bytes;*P=malloc(*N);assert(*P);memcpy(*P,Source[I],*N);Allocated++;if(Case==7)((UINT8 *)*P)[0x1004]^=1;return Case==8?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI MemoryMap(UINTN *N,EFI_MEMORY_DESCRIPTOR *D,UINTN *Key,UINTN *Stride,UINT32 *Version){Boundary();Maps++;assert(*N>=3*sizeof(*D));*N=3*sizeof(*D);*Key=Maps;*Stride=sizeof(*D);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;
  D[0]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesCode,.PhysicalStart=NPA_BASE,.NumberOfPages=0x13,.Attribute=EFI_MEMORY_UC|EFI_MEMORY_WB};
  D[1]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=VCS_BASE,.NumberOfPages=0x11,.Attribute=EFI_MEMORY_WB};
  D[2]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=HEAP_BASE,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};if(Case==23&&Observing)D[2].Attribute=EFI_MEMORY_UC;if(Case>=36&&Observing)D[1].Type=EfiBootServicesCode;
  if(Case==44&&Observing)D[2].Type=EfiBootServicesCode;
  if(Case==43&&Observing&&MmStarts>=2){EFI_MEMORY_DESCRIPTOR T=D[0];D[0]=D[2];D[2]=T;}
  if(Case==32&&Observing)Lose();return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){Boundary();assert((A>=NPA_BASE&&A<NPA_BASE+0x13000)||(A>=VCS_BASE&&A<VCS_BASE+0x11000)||(A>=HEAP_BASE&&A<HEAP_BASE+0x10000));*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=EfiGcdMemoryTypeSystemMemory,.Attributes=EFI_MEMORY_WB};if(Case==24&&Observing&&A>=HEAP_BASE)D->Attributes=EFI_MEMORY_WT;return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *A,UINT64 *B){assert(Services);*A=0;*B=MAX_UINT64;return 1000000;}UINT64 EFIAPI GetPerformanceCounter(VOID){assert(Services);return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){*(CPU_STATE *)P=(CPU_STATE){4,1,0x480803514ULL,0xd7fff000,0,0xff44};return EFI_SUCCESS;}UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *P){*P=A|(0xffULL<<56);return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN A){assert(Services&&m.Armed&&((A>=NPA_BASE&&A<NPA_BASE+0x13000)||(A>=VCS_BASE&&A<VCS_BASE+0x11000)||(A>=HEAP_BASE&&A<HEAP_BASE+0x10000)));Loads++;
  if(Case==12&&!Observing&&Loads==2){Lose();return 0;}
  if(Case==25&&Observing&&A==MM_CLIENT+0x20){EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=0x1000,.FAR=A,.ESR=0x96000010,.SPSR=5};EFI_SYSTEM_CONTEXT U={.SystemContextAArch64=&C};Handlers[0](0,U);assert(C.ELR==0x1004);}
  if(Observing&&A==MM_CLIENT+0x20)MmStarts++;
  if(Case==20&&MmStarts==2&&A==MM_CLIENT+0x20)Put32(HEAP_BASE+0x1230,0x80);
  if(Case==47&&Observing&&MmStarts==2&&A==HEAP_BASE+0x1400)Put32(HEAP_BASE+0x140c,0xaabbccdd);
  UINT32 V;memcpy(&V,(VOID *)(UINTN)A,4);return V;
}
EFI_STATUS PianoDisplayClockReadSnapshotClock(PIANO_DISPLAY_CLOCK_READ *R,PIANO_DISPLAY_CLOCK_SELECTOR Which,PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *P){
  assert(R==&Reader&&Which==PianoClockSelectNonGdscAhb&&Services&&!m.Report.Active&&!Handlers[0]&&!Handlers[3]);Selects++;ZeroMem(P,sizeof(*P));
  if(Case==30&&!Nested)Reenter();
  if(Case==21)return EFI_WARN_STALE_DATA;P->Revision=1;P->Selector=Which;P->MatchingSnapshots=2;P->ExpectedClockId=0x02010006;P->ParentRailMask=8;P->CurrentCorner=0x38;P->MmClient=Case==1?0:MM_CLIENT;P->MxClient=Case==2?0:MX_CLIENT;
  if(Case==22&&Selects==2)P->MmClient+=4;return EFI_SUCCESS;
}
static VOID MakeGraph(UINT64 Client,UINT64 Offset,CONST CHAR8 *NameText){UINT64 Resource=HEAP_BASE+Offset,Def=Resource+0x100,Node=Resource+0x180,Rail=Resource+0x200,NameAddress=Resource+0x400,Context=Resource+0x500;
  Put64(Client+0x20,Resource);Put32(Client+0x30,0x40);Put32(Client+0x68,0);Put32(Client+0x38,0x38);Put32(Client+0x50,0x38);Put64(Client+0x88,NPA_BASE+0x9714);
  Put64(Resource,Def);Put64(Resource+0x10,Node);Put64(Resource+0x28,VCS_BASE+0x98d8);Put32(Resource+0x30,0x38);Put32(Resource+0x40,0x38);Put32(Resource+0x44,0);Put64(Def,NameAddress);Put64(Def+0x28,Rail);Put64(Node+8,VCS_BASE+0x6a8c);Put64(Node+0x18,Rail);
  Put64(Rail+0xb8,Resource);Put64(Rail+0x78,NameAddress);Put64(Rail+0x20,VCS_BASE+0xa2c0);Put32(Rail+0x30,0x38);Put64(Rail+0x28,Context);Put64(Context+0x10,HEAP_BASE+0x9000);Put32(HEAP_BASE+0x9000,2);Put64(HEAP_BASE+0x9008,HEAP_BASE+0x9040);memcpy((VOID *)(UINTN)NameAddress,NameText,strlen(NameText)+1);
}
static VOID StaticGraph(UINT64 Client,UINT64 Offset,UINT32 Index){UINT64 Resource=HEAP_BASE+Offset,Rail=VCS_BASE+0xa488+0x120*Index,Def=Rail+0x78,Node=Rail+0x38;
  Put64(Resource,Def);Put64(Resource+0x10,Node);Put64(Def,Resource+0x400);Put64(Def+0x28,Rail);Put64(Node+8,VCS_BASE+0x6a8c);Put64(Node+0x18,Rail);
  UINT64 Context=VCS_BASE+0xbb08+0x328*Index;Put64(Rail+0xb8,Resource);Put64(Rail+0x20,VCS_BASE+0xa2c0);Put32(Rail+0x30,0x38);Put64(Rail+0x28,Context);Put64(Context+0x10,Context+0x290);Put32(Context+0x290,2);Put64(Context+0x298,HEAP_BASE+0x9040);(VOID)Client;
}
static VOID Relocate(UINT32 I){for(UINTN A=mPin[I].Reloc;A<mPin[I].Bytes;){UINT32 Page,N;memcpy(&Page,Source[I]+A,4);memcpy(&N,Source[I]+A+4,4);if(!N)break;assert(N>=8&&N<=mPin[I].Bytes-A);for(UINTN X=A+8;X<A+N;X+=2){UINT16 V;memcpy(&V,Source[I]+X,2);if((V>>12)==10){UINT64 P;UINTN R=Page+(V&4095);assert(R<=mPin[I].Bytes-8);memcpy(&P,Code[I]+R,8);P+=I?VCS_BASE:NPA_BASE;memcpy(Code[I]+R,&P,8);}}A+=N;}}
static VOID Run(UINT32 Number){Case=Number;for(UINT32 I=0;I<2;++I){UINT64 B=I?VCS_BASE:NPA_BASE;Code[I]=mmap((VOID *)(UINTN)B,mPin[I].Bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Code[I]==(VOID *)(UINTN)B);memcpy(Code[I],Source[I],mPin[I].Bytes);Relocate(I);LoadedImage[I]=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Code[I],.ImageSize=mPin[I].Bytes,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};}
  HeapBytes=mmap((VOID *)(UINTN)HEAP_BASE,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(HeapBytes==(VOID *)(UINTN)HEAP_BASE);MakeGraph(MM_CLIENT,0x1000,"/vcs/vdd_mm");MakeGraph(MX_CLIENT,0x3000,"/vcs/vdd_mx");
  if(Case==6)Code[1][0x1004]^=1;if(Case==27)Put64(NPA_BASE+0x110b8,0x10004);
  Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.FreePool=FreePool,.CreateEventEx=Create,.CloseEvent=CloseEvent,.GetMemoryMap=MemoryMap};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;gBS=&Bs;gDS=&Ds;
  PIANO_DISPLAY_RAIL_ENV Env={.Context=&Reader,.Services=&Bs,.DxeServices=&Ds,.Alive=TestAlive,.ClockReader=&Reader,.NpaHandle=(VOID *)1,.VcsHandle=(VOID *)2};EFI_STATUS E=PianoDisplayRailInit(&State,&Env);
  if((Case>=3&&Case<=13)||Case==27){if(Case==10||Case==11){assert(E==EFI_SUCCESS);E=PianoDisplayRailClose(&State);assert(E==EFI_DEVICE_ERROR&&State.Report.Retained);return;}
    assert(E!=EFI_SUCCESS&&!State.Report.Initialized);if(Case==8||Case==12||Case==13){assert(Case==8?State.Report.Retained:State.Report.ServicesLost);assert(PianoDisplayRailClose(&State)==EFI_ACCESS_DENIED);return;}
    assert(PianoDisplayRailClose(&State)==EFI_SUCCESS&&Allocated==Freed&&Closed==EventCount);return;}
  assert(E==EFI_SUCCESS&&State.Report.Initialized&&State.Image[0].CodeVerified&&State.Image[1].CodeVerified);Observing=TRUE;
  if(Case>=36){Put64(VCS_BASE+0xfa30,VCS_BASE+0xa468);Put64(VCS_BASE+0xa478,VCS_BASE+0xa488);Put32(VCS_BASE+0xa480,2);StaticGraph(MM_CLIENT,0x1000,0);StaticGraph(MX_CLIENT,0x3000,1);}
  if(Case==37)Put64(VCS_BASE+0xfa30,VCS_BASE+0xa470);if(Case==38)Put32(VCS_BASE+0xa480,0);if(Case==39)Put32(VCS_BASE+0xa480,21);
  if(Case==40)StaticGraph(MM_CLIENT,0x1000,19);if(Case==41)Put64(HEAP_BASE+0x1000,VCS_BASE+0xa488+0x120-4);if(Case==42)Put64(HEAP_BASE+0x1010,VCS_BASE+0xbb08);
  if(Case==45)Put64(VCS_BASE+0xa4b0,VCS_BASE+0xbb08+0x328);if(Case==46)Put64(VCS_BASE+0xbb18,VCS_BASE+0xbb08+0x290+4);
  if(Case==34)memcpy((VOID *)(UINTN)(HEAP_BASE+0x3400),"/vcs/vdd_mxa",13);
  if(Case==35)Put64(VCS_BASE+0x9908,VCS_BASE+0x6d68);
  if(Case==14)Put32(MM_CLIENT+0x30,0x800);if(Case==15)Put64(HEAP_BASE+0x1188,VCS_BASE+0x6a90);if(Case==16)Put64(HEAP_BASE+0x12b8,HEAP_BASE+0x3000);if(Case==17)Put64(HEAP_BASE+0x1220,VCS_BASE+0xa2c8);if(Case==18)Put32(MM_CLIENT+0x68,2);if(Case==19)Put64(MM_CLIENT+0x88,VCS_BASE+0x1000);
  E=PianoDisplayRailObserve(&State,"actual-mm-object-graph");CONST PIANO_DISPLAY_RAIL_SNAPSHOT *R=&State.Report.Snapshot[0];assert(State.Report.Count==1&&!R->PowerReady&&!R->MemoryOwnershipGranted&&!R->RpmhCompletionObserved);
  if(Case==0||Case==2||(Case>=28&&Case<=31)||Case==33||Case==34||Case==36||Case==43||Case==47){if(E!=EFI_SUCCESS)fprintf(stderr,"rail positive status%lx mm=%lx/%lx mx=%lx/%lx guard=%lx last=%s/%lx map=%u/%u\n",(unsigned long)E,(unsigned long)R->Mm[0].Status,(unsigned long)R->Mm[1].Status,(unsigned long)R->Mx[0].Status,(unsigned long)R->Mx[1].Status,(unsigned long)State.Report.Guard.Status,R->Mm[0].LastRead.Field,(unsigned long)R->Mm[0].LastRead.Address,R->Mm[0].LastRead.Map.Reason,R->Mm[0].LastRead.Map.DescriptorType);
    assert(E==EFI_SUCCESS&&R->MmCoherent&&R->Mm[0].Client==MM_CLIENT&&R->Mm[0].ActiveRequest==0x38&&R->Mm[0].NpaApplied==0x38&&R->Mm[0].VcsApplied==0x38&&!State.Report.Retained&&Selects==2);if(Case==2)assert(!R->MxCoherent&&R->Mx[0].Status==EFI_NOT_READY&&R->Mx[0].LastRead.Status==EFI_NOT_STARTED&&!R->Mx[0].LastRead.Address);
    UINT32 C=Calls,L=Loads;assert(PianoDisplayRailReemit(&State)==EFI_SUCCESS&&Calls==C&&Loads==L);}
  else{assert(E!=EFI_SUCCESS);if(Case==26)assert(State.Report.Retained&&m.Report.Retained&&PianoDisplayRailClose(&State)==EFI_ACCESS_DENIED);}
  if(Case==36)assert(State.Report.StaticProducer==EFI_SUCCESS&&State.Report.StaticCount==2&&R->Mm[0].Definition==VCS_BASE+0xa500&&R->Mm[0].Node==VCS_BASE+0xa4c0);
  if(Case==43){assert(R->MmCoherent);PIANO_DISPLAY_RAIL_GRAPH A=R->Mm[0],B=R->Mm[1];B.LastRead.Map.DescriptorIndex=A.LastRead.Map.DescriptorIndex+1;B.LastRead.Map.MapKey=42;assert(SameGraph(&A,&B));}
  if(Case==47){assert(R->MmCoherent&&!CompareMem(R->Mm[0].ResourceName,R->Mm[1].ResourceName,16));for(UINT32 I=12;I<16;++I)assert(!R->Mm[1].ResourceName[I]&&!R->Mm[1].RailName[I]);}
  if(Case==23||Case==44){assert(!strcmp(R->Mm[0].LastRead.Field,"Resource")&&R->Mm[0].LastRead.Address==MM_CLIENT+0x20&&R->Mm[0].LastRead.Status==EFI_NOT_READY&&R->Mm[0].LastRead.GuardMapping==EFI_NOT_STARTED);assert(R->Mm[0].LastRead.Map.Reason==(Case==23?PianoClockEfiMapCache:PianoClockEfiMapWrongType));}
  if(Case==24)assert(R->Mm[0].LastRead.Map.Reason==PianoClockEfiMapReady&&R->Mm[0].LastRead.GuardAttributes==EFI_MEMORY_WT&&R->Mm[0].LastRead.GuardMapping==EFI_NOT_READY);
  if(Case==42)assert(State.Report.StaticProducer==EFI_NOT_READY&&!strcmp(R->Mm[0].LastRead.Field,"StaticCount"));
  if(Case==33){Observing=FALSE;assert(PianoDisplayRailClose(&State)==EFI_ABORTED&&State.Report.Retained&&State.Report.ServicesLost&&State.Image[0].Copy&&!State.Report.Busy);return;}
  if(!State.Report.Retained){assert(PianoDisplayRailClose(&State)==EFI_SUCCESS&&Allocated==Freed&&Closed==EventCount&&!Handlers[0]&&!Handlers[3]);}
  if(Case>=28&&Case<=31)assert(Nested==1);
}
int main(int argc,char **argv){assert(argc==3);for(UINT32 I=0;I<2;++I){FILE *F=fopen(argv[I+1],"rb");assert(F);Source[I]=malloc(mPin[I].Bytes);assert(Source[I]&&fread(Source[I],1,mPin[I].Bytes,F)==mPin[I].Bytes&&fgetc(F)==EOF);fclose(F);}
  for(UINT32 I=0;I<48;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int E;assert(waitpid(P,&E,0)==P);if(!WIFEXITED(E)||WEXITSTATUS(E)){fprintf(stderr,"rail observer case%u failed\n",I);return 1;}}puts("Actual Rail collector+Guard+NPA/VCS PE pin:48 cases; pinned static row Code/Data admission, dynamic Code rejection, real last-field/map diagnostics, semantic pairs/name-tail normalization, no native request/MMIO/ready grant");return 0;}
