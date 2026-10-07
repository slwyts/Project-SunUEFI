// SPDX-License-Identifier: BSD-2-Clause-Patent
// Every owner/reader/collector is actual linked source. Only native ARM and
// EFI/CPU boundaries are fixtures; three native PE pins/relocations are real.
#include <stdint.h>
static unsigned ChildCase,ChildGet,ChildEnable,ChildDisable,ChildQueries;
static int JointFaultPhase;
static int JointCloseWarning(void *Event);
static void JointMmState(void);
#define main OriginalPipelineMain
#include "ActualNonGdscJointFixture.h"
#undef main
#include <Library/PrintLib.h>
#include "../../uefi/core/PianoDisplayClockObserve.h"
#include "../../uefi/components/display-rail/PianoDisplayNonGdscClock.h"
#define NPA_BASE 0xcfe00000ULL
#define VCS_BASE 0xcff00000ULL
#define HEAP_BASE HEAP
#define MM_CLIENT (HEAP+0x4104)
#define MX_CLIENT (HEAP+0x4404)
#define CHILD_NODE (BASE+0x2e520)
#define CHILD_PARENT (BASE+0x30478)
#define CHILD_REF (HEAP+0x1800)
#define Child mDisplay.Child
EFI_SYSTEM_TABLE *gST;
static UINT8 *Source[2],*Code[2];static EFI_LOADED_IMAGE_PROTOCOL RailLoaded[2];
static struct {UINT32 Bytes,Reloc;} JointPin[2]={{0x13000,0x12000},{0x11000,0x10000}};
static UINT32 Logs,StaticLoads,ImageLoads;
static VOID Put32(UINT64 A,UINT32 V){memcpy((VOID*)(UINTN)A,&V,4);}
#include "ActualRailFixtureHelpers.h"
#include "ActualChildNativeBoundary.h"
static EFI_STATUS JointLocate(EFI_GUID *G,VOID *R,VOID **O){Boundary();assert(!R);*O=G==&gEfiCpuArchProtocolGuid?(VOID*)&Cpu:G->Data1==0x79d6c870?(VOID*)(UINTN)(NPA_BASE+0x110b8):(VOID*)(UINTN)(BASE+0x28148);return EFI_SUCCESS;}
static EFI_STATUS JointHandle(EFI_HANDLE H,EFI_GUID *G,VOID **O){Boundary();assert(G==&gEfiLoadedImageProtocolGuid);if(H==(VOID*)5)*O=&Loaded;else{assert(H==(VOID*)1||H==(VOID*)2);*O=&RailLoaded[(UINTN)H-1];}return EFI_SUCCESS;}
static EFI_STATUS JointSection(CONST EFI_GUID *G,UINT8 T,UINTN I,VOID **O,UINTN *N){Boundary();assert(T==EFI_SECTION_PE32&&!I);UINT8 *Bytes;if(G->Data1==0x4db5dea6){*N=IMAGE_BYTES;Bytes=File;}else{UINT32 Id=G->Data1==0xcb29f4d1?0:1;assert(!Id||G->Data1==0x8bd3b475);*N=JointPin[Id].Bytes;Bytes=Source[Id];}*O=malloc(*N);assert(*O);memcpy(*O,Bytes,*N);PoolAllocations++;return EFI_SUCCESS;}
static EFI_STATUS JointMap(UINTN *N,EFI_MEMORY_DESCRIPTOR *D,UINTN *K,UINTN *Stride,UINT32 *Version){Boundary();assert(*N>=4*sizeof(*D));*N=4*sizeof(*D);*K=BsCalls;*Stride=sizeof(*D);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;
 UINT64 B[4]={BASE,NPA_BASE,VCS_BASE,HEAP},Pages[4]={IMAGE_BYTES/4096,0x13,0x11,16};for(UINT32 I=0;I<4;++I)D[I]=(EFI_MEMORY_DESCRIPTOR){.Type=I==3?EfiBootServicesData:EfiBootServicesCode,.PhysicalStart=B[I],.NumberOfPages=Pages[I],.Attribute=EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB};return EFI_SUCCESS;}
