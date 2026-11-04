// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual cold producer/contract/parser and live EFI checker; only hardware,
// BootServices/GCD/CPU-protocol boundaries are supplied by the host fixture.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include <PiDxe.h>
#include "PianoColdMemory.h"
#include "PianoColdBootObjects.h"
#include "PianoPlatformMemoryContract.h"
#include "PianoProductMemory.h"
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#define GetMemoryMap FixtureNativeMap
#define gMemoryDescriptor FixtureNativeRows
#include "PianoColdNativeTable.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *A){return strlen(A);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(void)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(void)L;(void)F;}
static PIANO_COLD_BOOT_OBJECT_REPORT Objects;
static PIANO_EARLY_MEMORY_REPORT Early;
static struct {EFI_HOB_GUID_TYPE H;PIANO_COLD_MEMORY_REPORT R;} MemoryHob;
static struct {EFI_HOB_GUID_TYPE H;PIANO_COLD_BOOT_OBJECT_REPORT R;} ObjectsHob;
static EFI_GUID MemoryGuid=PIANO_COLD_MEMORY_HOB_GUID,ObjectsGuid=PIANO_COLD_OBJECT_HOB_GUID;
static EFI_MEMORY_DESCRIPTOR Map[256];static UINTN MapCount,Case,Layouts,ProtocolLookups;
static VOID *Tree;static UINT32 TreeBytes;
static BOOLEAN Selected,Published;
static UINT64 PageTable[512];
EFI_STATUS PianoMemoryHostCpu(UINT64 *C){C[0]=4;C[1]=5;C[2]=28ULL|(3ULL<<32);C[3]=0xa7602000;C[4]=0;C[5]=0xff00;return EFI_SUCCESS;}
EFI_STATUS PianoMemoryHostTableEntry(UINT64 Table,UINTN Index,UINT64 *Value){assert(Table==0xa7602000&&Index<512);*Value=PageTable[Index];return EFI_SUCCESS;}
static EFI_BOOT_SERVICES Boot;static EFI_SYSTEM_TABLE System;static EFI_DXE_SERVICES Dxe;
EFI_BOOT_SERVICES *gBS=&Boot;EFI_SYSTEM_TABLE *gST=&System;EFI_DXE_SERVICES *gDS=&Dxe;
static EFI_STATUS ReadTree(VOID *C,UINT32 At,UINTN N,VOID *Out){assert(At<=TreeBytes&&N<=TreeBytes-At&&N<=256);memcpy(Out,(UINT8 *)C+At,N);return EFI_SUCCESS;}
EFI_STATUS PianoColdBootObjectsMemoryLayout(PIANO_MEMORY_LAYOUT *L){
 EFI_STATUS S=PianoMemoryLayoutRead(ReadTree,Tree,TreeBytes,L);Layouts++;
 if(Case==1&&Layouts==2)L->Ram[0].Bytes-=4096;return S;
}
CONST PIANO_COLD_BOOT_OBJECT_REPORT *PianoColdBootObjectsReport(VOID){return &Objects;}
CONST PIANO_EARLY_MEMORY_REPORT *PianoEarlyMemoryReport(VOID){return &Early;}
VOID GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **M,UINT8 *N){FixtureNativeMap(M,N);}
EFI_STATUS PianoProductSelectMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR *M,UINT8 N){assert(!Selected&&M&&N);Selected=TRUE;return EFI_SUCCESS;}
VOID *EFIAPI BuildGuidHob(CONST EFI_GUID *G,UINTN N){assert(!memcmp(G,&MemoryGuid,sizeof(*G))&&N==sizeof(MemoryHob.R)&&!Published);Published=TRUE;MemoryHob.H=(EFI_HOB_GUID_TYPE){.Header={EFI_HOB_TYPE_GUID_EXTENSION,sizeof(MemoryHob),0},.Name=*G};return &MemoryHob.R;}
VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *G){if(!memcmp(G,&MemoryGuid,sizeof(*G)))return Published?&MemoryHob:NULL;if(!memcmp(G,&ObjectsGuid,sizeof(*G)))return &ObjectsHob;return NULL;}
VOID *EFIAPI GetNextGuidHob(CONST EFI_GUID *G,CONST VOID *H){(void)G;(void)H;return NULL;}
static VOID Le32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;I++)P[I]=(UINT8)(V>>(I*8));}
static VOID Be64(UINT8 *P,UINT64 V){for(UINTN I=0;I<8;I++)P[I]=(UINT8)(V>>(56-I*8));}
static VOID Le64(UINT8 *P,UINT64 V){Le32(P,(UINT32)V);Le32(P+4,(UINT32)(V>>32));}
static EFI_STATUS EFIAPI GetMap(UINTN *Bytes,EFI_MEMORY_DESCRIPTOR *Out,UINTN *Key,UINTN *Stride,UINT32 *Version){UINTN Need=MapCount*sizeof(*Map);*Stride=sizeof(*Map);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;*Key=1;if(*Bytes<Need){*Bytes=Need;return EFI_BUFFER_TOO_SMALL;}memcpy(Out,Map,Need);*Bytes=Need;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetGcd(EFI_PHYSICAL_ADDRESS At,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
 for(UINTN I=0;I<MapCount;I++)if(At>=Map[I].PhysicalStart&&At-Map[I].PhysicalStart<Map[I].NumberOfPages*4096){*D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=Map[I].PhysicalStart,.Length=Map[I].NumberOfPages*4096,.GcdMemoryType=EfiGcdMemoryTypeSystemMemory,.Attributes=Case==6?EFI_MEMORY_WT:EFI_MEMORY_WB};return EFI_SUCCESS;}return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){(void)G;(void)R;ProtocolLookups++;*Out=NULL;return EFI_NOT_FOUND;}
