// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual GuardedRead + ClockRead. EFI, CPU and LDR boundaries are host fixtures;
// the FV bytes and native relocations are the captured, hash-pinned Clock PE.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/sha.h>
#undef NULL
#define PIANO_GUARDED_HOST_TEST 1
#include "../bootprofiles/guarded-read/PianoGuardedRead.c"
#include "../bootprofiles/uefi-app/PianoDisplayClockRead.h"
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiCpuArchProtocolGuid={.Data1=2},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *H){return SHA256(P,N,H)!=NULL;}
VOID EFIAPI CpuDeadLoop(VOID){abort();}
#define BASE 0xd0000000ULL
#define HEAP 0xcf000000ULL
static UINT8 *File,Image[0x44000],Heap[0x4000];
static EFI_BOOT_SERVICES Bs;static EFI_DXE_SERVICES Ds;static EFI_CPU_ARCH_PROTOCOL Cpu;static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static PIANO_DISPLAY_CLOCK_LEASE Lease;static PIANO_DISPLAY_CLOCK_READ Reader;
static EFI_CPU_INTERRUPT_HANDLER Handlers[4];static EFI_EVENT_NOTIFY ExitFn;static VOID *ExitContext;static EFI_TPL Tpl=TPL_APPLICATION;
static BOOLEAN Services=TRUE,DynamicPhase,Inject;static UINTN Case,Loads,Creates,Closes,MapCalls,NativeCalls,TargetLoads;static UINT64 Counter=100;
static VOID Put64(UINT8 *P,UINT64 V){memcpy(P,&V,8);}static VOID Put32(UINT8 *P,UINT32 V){memcpy(P,&V,4);}
static UINT8 *Pointer(UINT64 A,UINTN N){if(A>=BASE&&A-BASE<=sizeof(Image)&&N<=sizeof(Image)-(A-BASE))return Image+(UINTN)(A-BASE);if(A>=HEAP&&A-HEAP<=sizeof(Heap)&&N<=sizeof(Heap)-(A-HEAP))return Heap+(UINTN)(A-HEAP);assert(FALSE);return NULL;}
static BOOLEAN Alive(VOID *Context){assert(Context==(VOID*)1);return Services;}
#include "ActualMuMemoryMap.h"
static VOID Lost(VOID){assert(ExitFn);ExitFn((VOID*)7,ExitContext);Services=FALSE;}
static EFI_TPL EFIAPI Raise(EFI_TPL N){assert(Services);EFI_TPL T=Tpl;Tpl=N;return T;}static VOID EFIAPI Restore(EFI_TPL N){assert(Services);Tpl=N;}
static VOID EFIAPI Foreign(EFI_EXCEPTION_TYPE T,EFI_SYSTEM_CONTEXT C){(VOID)T;(VOID)C;abort();}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *P,EFI_EXCEPTION_TYPE T,EFI_CPU_INTERRUPT_HANDLER H){assert(Services&&P==&Cpu&&(T==0||T==3));if(H){if(Handlers[T])return EFI_ALREADY_STARTED;Handlers[T]=H;}else{assert(Handlers[T]==Exception);if(DynamicPhase&&((Case==38&&T==3)||(Case==39&&T==0)))return EFI_WARN_STALE_DATA;Handlers[T]=NULL;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){assert(Services&&G==&gEfiCpuArchProtocolGuid&&!R);*Out=&Cpu;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){assert(Services&&T==EVT_NOTIFY_SIGNAL&&P==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);Creates++;ExitFn=Fn;ExitContext=(VOID*)C;*Out=(VOID*)7;return DynamicPhase&&Case==40?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(Services&&E==(VOID*)7);Closes++;return DynamicPhase&&Case==18?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){assert(Services&&H==(VOID*)5&&G==&gEfiLoadedImageProtocolGuid);*Out=&Loaded;if(DynamicPhase&&Case==33&&TargetLoads)return EFI_WARN_STALE_DATA;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *P){assert(Services);free(P);return EFI_SUCCESS;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 T,UINTN I,VOID **Out,UINTN *N){(VOID)G;assert(Services&&T==EFI_SECTION_PE32&&!I);*N=sizeof(Image);*Out=malloc(*N);memcpy(*Out,File,*N);if(Case==1)((UINT8*)*Out)[0x1010]^=1;return Case==30?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Map(UINTN *N,EFI_MEMORY_DESCRIPTOR *M,UINTN *Key,UINTN *Stride,UINT32 *Version){
 assert(Services&&*N>=3*sizeof(*M));MapCalls++;*Stride=sizeof(*M);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;*Key=MapCalls;
 M[0]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesCode,.PhysicalStart=BASE,.NumberOfPages=sizeof(Image)/4096,.Attribute=EFI_MEMORY_WB};
 M[1]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=HEAP,.NumberOfPages=sizeof(Heap)/4096,.Attribute=EFI_MEMORY_WB};*N=2*sizeof(*M);
 if(Case>=60){UINT64 Cap=CoreConvertResourceDescriptorHobAttributesToCapabilities(EfiGcdMemoryTypeSystemMemory,0x703c07);assert((Cap&(EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB))==15);
  ActualMuEfiDescriptor(&M[0],BASE,sizeof(Image)/4096,EfiBootServicesCode,Cap);ActualMuEfiDescriptor(&M[1],HEAP,sizeof(Heap)/4096,EfiBootServicesData,Cap);
 }
 if(Case==63)M[0].Attribute=0;
 if(Case==64)M[0].Attribute|=EFI_MEMORY_RP;
 if(Case==65)M[0].Attribute|=EFI_MEMORY_RUNTIME;
 if(Case==66)M[0].VirtualStart=BASE+4096;
 if(Case==67)M[0].Type=EfiMemoryMappedIO;
 if(Case==68)return EFI_WARN_STALE_DATA;
 if(DynamicPhase){if(Case==9)M[1].Type=EfiConventionalMemory;if(Case==10)M[1].NumberOfPages=1;if(Case==11)M[1].Attribute=EFI_MEMORY_UC;if(Case==28){M[2]=M[1];*N=3*sizeof(*M);}if(Case==37)*Stride=sizeof(*M)-1;if(Case==4&&TargetLoads)Loaded.ImageSize--;}
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){assert(Services);*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A,.Length=4096,.GcdMemoryType=EfiGcdMemoryTypeSystemMemory,.Attributes=EFI_MEMORY_WB};if(DynamicPhase&&Case==12)D->Attributes=EFI_MEMORY_UC;if(DynamicPhase&&Case==61)D->Attributes=EFI_MEMORY_WC;return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *End){*First=0;*End=MAX_UINT64;return 1000000;}UINT64 EFIAPI GetPerformanceCounter(VOID){return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *P){*(CPU_STATE*)P=(CPU_STATE){4,1,0x480803514ULL,0xd7fff000,0,0xff44};return EFI_SUCCESS;}UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *P){*P=A|(0xffULL<<56);if(DynamicPhase&&Case==13)*P+=4096;if(DynamicPhase&&(Case==14||Case==62))*P^=0xbbULL<<56;return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN A){
 Loads++;if(DynamicPhase&&A>=HEAP+0x1000&&A<HEAP+0x1018)TargetLoads++;
 if(DynamicPhase&&Case==20){Lost();return 0;}
 if(DynamicPhase&&Case==19){EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=0x1000,.FAR=A,.ESR=0x96000010,.SPSR=5};EFI_SYSTEM_CONTEXT S={.SystemContextAArch64=&C};Handlers[0](0,S);assert(C.ELR==0x1004);return 0;}
 UINT32 V;memcpy(&V,Pointer(A,4),4);
 if(DynamicPhase&&Case==16&&A==HEAP+0x1010&&++Inject){V^=1;Inject=FALSE;Case=160;}
 if(DynamicPhase&&Case==15&&TargetLoads)Put64(Image+0x33ac0,HEAP+0x2000);
 if(DynamicPhase&&Case==36&&A==BASE+0x3fe48&&++Inject){Put64(Image+0x3fe48,BASE+0x283a4);Inject=FALSE;}
 return V;
}
static VOID Relocate(VOID){for(UINTN A=0x41000;A<sizeof(Image);){UINT32 Page,N;memcpy(&Page,File+A,4);memcpy(&N,File+A+4,4);if(!N)break;assert(N>=8&&N<=sizeof(Image)-A);for(UINTN X=A+8;X<A+N;X+=2){UINT16 V;memcpy(&V,File+X,2);if((V>>12)==10){UINT64 P;UINTN R=Page+(V&4095);assert(R<=sizeof(Image)-8);memcpy(&P,Image+R,8);P+=BASE;memcpy(Image+R,&P,8);}}A+=N;}}
static EFI_STATUS Read(UINT64 A,UINTN N,VOID *Out){return PianoDisplayClockReadCpu(&Reader,A,N,Out);}
EFI_STATUS __wrap_PianoGuardedReadBegin(CONST PIANO_GUARDED_CONFIG *C,VOID **T){EFI_STATUS S=PianoGuardedReadBegin(C,T);if(DynamicPhase&&Case==41&&S==EFI_SUCCESS)*T=NULL;return S;}
EFI_STATUS __wrap_PianoGuardedReadEnd(VOID *T){EFI_STATUS S=PianoGuardedReadEnd(T);return DynamicPhase&&Case==42&&S==EFI_SUCCESS?EFI_WARN_STALE_DATA:S;}
static VOID VerifyText(VOID){for(UINTN A=0x1000;A<0x28000;A+=256){UINT8 Out[256];assert(Read(BASE+A,sizeof(Out),Out)==EFI_SUCCESS);}assert(Reader.Report.TextVerified&&!Reader.PinnedCopy&&!Handlers[0]&&!Handlers[3]&&Creates==Closes);}
static VOID Run(UINTN C){
 Case=C;memcpy(Image,File,sizeof(Image));Relocate();
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.HandleProtocol=Handle,.GetMemoryMap=Map,.FreePool=Free,.CreateEventEx=Create,.CloseEvent=Close};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=(VOID*)(UINTN)BASE,.ImageSize=sizeof(Image),.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};
 Lease.NativeImage=(VOID*)5;Lease.ImageIdentity=&Loaded;Lease.ImageBase=Loaded.ImageBase;Lease.ImageSize=sizeof(Image);Lease.Report.NativeBase=BASE;Lease.Report.ClockId=0x04010033;
 Put64(Image+0x3fe48,BASE+0x283a0);Put64(Image+0x3f5f0,HEAP);Put64(Image+0x3fed8,HEAP);Put64(Image+0x3fed0,HEAP+0x2000);
 Put64(Heap+0x10,HEAP+0x2000);Put64(Heap+0x2010,BASE+0x25533);Put64(Heap+0x2018,HEAP);
 Put64(Image+0x33ac0,HEAP+0x1000);Put64(Heap+0x1008,HEAP);Put32(Heap+0x1010,0x00020003);Heap[0x19]=4;
 PIANO_DISPLAY_CLOCK_READ_ENV E={(VOID*)1,&Bs,&Ds,Alive,&Lease};UINT8 Out[256];memset(Out,0xa5,sizeof(Out));
 if(C==21){PianoDisplayClockReadFenceExit(&Reader);assert(PianoDisplayClockReadInitialize(&Reader,&E)==EFI_NOT_READY&&!Creates&&!Loads);return;}
 EFI_STATUS S=PianoDisplayClockReadInitialize(&Reader,&E);if(C==1||C==30){assert(S!=EFI_SUCCESS&&!Reader.PinnedCopy&&!Loads);return;}assert(S==EFI_SUCCESS);
 assert(Reader.Report.EfiMap.Status==EFI_NOT_STARTED&&Reader.Report.EfiMap.GetMapStatus==EFI_NOT_STARTED);
 if(C>=63&&C<=68){S=Read(BASE+0x1000,256,Out);assert(S!=EFI_SUCCESS&&Out[0]==0xa5&&!Reader.Report.Sessions&&!Loads);
  assert(Reader.Report.EfiMap.Status==S&&Reader.Report.EfiMap.DescriptorBytes==sizeof(EFI_MEMORY_DESCRIPTOR));
  PIANO_DISPLAY_CLOCK_EFI_MAP_REASON R=C==63?PianoClockEfiMapCache:C==64?PianoClockEfiMapReadProtected:C==65?PianoClockEfiMapRuntime:C==66?PianoClockEfiMapNonIdentityVirtual:C==67?PianoClockEfiMapWrongType:PianoClockEfiMapGetMap;
  assert(Reader.Report.EfiMap.Reason==R);if(C!=68)assert(Reader.Report.EfiMap.DescriptorBase==BASE&&Reader.Report.EfiMap.DescriptorPages==sizeof(Image)/4096&&Reader.Report.EfiMap.Cursor==BASE+0x1000);
  if(C==63){PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE F={0};assert(PianoDisplayClockReadFailureEvidence(&Reader,BASE+0x1000,256,S,&F)==EFI_SUCCESS&&F.Sequence==1&&F.MapStatus==EFI_NOT_READY&&!F.Sessions&&!F.GuardReads);assert(PianoDisplayClockReadFailureEvidence(&Reader,BASE+0x1004,256,S,&F)==EFI_NOT_READY);assert(Read(BASE+0x1000,256,Out)==EFI_NOT_READY);assert(PianoDisplayClockReadFailureEvidence(&Reader,BASE+0x1000,256,S,&F)==EFI_NOT_READY);}
  goto End;
 }
 if(C==3){Loaded.ImageSize--;assert(Read(BASE+0x1000,256,Out)!=EFI_SUCCESS&&!Loads);goto End;}
 if(C==5){Reader.Report.TextVerified=TRUE;assert(Read(BASE+0x3fe48,8,Out)==EFI_NOT_READY&&!Loads);goto End;}
 if(C==31){assert(Read(BASE+0x1100,256,Out)==EFI_ACCESS_DENIED&&!Loads);goto End;}
 if(C==2){Image[0x1004]^=1;assert(Read(BASE+0x1000,256,Out)==EFI_COMPROMISED_DATA&&Out[0]==0xa5);goto End;}
 VerifyText();DynamicPhase=TRUE;
 if(C==6)S=Read(HEAP+0x3000,4,Out);else if(C==7)S=Read(0x127004,4,Out);else if(C==8)S=Read(0xa00000000,4,Out);
 else if(C==22)S=Read(HEAP+0x18,1,Out);else if(C==24){Put64(Image+0x3fe48,MAX_UINT64-16);S=Read(HEAP+0x1010,4,Out);}
 else if(C==25){Put32(Image+0x283a8,257);S=Read(HEAP+0x1010,4,Out);}
 else if(C==26){Put64(Image+0x286b0,BASE+0x32420);S=Read(HEAP+0x1010,4,Out);}
 else if(C==29){Put64(Image+0x3fe48,HEAP+0x2000);S=Read(HEAP+0x1010,4,Out);}
 else if(C==32){assert(Read(BASE+0x3fe48,8,&Lease.ImageBase)==EFI_INVALID_PARAMETER);S=EFI_ACCESS_DENIED;}
 else if(C==34)S=Read(BASE+0x41000,4,Out);else if(C==35){Put64(Image+0x3f5f0,0);S=Read(HEAP+0x1010,4,Out);}
 else if(C==23||C==27){for(UINTN I=0;I<(C==23?1:64);++I){Put64(Heap+0x1000+I*24,C==23?HEAP+0x1000:HEAP+0x1000+(I+1)*24);Put64(Heap+0x1008+I*24,HEAP+0x100);}S=Read(HEAP+0x3000,4,Out);}
 else if(C==43){Put64(Image+0x3fed8,HEAP+0x100);S=Read(HEAP+0x1010,4,Out);}
 else if(C==44){Put32(Heap+0x2008,1);S=Read(HEAP+0x1010,4,Out);}
 else if(C==45){Put64(Heap+0x2010,BASE+0x25534);S=Read(HEAP+0x1010,4,Out);}
 else if(C==46){Put64(Heap+0x10,HEAP+0x2100);S=Read(HEAP+0x1010,4,Out);}
 else if(C==47){Put64(Heap+0x2018,0);S=Read(HEAP+0x1010,4,Out);}
 else if(C==48){Put64(Heap+0x2018,HEAP+0x3000);Put64(Heap+0x3000,HEAP+0x3000);Put64(Heap+0x3010,HEAP+0x2000);S=Read(HEAP+0x1010,4,Out);}
 else if(C==49){Put32(Heap+0x2008,1);Put64(Heap+0x2000,HEAP+0x2000);S=Read(HEAP+0x1010,4,Out);}
 else if(C==50){for(UINTN I=0;I<64;++I){Put64(Heap+0x2000+I*32,HEAP+0x2000+(I+1)*32);Put32(Heap+0x2008+I*32,1);Put64(Heap+0x2010+I*32,BASE+0x25533);}S=Read(HEAP+0x1010,4,Out);}
 else if(C==51){Put64(Heap+0x2018,HEAP+0x3000);for(UINTN I=0;I<64;++I){Put64(Heap+0x3000+I*32,HEAP+0x3000+(I+1)*32);Put64(Heap+0x3010+I*32,HEAP+0x2000);}S=Read(HEAP+0x1010,4,Out);}
 else if(C==52)S=Read(HEAP+0x19,2,Out);
 else if(C==53)S=Read(HEAP+0x2018,8,Out);
 else if(C==54){Put64(Image+0x33a70,HEAP+0x3000);S=Read(HEAP+0x1010,4,Out);}
 else if(C==55){assert(Read(BASE+0x3fe48,8,(VOID*)(UINTN)(BASE+0x29000))==EFI_INVALID_PARAMETER);S=EFI_ACCESS_DENIED;}
 else if(C==56){Put64(Image+0x3fe48,BASE+0x283a0);Put64(Image+0x3f5f0,BASE+0x39000);Put64(Image+0x3fed8,BASE+0x39000);S=Read(HEAP+0x1010,4,Out);}
 else if(C==57){Put64(Image+0x33a68,BASE+0x14e06);S=Read(HEAP+0x1010,4,Out);}
 else if(C==58)S=Read(HEAP+0x1010,257,Out);
 else if(C==59)S=Read(MAX_UINT64-1,4,Out);
 else{if(C==17)Handlers[0]=Foreign;S=Read(HEAP+0x1010,4,Out);}
 if(C==0||C==60){assert(S==EFI_SUCCESS&&Out[0]==3&&Out[1]==0&&Out[2]==2&&Reader.Report.Role==PianoClockReadClientRef&&Reader.Report.ObjectBytes==24&&Reader.Report.ClientNodes==1);assert(Read(HEAP+0x19,1,Out)==EFI_SUCCESS&&Out[0]==4);assert(Read(BASE+0x3fe48,8,Lease.LiveText)==EFI_SUCCESS);if(C==60)assert(Reader.Report.EfiMap.DescriptorAttributes==(CoreConvertResourceDescriptorHobAttributesToCapabilities(EfiGcdMemoryTypeSystemMemory,0x703c07)&~(EFI_MEMORY_ACCESS_MASK|EFI_MEMORY_RUNTIME))&&Reader.Report.Guard.LastGcdAttributes==EFI_MEMORY_WB&&(Reader.Report.Guard.LastPar>>56)==0xff);}
 else{if(S==EFI_SUCCESS){fprintf(stderr,"unexpected successful case%lu\n",(unsigned long)C);abort();}assert(Out[0]==0xa5);}
 assert(!NativeCalls&&!Reader.Report.MemoryOwnershipGranted&&!Reader.Report.Guard.MemoryOwnershipGranted);
 if(C==9)assert(Reader.Report.EfiMap.Reason==PianoClockEfiMapWrongType&&Reader.Report.EfiMap.DescriptorType==EfiConventionalMemory);
 if(C==11)assert(Reader.Report.EfiMap.Reason==PianoClockEfiMapCache&&Reader.Report.EfiMap.DescriptorAttributes==EFI_MEMORY_UC);
 if(C==28)assert(Reader.Report.EfiMap.Reason==PianoClockEfiMapOverlap&&Reader.Report.EfiMap.ConflictIndex==1);
 if(C==37)assert(Reader.Report.EfiMap.Reason==PianoClockEfiMapFormat);
 if(C==61||C==62)assert(Reader.Report.EfiMap.Reason==PianoClockEfiMapReady&&(Reader.Report.EfiMap.DescriptorAttributes&15)==15&&Reader.Report.Guard.MappingStatus==EFI_NOT_READY);
 if(C==18||C==20||(C>=38&&C<=42)){assert(Reader.Report.Retained);UINTN Before=Loads;assert(Read(HEAP+0x1010,4,Out)==EFI_ACCESS_DENIED&&Loads==Before);assert(PianoDisplayClockReadClose(&Reader)==EFI_ACCESS_DENIED);return;}
 if(C==17)Handlers[0]=NULL;assert(!Handlers[0]&&!Handlers[3]&&Creates==Closes);
End:
 assert(PianoDisplayClockReadClose(&Reader)==EFI_SUCCESS&&!Reader.PinnedCopy);
}
int main(int Argc,char **Argv){assert(Argc==2);FILE *F=fopen(Argv[1],"rb");assert(F);File=malloc(sizeof(Image));assert(fread(File,1,sizeof(Image),F)==sizeof(Image)&&fgetc(F)==EOF);fclose(F);for(UINTN I=0;I<69;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"clock read case%lu failed\n",(unsigned long)I);return 1;}}free(File);puts("Actual ClockRead+GuardedRead:69 pinnedPE/text/actualMu capabilities-map/currentGCD-PAR/typedgraph/fresh-failure-evidence/EBS/cleanup cases; host fixtures, no native or hardware calls");return 0;}