static EFI_STATUS JointGcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){Boundary();BOOLEAN Mmio=A==0x127000||A==0xaf08000||A==0xaf09000;*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=Mmio?EfiGcdMemoryTypeMemoryMappedIo:EfiGcdMemoryTypeSystemMemory,.Attributes=Mmio?EFI_MEMORY_UC:EFI_MEMORY_WB};return EFI_SUCCESS;}
static EFI_STATUS JointAt(UINTN A,UINT64 *P){*P=A|((A==0x127000||A==0xaf08000||A==0xaf09000)?0:0xffULL<<56);return EFI_SUCCESS;}
static UINT32 JointLoad(UINTN A){assert(Services&&m.Armed);Loads++;if(A==0x127004||A==0x127008){GccLoads++;return A==0x127004?Ahb:Hf;}
 assert((A>=BASE&&A<BASE+IMAGE_BYTES)||(A>=NPA_BASE&&A<NPA_BASE+0x13000)||(A>=VCS_BASE&&A<VCS_BASE+0x11000)||(A>=HEAP&&A<HEAP+0x10000));
 if(A>=VCS_BASE+0xa488&&A<VCS_BASE+0xfa28)StaticLoads++;if((A>=NPA_BASE&&A<NPA_BASE+0x13000)||(A>=VCS_BASE&&A<VCS_BASE+0x11000))ImageLoads++;CpuLoads++;UINT32 V;memcpy(&V,(VOID*)A,4);return V;}