static VOID Object(PIANO_COLD_OBJECT_ROLE Role,UINT64 Base,UINT64 Bytes){Objects.Objects[Objects.Count++]=(PIANO_COLD_BOOT_OBJECT){Base,Bytes,Role,0};}
static VOID Setup(VOID){
 Objects=(PIANO_COLD_BOOT_OBJECT_REPORT){.Version=PIANO_COLD_OBJECT_VERSION,.Bytes=sizeof(Objects),.Status=EFI_SUCCESS,.Attempted=TRUE,.Finished=TRUE,.Coherent=TRUE,.Epoch=10,.Cpu={.Pc=0xa7110000,.Sp=0xa764c000,.El=4,.Sctlr=0x30d01808,.Vbar=0xa7100800,.SpSel=1}};
 Objects.After=Objects.Cpu;Objects.Handoff=(PIANO_COLD_BOOT_HANDOFF){.Magic=PIANO_COLD_HANDOFF_MAGIC,.Dtb=0xa8500000,.EntryEl=4,.ExtensionMagic=PIANO_COLD_EXTENSION_MAGIC,.Version=1,.Bytes=144,.Counter=10,.ShimBase=0xa8000000,.ShimBytes=0x400,.FdSource=0xa8000400,.FdBase=0xa7100000,.FdBytes=0x300000,.Flags=127,.EntryPc=0xa8000040};Objects.Handoff.Crc32=PianoColdHandoffCrc(&Objects.Handoff);
 Objects.DtbBytes=TreeBytes;memcpy(Objects.DtbHeader,Tree,40);Objects.DtbHeaderCrc32=PianoEarlyMemoryBytesCrc32(Tree,40);Objects.InitrdStart=0xa8800000;Objects.InitrdEnd=0xa8900000;
 Object(PianoColdObjectFirmware,0xa7100000,0x300000);Object(PianoColdObjectStack,0xa760d000,0x40000);Object(PianoColdObjectActiveVector,0xa7100800,2048);Object(PianoColdObjectOriginalShim,0xa8000000,0x400);Object(PianoColdObjectOriginalFd,0xa8000400,0x300000);Object(PianoColdObjectFactoryDtb,0xa8500000,TreeBytes);Object(PianoColdObjectCombinedInitrd,0xa8800000,0x100000);
 if(Case==2)Object(PianoColdObjectScheduler,0xbd980000,4096);
 Objects.ReportCrc32=PianoColdObjectsCrc(&Objects);assert(PianoColdBootObjectsValidate(&Objects)==EFI_SUCCESS);
 Early=(PIANO_EARLY_MEMORY_REPORT){.Finished=TRUE,.Status=EFI_SUCCESS,.ColdStateVerified=TRUE,.RawPayloadCoherent=TRUE,.EntryEl=Objects.Cpu.El,.EntrySctlr=Objects.Cpu.Sctlr,.EntryVbar=Objects.Cpu.Vbar,.RawPayloadBytes=168};
 UINT8 *P=Early.RawPayload;Le32(P,0x9da5e0a8);Le32(P+4,0xaf9ec4e2);Le32(P+8,3);Le32(P+16,2);
 Le64(P+24+0x10,0x80000000);Le64(P+24+0x18,0x400000000ULL);Le32(P+24+0x24,14);Le32(P+24+0x2c,1);Le64(P+24+0x40,0x40000000);
 P+=24+72;Le64(P+0x10,0xa10000000ULL);Le64(P+0x18,4096);Le32(P+0x24,14);Le32(P+0x2c,2);
 assert(PianoSmemRamParse(Early.RawPayload,Early.RawPayloadBytes,&Early.Smem)==EFI_SUCCESS);Early.Smem.RepeatedMetadataEqual=Early.Smem.RepeatedPayloadEqual=TRUE;Early.Smem.PayloadBytes=Early.RawPayloadBytes;Early.RawPayloadCrc32=PianoEarlyMemoryBytesCrc32(Early.RawPayload,Early.RawPayloadBytes);if(Case==3)Early.RawPayloadCrc32^=1;
 for(UINTN I=0;I<512;++I)PageTable[I]=((UINT64)I<<30)|BIT10|(3ULL<<8)|(1ULL<<2)|1;
 Boot.GetMemoryMap=GetMap;Boot.LocateProtocol=Locate;Dxe.GetMemorySpaceDescriptor=GetGcd;System.BootServices=&Boot;
}
static VOID Run(VOID){
 Setup();
 if(Case>=14){
  UINTN Heap=0;for(;Heap<ARRAY_SIZE(FixtureNativeRows);++Heap)if(!strcmp(FixtureNativeRows[Heap].Name,"DXE_Heap"))break;assert(Heap<ARRAY_SIZE(FixtureNativeRows));
  if(Case==14){FixtureNativeRows[Heap].Address+=4096;FixtureNativeRows[Heap].Length-=8192;}
  if(Case==15){FixtureNativeRows[Heap].Address=0x70000000;FixtureNativeRows[Heap].Length=4096;}
  if(Case==16){FixtureNativeRows[Heap].Address=0x82800000;FixtureNativeRows[Heap].Length=4096;}
 }
 EFI_STATUS S=PianoColdMemoryPrepare();if((Case<=3&&Case)||Case==15||Case==16){assert(S!=EFI_SUCCESS&&!Selected);return;}assert(S==EFI_SUCCESS&&Selected&&Layouts==2);assert(PianoColdMemoryPublishHob()==EFI_SUCCESS);
 PIANO_COLD_MEMORY_REPORT *R=&MemoryHob.R;assert(R->Status==EFI_SUCCESS&&R->Count>R->NativeCount&&R->BootEpoch==10&&R->Layout.DramBytes>0x500000000ULL);
 BOOLEAN UnknownReserved=FALSE,Expanded=FALSE;
 for(UINTN I=0;I<R->Count;++I){EFI_MEMORY_REGION_DESCRIPTOR *D=&R->Rows[I];if(D->Address<=0xa10000000ULL&&0xa10000000ULL-D->Address<D->Length){assert(D->MemoryType==EfiReservedMemoryType);UnknownReserved=TRUE;}if(D->Address<=0xd00000000ULL&&0xd00000000ULL-D->Address<D->Length&&D->MemoryType==EfiLoaderData)Expanded=TRUE;
  if(D->HobOption==NoHob||D->ResourceType==MMAP_IO)continue;UINT32 Type=D->ResourceType==SYS_MEM?(UINT32)D->MemoryType:EfiReservedMemoryType;
  Map[MapCount++]=(EFI_MEMORY_DESCRIPTOR){.Type=Type,.PhysicalStart=D->Address,.NumberOfPages=D->Length/4096,.Attribute=EFI_MEMORY_WB|((Type==EfiRuntimeServicesCode||Type==EfiRuntimeServicesData)?EFI_MEMORY_RUNTIME:0)};
 }
 assert(UnknownReserved&&Expanded);
 UINT64 HobHeap=0;for(UINTN I=0;I<R->NativeCount;++I)if(!strcmp(R->Rows[I].Name,"DXE_Heap"))HobHeap=R->Rows[I].Address;assert(HobHeap);
 Object(PianoColdObjectHobHeap,HobHeap,4096);Objects.Published=TRUE;Objects.PublishStatus=EFI_SUCCESS;Objects.ReportCrc32=PianoColdObjectsCrc(&Objects);ObjectsHob.H=(EFI_HOB_GUID_TYPE){.Header={EFI_HOB_TYPE_GUID_EXTENSION,sizeof(ObjectsHob),0},.Name=ObjectsGuid};ObjectsHob.R=Objects;
 if(Case==4){ObjectsHob.R.Epoch++;ObjectsHob.R.ReportCrc32=PianoColdObjectsCrc(&ObjectsHob.R);}
 if(Case==5)for(UINTN I=0;I<MapCount;++I)if(Map[I].PhysicalStart<=0xd00000000ULL&&0xd00000000ULL-Map[I].PhysicalStart<Map[I].NumberOfPages*4096&&Map[I].Type==EfiLoaderData){Map[I].Type=EfiConventionalMemory;break;}
 if(Case==7)PageTable[52]=0;
 if(Case==10)PageTable[52]&=~(7ULL<<2);if(Case==11)PageTable[52]+=1ULL<<30;
 PIANO_LINUX_MEMORY_PROOF Proof;S=PianoProductMemoryCheck(NULL,&Proof);
 if((Case>=4&&Case<=7)||Case==10||Case==11){assert(S!=EFI_SUCCESS&&!Proof.FullDdr);return;}
 assert(S==EFI_SUCCESS&&Proof.FullDdr&&Proof.OwnershipVerified&&Proof.NormalBytes<Proof.DramBytes&&!ProtocolLookups);
 assert(PianoProductMemoryValidate(NULL,&Proof)==EFI_SUCCESS);
 assert(PianoProductMemoryValidateDtb(Tree,TreeBytes)==EFI_SUCCESS);
 if(Case==8){UINTN I;for(I=0;I<MapCount;++I)if(Map[I].Type==EfiConventionalMemory)break;assert(I<MapCount);EFI_MEMORY_DESCRIPTOR Rest=Map[I];assert(Rest.NumberOfPages>1);Map[I].Type=EfiLoaderData;Map[I].NumberOfPages=1;Rest.PhysicalStart+=4096;Rest.NumberOfPages--;Map[MapCount++]=Rest;assert(PianoProductMemoryValidate(NULL,&Proof)==EFI_SUCCESS);}
 if(Case==12||Case==13){UINT64 Base=0;UINTN I;
  for(I=0;I<MapCount;++I)if((Case==12&&Map[I].Type==EfiConventionalMemory)||(Case==13&&Map[I].Type==EfiLoaderData&&Map[I].PhysicalStart>=0xa00000000ULL)){Base=Map[I].PhysicalStart;break;}
  assert(I<MapCount);if(Case==12)Map[I].Type=EfiLoaderData;
  INT32 Node=FdtFirstSubnode(Tree,FdtPathOffset(Tree,"/reserved-memory")),N=0;UINT8 *Reg=NULL;
  for(;Node>=0;Node=FdtNextSubnode(Tree,Node)){Reg=(UINT8 *)FdtGetProp(Tree,Node,"reg",&N);if(Reg&&N==16)break;}assert(Reg&&N==16);
  Be64(Reg,Base);Be64(Reg+8,4096);
  S=PianoProductMemoryValidateDtb(Tree,TreeBytes);assert(Case==12?S==EFI_ACCESS_DENIED:S==EFI_SUCCESS);
 }
 if(Case==9){UINT32 Struct=((UINT8 *)Tree)[8]<<24|((UINT8 *)Tree)[9]<<16|((UINT8 *)Tree)[10]<<8|((UINT8 *)Tree)[11];((UINT8 *)Tree)[Struct]^=1;assert(PianoProductMemoryValidateDtb(Tree,TreeBytes)!=EFI_SUCCESS);}
}
int main(int Argc,char **Argv){assert(Argc==2);FILE *F=fopen(Argv[1],"rb");assert(F);fseek(F,0,SEEK_END);TreeBytes=(UINT32)ftell(F);rewind(F);Tree=malloc(TreeBytes);assert(fread(Tree,1,TreeBytes,F)==TreeBytes);fclose(F);for(Case=0;Case<17;Case++){pid_t P=fork();assert(P>=0);if(!P){Run();_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"cold-memory case %lu failed\n",(unsigned long)Case);return 1;}}free(Tree);puts("Factory 24GB source vs legacy 16GB SMEM: actual cold producer, reserved unknown records, immutable HOB/epoch, real live-map/cache/permission checks and allocator changes: 17 cases passed");return 0;}
