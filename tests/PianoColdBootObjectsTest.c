// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual collector and real PrePi HOB library. CPU/LDR boundary is a fixture.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include <PiPei.h>
#include <Library/PrePiLib.h>
#define PIANO_COLD_OBJECT_HOST_TEST 1
#include "../bootprofiles/early-memory/PianoColdBootObjects.c"
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
static UINTN Case,Loads;static UINT64 Counter=1000;static PIANO_COLD_BOOT_HANDOFF Handoff;static UINT8 Shim[64],Dtb[256];static UINT32 DtbBytes;
static UINTN ColdLogs,ColdObjectLogs;static BOOLEAN Services=TRUE;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return TRUE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;assert(Services);ColdLogs++;if(strstr(Format,"PIANO_COLD_OBJECT i="))ColdObjectLogs++;}
static BOOLEAN Alive(VOID){return Services;}
#include "../bootprofiles/uefi-app/PianoProductBootObjects.h"
static VOID *HobList;VOID *EFIAPI PrePeiGetHobList(VOID){return HobList;}EFI_STATUS EFIAPI PrePeiSetHobList(VOID *P){HobList=P;return EFI_SUCCESS;}
EFI_GUID *EFIAPI CopyGuid(EFI_GUID *D,CONST EFI_GUID *S){memcpy(D,S,sizeof(*D));return D;}
BOOLEAN EFIAPI CompareGuid(CONST EFI_GUID *A,CONST EFI_GUID *B){return !memcmp(A,B,sizeof(*A));}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"HOB assertion%s:%lu:%s\n",F,(unsigned long)L,D);abort();}
EFI_STATUS PianoColdHostCpu(PIANO_COLD_CPU *C){*C=(PIANO_COLD_CPU){0xa7110000,0xa764c000,4,0x30d01808,0xa7100800,0x3c0,1,0x81200000,0,++Counter,1000000000};if(Case==1)C->El=8;if(Case==2)C->Sctlr|=BIT0;if(Case==3)C->Sp=0xa8000000;if(Case==4)C->Pc=0xa8000000;if(Case==17&&Loads)C->Vbar+=2048;if(Case==18&&Loads)Counter=C->Counter=2000002000;return EFI_SUCCESS;}
UINTN EFIAPI PianoSecRead32(UINT64 A,UINT32 *V,PIANO_SEC_READ_STATE *S){Loads++;S->Address=A;const UINT8 *P=NULL;
 if(Case==16&&Loads==2){S->Faulted=1;S->Far=A;return 1;}
 if(A>=PIANO_COLD_HANDOFF_ADDRESS&&A<PIANO_COLD_HANDOFF_ADDRESS+144)P=(UINT8*)&Handoff+(A-PIANO_COLD_HANDOFF_ADDRESS);
 else if(A>=Handoff.ShimBase&&A<Handoff.ShimBase+64)P=Shim+(A-Handoff.ShimBase);
 else if(A>=Handoff.Dtb&&A<Handoff.Dtb+sizeof(Dtb))P=Dtb+(A-Handoff.Dtb);else assert(FALSE);
 memcpy(V,P,4);if(Case==15&&Loads==36)Handoff.Dtb+=4;return 0;
}
static VOID Be(UINT8 *P,UINT32 V){P[0]=V>>24;P[1]=V>>16;P[2]=V>>8;P[3]=V;}
static VOID Be64(UINT8 *P,UINT64 V){Be(P,(UINT32)(V>>32));Be(P+4,(UINT32)V);}
static VOID MakeDtb(VOID){UINT32 At=56;Be(Dtb+At,1);At+=8;Be(Dtb+At,1);At+=4;memcpy(Dtb+At,"chosen",7);At+=8;
 CONST CHAR8 Names[]="linux,initrd-start\0linux,initrd-end\0";
 Be(Dtb+At,3);Be(Dtb+At+4,8);Be(Dtb+At+8,0);Be64(Dtb+At+12,0xa8600000);At+=20;
 Be(Dtb+At,3);Be(Dtb+At+4,8);Be(Dtb+At+8,sizeof("linux,initrd-start"));Be64(Dtb+At+12,0xa8700000);At+=20;
 Be(Dtb+At,2);Be(Dtb+At+4,2);Be(Dtb+At+8,9);At+=12;memcpy(Dtb+At,Names,sizeof(Names));DtbBytes=At+sizeof(Names);
 Be(Dtb,0xd00dfeed);Be(Dtb+4,DtbBytes);Be(Dtb+8,56);Be(Dtb+12,At);Be(Dtb+16,40);Be(Dtb+20,17);Be(Dtb+24,16);Be(Dtb+32,sizeof(Names));Be(Dtb+36,At-56);
}
static VOID Run(UINTN N){Case=N;MakeDtb();Handoff=(PIANO_COLD_BOOT_HANDOFF){.Magic=PIANO_COLD_HANDOFF_MAGIC,.Dtb=0xa8500000,.EntryEl=4,.ExtensionMagic=PIANO_COLD_EXTENSION_MAGIC,.Version=1,.Bytes=144,.Counter=10,.Frequency=1000000000,.ShimBase=0xa8000000,.ShimBytes=0x400,.FdSource=0xa8000400,.FdBase=0xa7100000,.FdBytes=0x300000,.EntrySp=0xa9000000,.EntryPc=0xa8000040,.Flags=127};
 memcpy(Shim+8,&Handoff.FdBase,8);memcpy(Shim+16,&Handoff.FdBytes,8);UINT32 Magic=0x644d5241;memcpy(Shim+56,&Magic,4);
 if(N==5)Handoff.ExtensionMagic=0;if(N==6)Handoff.Version=2;if(N==7)Handoff.Reserved=1;if(N==8)Handoff.Flags|=128;
 if(N==9)Handoff.FdSource+=16;if(N==10)Handoff.EntryPc=Handoff.ShimBase+Handoff.ShimBytes;
 if(N==11){Handoff.ShimBase=0xa7ffef00;Handoff.ShimBytes=0x100;Handoff.FdSource=PIANO_COLD_HANDOFF_ADDRESS;}
 if(N==12){Handoff.ShimBase=0xa70ffc00;Handoff.FdSource=0xa7100000;Handoff.EntryPc=Handoff.ShimBase+64;Handoff.Flags&=~64ULL;}
 if(N==13)Handoff.Counter=2000;if(N==14)Shim[56]^=1;if(N==19)Dtb[0]^=1;if(N==20)Be(Dtb+20,18);
 Handoff.Crc32=PianoColdHandoffCrc(&Handoff);if(N==21)Handoff.Crc32^=1;
 EFI_STATUS E=PianoColdBootObjectsObserve();CONST PIANO_COLD_BOOT_OBJECT_REPORT *R=PianoColdBootObjectsReport();
 if(N==0||N==12||N==22||N==23||N==18||N==5){
  if(N!=18&&N!=5)assert(E==EFI_SUCCESS&&R->Coherent&&R->Epoch==10&&R->DtbBytes==DtbBytes&&R->InitrdStart==0xa8600000&&R->InitrdEnd==0xa8700000&&R->Count==13&&PianoColdBootObjectsValidate(R)==EFI_SUCCESS);
  else assert(E!=EFI_SUCCESS&&!R->Coherent&&PianoColdBootObjectsValidate(R)==EFI_NOT_READY);
  VOID *Space=mmap((VOID*)0xbd980000,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Space==(VOID*)0xbd980000);
  HobList=HobConstructor(Space,0x10000,Space,(UINT8*)Space+0x10000);
  if(N==22||N==23){EFI_HOB_HANDOFF_INFO_TABLE *P=HobList;UINT64 Before=P->EfiFreeMemoryBottom;if(N==22)P->Header.HobType=0;else P->EfiFreeMemoryTop=Before;
   assert(PianoColdBootObjectsPublishHob()!=EFI_SUCCESS&&P->EfiFreeMemoryBottom==Before&&!PianoColdBootObjectsReport()->Published);return;
  }
  assert(PianoColdBootObjectsPublishHob()==EFI_SUCCESS&&PianoColdBootObjectsReport()->Published);if(N!=18&&N!=5)assert(PianoColdBootObjectsReport()->Count==14&&PianoColdBootObjectsValidate(PianoColdBootObjectsReport())==EFI_SUCCESS);
  EFI_HOB_GUID_TYPE *Guid=(VOID*)((UINT8*)HobList+sizeof(EFI_HOB_HANDOFF_INFO_TABLE));assert(Guid->Header.HobType==EFI_HOB_TYPE_GUID_EXTENSION);PIANO_COLD_BOOT_OBJECT_REPORT *Saved=(VOID*)(Guid+1);assert(!memcmp(Saved,PianoColdBootObjectsReport(),sizeof(*Saved)));
  assert(PianoProductBootObjectsReemit(Alive)==E&&PianoProductBootObjectsStatus()==EFI_SUCCESS&&PianoProductBootObjectsSnapshot());assert(ColdLogs>=10&&ColdObjectLogs==Saved->Count);
  UINT32 Count=Saved->Count;memset(Saved,0,sizeof(*Saved));assert(PianoProductBootObjectsReemit(Alive)==E&&ColdObjectLogs==2*Count); // replay frozen cache, no second physical/HOB read
  *Saved=*PianoColdBootObjectsReport();Services=FALSE;UINTN Before=ColdLogs;assert(PianoProductBootObjectsReemit(Alive)==EFI_ABORTED&&ColdLogs==Before);Services=TRUE;
  Saved->Objects[0].Bytes=0;Saved->ReportCrc32=PianoColdObjectsCrc(Saved);assert(PianoColdBootObjectsValidate(Saved)==EFI_COMPROMISED_DATA);
  assert(PianoColdBootObjectsPublishHob()==EFI_NOT_READY);
 }else{assert(E!=EFI_SUCCESS&&R->Finished&&!R->Coherent&&PianoColdBootObjectsValidate(R)!=EFI_SUCCESS);if(N==5)assert(R->Reason==PianoColdReasonLegacyHandoff);if(N==16)assert(R->RecoveredFaults==1&&R->Reason==PianoColdReasonRead);if(N==18)assert(E==EFI_TIMEOUT&&R->Reason==PianoColdReasonBudget&&R->ElapsedUsecs>=PIANO_COLD_MAX_USECS);}
 assert(!R->MemoryOwnershipGranted&&!R->HighDdrPublished&&!R->AuthorityReady&&R->Loads==Loads);assert(PianoColdBootObjectsObserve()==EFI_ALREADY_STARTED);
}
int main(VOID){for(UINTN I=0;I<24;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"cold objects case%lu failed\n",(unsigned long)I);return 1;}}puts("Actual cold BootObjects:24 native-map/CPU/legacy/ABI/header/chosen/epoch/overlap/fault/budget/prechecked-PHIT/HOB cases; no permissions/device");return 0;}
