// SPDX-License-Identifier: BSD-2-Clause-Patent
// One actual Lease + Reader + Guard pipeline. Only EFI/CPU/ARM-call boundaries
// are host fixtures. Root's GCC adapter is compiled verbatim by the Python test.
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
#include "../bootprofiles/guarded-read/PianoGuardedRead.c"
#include "../bootprofiles/uefi-app/PianoDisplayClockRead.h"
#include "ActualClockObjectComparison.h"
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiCpuArchProtocolGuid={.Data1=2},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *H){return SHA256(P,N,H)!=NULL;}
VOID EFIAPI CpuDeadLoop(VOID){abort();}
#define BASE 0xd0000000ULL
#define HEAP 0xcf000000ULL
#define IMAGE_BYTES 0x44000U
#define CLIENT_REF (HEAP+0x1000)
#define NODE (BASE+0x33a68)
#define FIXTURE_ID 0x04010033U
static UINT8 *File,*Image,*Heap;
static EFI_BOOT_SERVICES Bs;static EFI_DXE_SERVICES Ds;static EFI_CPU_ARCH_PROTOCOL Cpu;static EFI_LOADED_IMAGE_PROTOCOL Loaded;
EFI_BOOT_SERVICES *gBS=&Bs;EFI_DXE_SERVICES *gDS=&Ds;
static struct {
 PIANO_DISPLAY_CLOCK_LEASE Lease;PIANO_DISPLAY_CLOCK_READ Reader;
 BOOLEAN (*Alive)(VOID);EFI_STATUS Status;BOOLEAN Attempted,Retained,ServicesLost;
} mDisplay;
static EFI_CPU_INTERRUPT_HANDLER Handlers[4];static EFI_TPL Tpl=TPL_APPLICATION;
typedef struct {EFI_EVENT_NOTIFY Notify;VOID *Context;BOOLEAN Live,Guard;UINT32 GccSequence;} EVENT;
static EVENT Events[8192];static UINTN EventCount,EventClosed;
static BOOLEAN Services=TRUE,InGcc;static UINTN Case,GetCalls,EnableCalls,EnabledCalls,OnCalls,DisableCalls,NativeAllocated,PoolAllocations,PoolFrees,Loads,CpuLoads,GccLoads,GccCalls,BsCalls,LastLostBsCalls;
static UINT32 Ahb=0x88000002,Hf=0x08200001;static UINT64 Counter=100;
static BOOLEAN GlobalAlive(VOID){return Services;}
// Verbatim Root ReadAlive / LeaseAlive / ReadGcc, no replacement algorithm.
#include "ActualDisplayGcc.h"
static VOID Put64(UINT64 A,UINT64 V){memcpy((VOID*)(UINTN)A,&V,8);}static VOID Put16(UINT64 A,UINT16 V){memcpy((VOID*)(UINTN)A,&V,2);}static UINT16 Get16(UINT64 A){UINT16 V;memcpy(&V,(VOID*)(UINTN)A,2);return V;}
static VOID Boundary(VOID){assert(Services);BsCalls++;}
static VOID Lost(VOID){
 assert(Services);mDisplay.ServicesLost=mDisplay.Retained=TRUE;PianoDisplayClockReadFenceExit(&mDisplay.Reader);
 for(UINTN I=0;I<EventCount;++I)if(Events[I].Live)Events[I].Notify(&Events[I],Events[I].Context);
 Services=FALSE;LastLostBsCalls=BsCalls;
}
static EFI_TPL EFIAPI Raise(EFI_TPL N){Boundary();EFI_TPL T=Tpl;Tpl=N;return T;}static VOID EFIAPI Restore(EFI_TPL N){Boundary();Tpl=N;}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){Boundary();assert(P==&Cpu&&(T==0||T==3));if(H){if(Handlers[T])return EFI_ALREADY_STARTED;Handlers[T]=H;}else{assert(Handlers[T]==Exception);Handlers[T]=NULL;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **Out){Boundary();assert(!Registration);if(G==&gEfiCpuArchProtocolGuid)*Out=&Cpu;else *Out=(VOID*)(UINTN)(BASE+0x28148);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *Key,UINTN *Count,EFI_HANDLE **Out){Boundary();assert(T==ByProtocol&&G==&gEfiLoadedImageProtocolGuid&&!Key);*Count=1;*Out=malloc(sizeof(**Out));assert(*Out);(*Out)[0]=(VOID*)5;PoolAllocations++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){Boundary();assert(H==(VOID*)5&&G==&gEfiLoadedImageProtocolGuid);*Out=&Loaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *P){Boundary();assert(P);free(P);PoolFrees++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){
 Boundary();assert(T==EVT_NOTIFY_SIGNAL&&P==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid&&EventCount<ARRAY_SIZE(Events));
 EVENT *E=&Events[EventCount++];*E=(EVENT){Fn,(VOID*)C,TRUE,Fn==ExitNotify,(UINT32)GccCalls};*Out=E;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event){
 Boundary();EVENT *E=Event;assert(E>=Events&&E<Events+EventCount&&E->Live);
 if((Case==5&&E->Guard&&InGcc&&GccCalls==2)||(Case==6&&E->Guard&&!InGcc&&GetCalls&&!EnableCalls)||(Case==8&&!E->Guard&&DisableCalls))return EFI_WARN_STALE_DATA;
 E->Live=FALSE;EventClosed++;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 T,UINTN I,VOID **Out,UINTN *N){Boundary();(VOID)G;assert(T==EFI_SECTION_PE32&&!I);*N=IMAGE_BYTES;*Out=malloc(*N);assert(*Out);memcpy(*Out,File,*N);PoolAllocations++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Map(UINTN *N,EFI_MEMORY_DESCRIPTOR *M,UINTN *Key,UINTN *Stride,UINT32 *Version){
 Boundary();assert(*N>=2*sizeof(*M));*N=2*sizeof(*M);*Key=BsCalls;*Stride=sizeof(*M);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;
 M[0]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesCode,.PhysicalStart=BASE,.NumberOfPages=IMAGE_BYTES/4096,.Attribute=EFI_MEMORY_WB};
 M[1]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=HEAP,.NumberOfPages=4,.Attribute=EFI_MEMORY_WB};return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){Boundary();BOOLEAN Gcc=A==0x127000;*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=Gcc?EfiGcdMemoryTypeMemoryMappedIo:EfiGcdMemoryTypeSystemMemory,.Attributes=Gcc?EFI_MEMORY_UC:EFI_MEMORY_WB};return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *End){*First=0;*End=MAX_UINT64;return 1000000;}UINT64 EFIAPI GetPerformanceCounter(VOID){return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){*(CPU_STATE*)P=(CPU_STATE){4,1,0x480803514ULL,0xd7fff000,0,0xff44};return EFI_SUCCESS;}UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *Par){*Par=A|(A==0x127000?0:0xffULL<<56);return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN A){
 assert(Services&&m.Armed);Loads++;
 if(A==0x127004||A==0x127008){assert(InGcc);GccLoads++;return A==0x127004?Ahb:Hf;}
 assert(!InGcc&&((A>=BASE&&A<BASE+IMAGE_BYTES)||(A>=HEAP&&A<HEAP+0x4000)));CpuLoads++;
 if(Case==4&&GetCalls&&!EnableCalls&&A==CLIENT_REF){Lost();return 0;}
 UINT32 V;memcpy(&V,(VOID*)A,4);return V;
}
static EFI_STATUS Gcc(VOID *Context,PIANO_DISPLAY_CLOCK_LEASE_GCC *R){
 assert(Context==&mDisplay.Reader&&!m.Report.Active&&!Handlers[0]&&!Handlers[3]);InGcc=TRUE;GccCalls++;EFI_STATUS E=ReadGcc(Context,R);InGcc=FALSE;return E;
}
static VOID NativeBoundary(EFI_CLOCK_PROTOCOL *P){
 assert(Services&&P==(EFI_CLOCK_PROTOCOL*)(UINTN)(BASE+0x28148)&&Tpl==TPL_APPLICATION);
 assert(mDisplay.Reader.Report.TextVerified&&mDisplay.Lease.Report.Identity==EFI_SUCCESS&&mDisplay.Reader.NextText==0x28000);
 assert(!m.Report.Active&&!m.Report.SyncOwned&&!m.Report.SErrorOwned&&!m.Report.Retained&&!Handlers[0]&&!Handlers[3]);
 for(UINTN I=0;I<EventCount;++I)assert(!Events[I].Live||!Events[I].Guard);
}
EFI_STATUS PianoDisplayClockHostGet(EFI_CLOCK_PROTOCOL *P,CONST CHAR8 *Name,UINTN *Id){
 NativeBoundary(P);assert(!strcmp(Name,"gcc_disp_ahb_clk")&&*Id==MAX_UINTN);GetCalls++;
 // ARM native boundary fixture: GetID's first allocation is exactly24B. The
 // actual Reader discovers and validates that new graph, not a preaccepted span.
 if(!Get16(NODE+0x50)&&Case!=1){assert(*(UINT64*)(UINTN)(NODE+0x58)==0);memset((VOID*)(UINTN)CLIENT_REF,0,24);Put64(CLIENT_REF+8,HEAP);Put64(NODE+0x58,CLIENT_REF);NativeAllocated=24;}
 if(Case==2)Put64(HEAP+0x10,HEAP+0x2100);
 *Id=FIXTURE_ID;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostEnable(EFI_CLOCK_PROTOCOL *P,UINTN Id){
 NativeBoundary(P);assert(Id==FIXTURE_ID&&GetCalls==1&&mDisplay.Lease.Report.Baseline.MatchingSnapshots==2);EnableCalls++;
 Put16(NODE+0x50,Get16(NODE+0x50)+1);Put16(CLIENT_REF+0x10,Get16(CLIENT_REF+0x10)+1);Ahb=Case==1?0x08000003:0x88000003;
 if(Case==3)Lost();return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostIsEnabled(EFI_CLOCK_PROTOCOL *P,UINTN Id,BOOLEAN *Enabled){NativeBoundary(P);assert(Id==FIXTURE_ID&&EnableCalls==1&&*Enabled==0xa5);EnabledCalls++;*Enabled=Case==10?FALSE:(Ahb&BIT0)!=0;return EFI_SUCCESS;}
EFI_STATUS PianoDisplayClockHostIsOn(EFI_CLOCK_PROTOCOL *P,UINTN Id,BOOLEAN *On){NativeBoundary(P);assert(Id==FIXTURE_ID&&EnableCalls==1&&*On==0xa5);OnCalls++;if(Case!=11)*On=(Ahb>>28)==0||(Ahb>>28)==2;return EFI_SUCCESS;}
EFI_STATUS PianoDisplayClockHostDisable(EFI_CLOCK_PROTOCOL *P,UINTN Id){
 NativeBoundary(P);assert(Id==FIXTURE_ID&&EnableCalls==1&&!DisableCalls&&mDisplay.Lease.Report.ReleaseBefore.MatchingSnapshots==2);DisableCalls++;
 if(Case!=7){Put16(NODE+0x50,Get16(NODE+0x50)-1);Put16(CLIENT_REF+0x10,Get16(CLIENT_REF+0x10)-1);}if(!Get16(CLIENT_REF+0x10))Ahb=0x88000002;return EFI_SUCCESS;
}
static VOID Relocate(VOID){for(UINTN A=0x41000;A<IMAGE_BYTES;){UINT32 Page,N;memcpy(&Page,File+A,4);memcpy(&N,File+A+4,4);if(!N)break;assert(N>=8&&N<=IMAGE_BYTES-A);for(UINTN X=A+8;X<A+N;X+=2){UINT16 V;memcpy(&V,File+X,2);if((V>>12)==10){UINT64 Ptr;UINTN R=Page+(V&4095);assert(R<=IMAGE_BYTES-8);memcpy(&Ptr,Image+R,8);Ptr+=BASE;memcpy(Image+R,&Ptr,8);}}A+=N;}}
static VOID Run(UINTN Number){
 // Same semantic object with deliberately different representation padding.
 DCR_OBJECT A,B;memset(&A,0xa5,sizeof(A));memset(&B,0x5a,sizeof(B));
 A.Role=B.Role=PianoClockReadClientRef;A.Base=B.Base=CLIENT_REF;A.Bytes=B.Bytes=24;A.Anchor=B.Anchor=NODE+0x58;
 assert(memcmp(&A,&B,sizeof(A))&&DcrSameObject(&A,&B));
 B.Role=PianoClockReadClient;assert(!DcrSameObject(&A,&B));B.Role=A.Role;B.Base++;assert(!DcrSameObject(&A,&B));B.Base=A.Base;B.Bytes++;assert(!DcrSameObject(&A,&B));B.Bytes=A.Bytes;B.Anchor++;assert(!DcrSameObject(&A,&B));
 Case=Number;Image=mmap((VOID*)(UINTN)BASE,IMAGE_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Image==(VOID*)(UINTN)BASE);
 Heap=mmap((VOID*)(UINTN)HEAP,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Heap==(VOID*)(UINTN)HEAP);
 memcpy(Image,File,IMAGE_BYTES);Relocate();Put64(BASE+0x3fe48,BASE+0x283a0);Put64(BASE+0x3f5f0,HEAP);Put64(BASE+0x3fed8,HEAP);Put64(BASE+0x3fed0,HEAP+0x2000);
 Put64(HEAP+0x10,HEAP+0x2000);Put64(HEAP+0x2010,BASE+0x25533);Put64(HEAP+0x2018,HEAP);Put64(NODE+0x58,0);Put16(NODE+0x50,0);Put16(NODE+0x52,0);
 if(Case==1){Put64(NODE+0x58,CLIENT_REF);Put64(CLIENT_REF+8,HEAP);Put16(NODE+0x50,2);Put16(CLIENT_REF+0x10,2);Ahb=0x08000003;}
 if(Case==9)Image[0x1004]^=1;
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.GetMemoryMap=Map,.FreePool=Free,.CreateEventEx=Create,.CloseEvent=Close};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Image,.ImageSize=IMAGE_BYTES,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};
 mDisplay.Alive=GlobalAlive;mDisplay.Attempted=TRUE;
 PIANO_DISPLAY_CLOCK_READ_ENV ReaderEnv={.Context=&mDisplay,.Services=&Bs,.DxeServices=&Ds,.BootServicesAlive=ReadAlive,.Lease=&mDisplay.Lease};assert(PianoDisplayClockReadInitialize(&mDisplay.Reader,&ReaderEnv)==EFI_SUCCESS);
 PIANO_DISPLAY_CLOCK_LEASE_ENV LeaseEnv={.Context=&mDisplay.Reader,.Services=&Bs,.BootServicesAlive=LeaseAlive,.ReadCpu=PianoDisplayClockReadCpu,.ReadGcc=Gcc};
 EFI_STATUS E=PianoDisplayClockLeaseAcquire(&mDisplay.Lease,&LeaseEnv);
 if((Case==0||Case==1)&&E!=EFI_SUCCESS)fprintf(stderr,"pipeline case%lu status=%lx identity=%lx before=%lx get=%lx enable=%lx held=%u retained=%u reader=%lx readerIdentity=%lx map=%lx end=%lx nextText=%lx role=%u addr=%lx object=%lx/%lx guard=%lx/%lx counts=%lu/%lu/%lu/%lu gcc=%lu\n",(unsigned long)Case,(unsigned long)E,(unsigned long)mDisplay.Lease.Report.Identity,(unsigned long)mDisplay.Lease.Report.Before,(unsigned long)mDisplay.Lease.Report.GetId,(unsigned long)mDisplay.Lease.Report.Enable,mDisplay.Lease.Report.Held,mDisplay.Lease.Report.Retained,(unsigned long)mDisplay.Reader.Report.Status,(unsigned long)mDisplay.Reader.Report.IdentityStatus,(unsigned long)mDisplay.Reader.Report.MapStatus,(unsigned long)mDisplay.Reader.Report.EndStatus,(unsigned long)mDisplay.Reader.NextText,mDisplay.Reader.Report.Role,(unsigned long)mDisplay.Reader.Report.Address,(unsigned long)mDisplay.Reader.Report.ObjectBase,(unsigned long)mDisplay.Reader.Report.ObjectBytes,(unsigned long)mDisplay.Reader.Report.Guard.Status,(unsigned long)mDisplay.Reader.Report.Guard.CleanupStatus,(unsigned long)GetCalls,(unsigned long)EnableCalls,(unsigned long)EnabledCalls,(unsigned long)OnCalls,(unsigned long)GccCalls);
 if(Case==0||Case==1||Case==7||Case==8){
  assert(E==EFI_SUCCESS&&mDisplay.Lease.Report.Held&&!mDisplay.Lease.Report.Retained&&mDisplay.Lease.Report.OwnedReferences==1&&GetCalls==1&&EnableCalls==1&&EnabledCalls==1&&OnCalls==1&&GccCalls==2);
  assert(mDisplay.Lease.Report.Baseline.Total[0]==(Case==1?2:0)&&mDisplay.Lease.Report.Acquired.Total[0]==(Case==1?3:1)&&mDisplay.Lease.Report.Acquired.PerClient[0]==(Case==1?3:1));
  assert(mDisplay.Lease.Report.EnabledObserved==TRUE&&mDisplay.Lease.Report.OnObserved==(Case==1?TRUE:FALSE));
  assert(NativeAllocated==(Case==1?0:24)&&!mDisplay.Reader.PinnedCopy&&!mDisplay.Lease.PinnedCopy&&GccLoads==8);
  E=PianoDisplayClockLeaseRelease(&mDisplay.Lease);
  if(Case==0||Case==1){assert(E==EFI_SUCCESS&&mDisplay.Lease.Report.Released&&!mDisplay.Lease.Report.Held&&!mDisplay.Lease.Report.Retained&&!mDisplay.Lease.Exit&&mDisplay.Lease.Report.OwnedReferences==0&&DisableCalls==1&&GccCalls==3&&GccLoads==12);
   assert(Get16(NODE+0x50)==(Case==1?2:0)&&Get16(CLIENT_REF+0x10)==(Case==1?2:0));assert(*(UINT64*)(UINTN)(NODE+0x58)==CLIENT_REF); // Disable did not free/unlink.
   assert(PianoDisplayClockReadClose(&mDisplay.Reader)==EFI_SUCCESS&&!mDisplay.Reader.Report.Retained&&EventCount==EventClosed&&PoolAllocations==PoolFrees&&!Handlers[0]&&!Handlers[3]);
  }else{assert(E!=EFI_SUCCESS&&mDisplay.Lease.Report.Retained&&!mDisplay.Lease.Report.Released&&DisableCalls==1);UINTN Before=BsCalls;assert(PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_ACCESS_DENIED&&BsCalls==Before);}
 }else{
  assert(E!=EFI_SUCCESS&&mDisplay.Lease.Report.Retained&&!DisableCalls&&!mDisplay.Lease.Report.Released);
  if(Case==2||Case==4||Case==6)assert(GetCalls==1&&!EnableCalls&&!mDisplay.Lease.Report.Held);
  if(Case==3||Case==5||Case==10||Case==11)assert(GetCalls==1&&EnableCalls==1);
  if(Case==9)assert(!GetCalls&&!EnableCalls&&!GccCalls);
  if(Case==3||Case==4){assert(!Services&&mDisplay.Reader.Report.Retained&&mDisplay.Reader.Report.ServicesLost);UINTN Before=BsCalls;assert(PianoDisplayClockReadClose(&mDisplay.Reader)==EFI_ACCESS_DENIED&&PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_ACCESS_DENIED&&BsCalls==Before&&Before==LastLostBsCalls);}
  if(Case==5)assert(m.Report.Retained&&mDisplay.Lease.Report.AfterGcc.Retained&&mDisplay.Retained&&mDisplay.Lease.Report.Held);
  if(Case==6)assert(mDisplay.Reader.Report.Retained&&PianoDisplayClockReadClose(&mDisplay.Reader)==EFI_ACCESS_DENIED);
 }
 assert(!mDisplay.Reader.Report.MemoryOwnershipGranted&&!mDisplay.Reader.Report.Guard.MemoryOwnershipGranted);
 // Retained fixtures deliberately keep exact events/FV copies alive until
 // process exit, as the actual driver-lifetime contract requires.
}
int main(int Argc,char **Argv){assert(Argc==2);FILE *F=fopen(Argv[1],"rb");assert(F);File=malloc(IMAGE_BYTES);assert(fread(File,1,IMAGE_BYTES,F)==IMAGE_BYTES&&fgetc(F)==EOF);fclose(F);for(UINTN I=0;I<12;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"display clock pipeline case%lu failed\n",(unsigned long)I);return 1;}}free(File);puts("Actual ClockLease+ClockRead+Guard+verbatim RootGcc:12 full-pin/live-text/first-24B-entry/refs+1/-1/HWCG-on-FALSE/no-cross-session/native-boundary/graph/EBS/cleanup cases; host fixtures, no device");return 0;}