static int JointCloseWarning(void *Event){EVENT *E=Event;return JointFaultPhase&&((ChildCase==2&&!E->Guard&&E->Context==&Child)||(ChildCase==5&&E->Guard&&m.Config.Context==&mDisplay.Rail));}
static VOID JointMmState(void){
 UINT32 Value=*(UINT32*)(UINTN)(CHILD_PARENT+0x4c),Active=(*(UINT32*)(UINTN)(MM_CLIENT+0x68))^1;Put32(MM_CLIENT+0x68,Active);Put32(MM_CLIENT+0x38+24*Active,Value);
 Put32(HEAP+0x5030,Value);Put32(HEAP+0x5040,Value);Put32(VCS_BASE+0xa488+0x30,Value);
}
static VOID JointPrepare(VOID){
 Case=200;Image=mmap((VOID*)(UINTN)BASE,IMAGE_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Image==(VOID*)(UINTN)BASE);Heap=mmap((VOID*)(UINTN)HEAP,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Heap==(VOID*)(UINTN)HEAP);
 memcpy(Image,File,IMAGE_BYTES);Relocate();Put64(BASE+0x3fe48,BASE+0x283a0);Put64(BASE+0x3f5f0,HEAP);Put64(BASE+0x3fed8,HEAP);Put64(BASE+0x3fed0,HEAP+0x2000);Put64(HEAP+0x10,HEAP+0x2000);Put64(HEAP+0x2010,BASE+0x25533);Put64(HEAP+0x2018,HEAP);Put64(NODE+0x58,0);Put16(NODE+0x50,0);Put16(NODE+0x52,0);Put64(CHILD_NODE+0x58,0);Put16(CHILD_NODE+0x50,0);Put16(CHILD_NODE+0x52,0);
 for(UINT32 I=0;I<2;++I){UINT64 B=I?VCS_BASE:NPA_BASE;Code[I]=mmap((VOID*)(UINTN)B,JointPin[I].Bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Code[I]==(VOID*)(UINTN)B);memcpy(Code[I],Source[I],JointPin[I].Bytes);RailRelocate(I);RailLoaded[I]=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Code[I],.ImageSize=JointPin[I].Bytes,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};}
 Put64(VCS_BASE+0xfa30,VCS_BASE+0xa468);Put64(VCS_BASE+0xa478,VCS_BASE+0xa488);Put32(VCS_BASE+0xa480,2);
 MakeGraph(MM_CLIENT,0x5000,"/vcs/vdd_mm");MakeGraph(MX_CLIENT,0x6000,"/vcs/vdd_mxa");StaticGraph(MM_CLIENT,0x5000,0);StaticGraph(MX_CLIENT,0x6000,1);
 Put64(BASE+0x3df68,MM_CLIENT);Put64(BASE+0x3e128,ChildCase==1?0:MX_CLIENT);
 Put32(MM_CLIENT+0x38,0);Put32(MM_CLIENT+0x50,0);Put32(HEAP+0x5030,0);Put32(HEAP+0x5040,0);Put32(VCS_BASE+0xa488+0x30,0);
 if(ChildCase==3)Put64(VCS_BASE+0xa478,VCS_BASE+0xa480);
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.GetMemoryMap=Map,.FreePool=Free,.CreateEventEx=Create,.CloseEvent=Close};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Image,.ImageSize=IMAGE_BYTES,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};mDisplay.Alive=GlobalAlive;mDisplay.Attempted=TRUE;
 PIANO_DISPLAY_CLOCK_READ_ENV RE={.Context=&mDisplay,.Services=&Bs,.DxeServices=&Ds,.BootServicesAlive=ReadAlive,.Lease=&mDisplay.Lease};assert(PianoDisplayClockReadInitialize(&mDisplay.Reader,&RE)==EFI_SUCCESS);
 PIANO_DISPLAY_CLOCK_LEASE_ENV GE={.Context=&mDisplay.Reader,.Services=&Bs,.BootServicesAlive=LeaseAlive,.ReadCpu=PianoDisplayClockReadCpu,.ReadGcc=Gcc,.GetReadFailureEvidence=PianoDisplayClockReadFailureEvidence};assert(PianoDisplayClockLeaseAcquire(&mDisplay.Lease,&GE)==EFI_SUCCESS);
 PIANO_DISPLAY_RAIL_ENV Rail={.Context=&mDisplay,.Services=&Bs,.DxeServices=&Ds,.Alive=ReadAlive,.ClockReader=&mDisplay.Reader,.NpaHandle=(VOID*)1,.VcsHandle=(VOID*)2};assert(PianoDisplayRailInit(&mDisplay.Rail,&Rail)==EFI_SUCCESS&&mDisplay.Rail.Image[0].CodeVerified&&mDisplay.Rail.Image[1].CodeVerified);
}
static VOID RunJoint(unsigned C){
 ChildCase=C;JointPrepare();if(C==5)JointFaultPhase=1;PIANO_NON_GDSC_CLOCK_ENV Env={&mDisplay.Lease,&mDisplay.Reader,&mDisplay.Rail};EFI_STATUS E=PianoDisplayNonGdscClockAcquire(&Child,&Env);
 if(C==0||C==1||C==2||C==4){if(E!=EFI_SUCCESS)fprintf(stderr,"joint acquire status%lx before%lx after%lx static%lx MM%lx/%lx field=%s\n",(unsigned long)E,(unsigned long)Child.Report.Before,(unsigned long)Child.Report.After,(unsigned long)mDisplay.Rail.Report.StaticProducer,(unsigned long)Child.Report.RailAfter.Mm[0].Status,(unsigned long)Child.Report.RailAfter.Mm[1].Status,mDisplay.Rail.Report.LastRead.Field);
  assert(E==EFI_SUCCESS&&Child.Report.Held&&Child.Report.OwnedReferences==1&&!Child.Report.Retained&&!Child.Report.TransactionToken&&!mDisplay.Lease.BorrowToken&&ChildEnable==1&&Child.Report.OnObserved==FALSE);
  assert(Child.Report.RailBefore.Mm[0].ActiveRequest==0&&Child.Report.RailBefore.Mm[0].VcsApplied==0&&Child.Report.RailAfter.Mm[0].ActiveRequest==0x38&&Child.Report.RailAfter.Mm[0].NpaApplied==0x38&&Child.Report.RailAfter.Mm[0].VcsApplied==0x38);
  assert(Child.Report.RailAfter.Mm[0].Node==VCS_BASE+0xa488+0x38&&Child.Report.RailAfter.Mm[0].Definition==VCS_BASE+0xa488+0x78&&Child.Report.RailAfter.Mm[0].RpmhContext==VCS_BASE+0xbb08&&StaticLoads&&ImageLoads);
  E=PianoDisplayClockObserveHeld("joint-child-held",GlobalAlive,&mDisplay.Lease);CONST PIANO_DISPLAY_CLOCK_SNAPSHOT *O=&PianoDisplayClockGetReport()->Snapshot[0];assert(E==EFI_NOT_READY&&O->ClockReferenceHeld&&O->HeldBorrow==EFI_SUCCESS&&O->HeldReturn==EFI_SUCCESS&&!O->ControllerBusHeld&&!mDisplay.Lease.BorrowToken&&Child.Report.Held);
  for(UINT32 I=2;I<16;++I)assert(!O->Register[I].Attempts);
  if(C==4)Put32(VCS_BASE+0xa480,0);JointFaultPhase=1;E=PianoDisplayNonGdscClockRelease(&Child);
  if(C==0||C==1){assert(E==EFI_SUCCESS&&Child.Report.Released&&!Child.Report.Held&&!Child.Report.Retained&&!Child.Report.OwnedReferences&&!Child.Exit&&!Child.Report.TransactionToken&&!mDisplay.Lease.BorrowToken);
   assert(Child.Report.RailRetired.Mm[0].ActiveRequest==0&&Child.Report.RailRetired.Mm[0].NpaApplied==0&&Child.Report.RailRetired.Mm[0].VcsApplied==0&&ChildDisable==1&&mDisplay.Rail.Report.Count==4);
   assert(PianoDisplayRailClose(&mDisplay.Rail)==EFI_SUCCESS&&PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_SUCCESS&&PianoDisplayClockReadClose(&mDisplay.Reader)==EFI_SUCCESS&&PoolAllocations==PoolFrees&&EventCount==EventClosed&&!Handlers[0]&&!Handlers[3]);
  }else{assert(E!=EFI_SUCCESS&&Child.Report.Retained&&!Child.Report.Released&&Child.Report.TransactionToken&&mDisplay.Lease.BorrowToken);if(C==4)assert(!ChildDisable);}
 }else{assert(E!=EFI_SUCCESS&&Child.Report.Retained&&!ChildEnable);if(C==3)assert(!ChildGet);if(C==5)assert(mDisplay.Rail.Report.Retained&&m.Report.Retained);}
 if(Child.Report.Retained){UINTN Calls=ChildDisable;assert(PianoDisplayNonGdscClockRelease(&Child)==EFI_ACCESS_DENIED&&PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_ACCESS_DENIED&&ChildDisable==Calls);}
 assert(!Child.Report.RailAfter.PowerReady&&!Child.Report.RailAfter.RpmhCompletionObserved&&!mDisplay.Rail.Report.Snapshot[0].MemoryOwnershipGranted);
}
int main(int Argc,char **Argv){assert(Argc==4);FILE *F=fopen(Argv[1],"rb");assert(F);File=malloc(IMAGE_BYTES);assert(fread(File,1,IMAGE_BYTES,F)==IMAGE_BYTES&&fgetc(F)==EOF);fclose(F);for(UINT32 I=0;I<2;++I){F=fopen(Argv[I+2],"rb");assert(F);Source[I]=malloc(JointPin[I].Bytes);assert(fread(Source[I],1,JointPin[I].Bytes,F)==JointPin[I].Bytes&&fgetc(F)==EOF);fclose(F);}for(unsigned C=0;C<6;++C){pid_t P=fork();assert(P>=0);if(!P){RunJoint(C);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"nonGdsc joint case%u failed\n",C);return 1;}}puts("Actual NonGdsc+Rail+GCCLease+Reader+Guard+heldObserve joint:6 static VCS row/context/currentCount/three-pin/native-change/MM0-38-0/MXmissing/partialcleanup cases; ARM+EFI boundaries only, no device");return 0;}
