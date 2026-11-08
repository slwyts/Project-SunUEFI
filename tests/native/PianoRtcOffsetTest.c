// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../uefi/components/rtc/PianoRtcOffset.c"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef FDT_TAGSIZE
#include <libfdt.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiSimpleFileSystemProtocolGuid={.Data1=1};
EFI_GUID gEfiPartitionInfoProtocolGuid={.Data1=2};
EFI_GUID gEfiBlockIoProtocolGuid={.Data1=3};
EFI_GUID gEfiFileInfoGuid={.Data1=4};
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Filesystem;
STATIC EFI_PARTITION_INFO_PROTOCOL Partition;
STATIC EFI_BLOCK_IO_PROTOCOL Block;
STATIC EFI_BLOCK_IO_MEDIA Media;
STATIC EFI_FILE_PROTOCOL Root,File;
STATIC UINT8 Page[EFI_PAGE_SIZE] __attribute__((aligned(EFI_PAGE_SIZE)));
STATIC UINT8 Tree[8192] __attribute__((aligned(8)));
STATIC UINT64 Milliseconds=1774688487120ULL;
STATIC UINT64 FileBytes=8;
STATIC UINTN RootClosed,FileClosed,Reads,Allocations;
STATIC EFI_STATUS OpenStatus=EFI_SUCCESS;

VOID *EFIAPI CopyMem(VOID *To,CONST VOID *From,UINTN Bytes){return memcpy(To,From,Bytes);}
VOID *EFIAPI ZeroMem(VOID *To,UINTN Bytes){return memset(To,0,Bytes);}
INTN EFIAPI StrnCmp(CONST CHAR16 *A,CONST CHAR16 *B,UINTN Count){
  while(Count--){if(*A!=*B)return (INTN)*A-*B;if(!*A)return 0;++A;++B;}return 0;
}
VOID EFIAPI FreePool(VOID *Buffer){free(Buffer);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Buffer,UINTN Bytes){assert(Buffer==Page&&Bytes==sizeof(Page));return Buffer;}

STATIC EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles){
  assert(Type==ByProtocol&&Guid==&gEfiSimpleFileSystemProtocolGuid&&!Key);
  *Count=1;*Handles=malloc(sizeof(**Handles));assert(*Handles);**Handles=&Filesystem;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Interface){
  assert(Handle==&Filesystem);
  if(Guid==&gEfiPartitionInfoProtocolGuid)*Interface=&Partition;
  else if(Guid==&gEfiBlockIoProtocolGuid)*Interface=&Block;
  else if(Guid==&gEfiSimpleFileSystemProtocolGuid)*Interface=&Filesystem;
  else abort();
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This){
  if(This==&Root)++RootClosed;else{assert(This==&File);++FileClosed;}return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **Out,CHAR16 *Name,UINT64 Mode,UINT64 Attr){
  assert(This==&Root&&!StrnCmp(Name,L"\\time\\ats_2",12)&&Mode==EFI_FILE_MODE_READ&&!Attr);
  if(OpenStatus==EFI_SUCCESS)*Out=&File;return OpenStatus;
}
STATIC EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Out){
  assert(This==&Filesystem);*Out=&Root;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN *Bytes,VOID *Buffer){
  assert(This==&File&&Guid==&gEfiFileInfoGuid&&*Bytes>=SIZE_OF_EFI_FILE_INFO);
  EFI_FILE_INFO *Info=Buffer;ZeroMem(Info,*Bytes);Info->FileSize=FileBytes;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This,UINTN *Bytes,VOID *Buffer){
  assert(This==&File&&*Bytes==8);++Reads;
  for(UINTN I=0;I<8;++I)((UINT8 *)Buffer)[I]=(UINT8)(Milliseconds>>(I*8));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Pages(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE Memory,UINTN Count,EFI_PHYSICAL_ADDRESS *Address){
  assert(Type==AllocateAnyPages&&Memory==EfiReservedMemoryType&&Count==1);
  ++Allocations;*Address=(UINTN)Page;return EFI_SUCCESS;
}
STATIC VOID MakeTree(VOID){
  assert(!fdt_create_empty_tree(Tree,sizeof(Tree)));
  assert(!fdt_setprop_string(Tree,0,"compatible","xiaomi,piano"));
  int Node=fdt_add_subnode(Tree,0,"reserved-memory");assert(Node>=0);
  assert(!fdt_setprop_u32(Tree,Node,"#address-cells",2));
  assert(!fdt_setprop_u32(Tree,Node,"#size-cells",2));
  assert(!fdt_setprop(Tree,Node,"ranges",NULL,0));
  Node=fdt_add_subnode(Tree,0,"rtc");assert(Node>=0);
  assert(!fdt_setprop_string(Tree,Node,"compatible","qcom,pmk8350-rtc"));
  assert(!fdt_setprop_u32(Tree,Node,"phandle",42));
}

int main(int Argc,char **Argv){
  assert(Argc==2);int Case=atoi(Argv[1]);
  gBS=&Services;Services.LocateHandleBuffer=Locate;Services.HandleProtocol=Protocol;Services.AllocatePages=Pages;
  Filesystem.OpenVolume=Volume;Root.Open=Open;Root.Close=File.Close=Close;File.GetInfo=Info;File.Read=Read;
  Partition.Revision=EFI_PARTITION_INFO_PROTOCOL_REVISION;Partition.Type=PARTITION_TYPE_GPT;
  memcpy(Partition.Info.Gpt.PartitionName,L"persist",sizeof(L"persist"));
  Block.Media=&Media;Media.MediaPresent=Media.LogicalPartition=Media.ReadOnly=TRUE;
  memset(Page,0xa5,sizeof(Page));MakeTree();
  if(Case==1)FileBytes=9;
  if(Case==2)Milliseconds=0;
  if(Case==3)Milliseconds=MAX_UINT64;
  if(Case==4)OpenStatus=EFI_NOT_FOUND;
  if(Case==5)Media.ReadOnly=FALSE;
  EFI_STATUS Status=PianoRtcOffsetInitialize();
  assert(PianoRtcOffsetInitialize()==Status);
  if(Case>=1&&Case<=5){
    assert(Status!=EFI_SUCCESS&&!Allocations);
    assert(RootClosed==(Case==5?0:1)&&FileClosed==(Case>=4?0:1));
    assert(PianoRtcOffsetApplyDtb(Tree,sizeof(Tree))==EFI_SUCCESS);
    assert(!fdt_getprop(Tree,fdt_path_offset(Tree,"/rtc"),"nvmem-cells",NULL));
    return 0;
  }
  assert(Status==EFI_SUCCESS&&Reads==1&&RootClosed==1&&FileClosed==1&&Allocations==1);
  assert(mOffset==1774688487&&Page[0]==0xe7);
  for(UINTN I=4;I<sizeof(Page);++I)assert(!Page[I]);
  int Rtc=fdt_path_offset(Tree,"/rtc");
  if(Case==6)assert(!fdt_setprop(Tree,Rtc,"allow-set-time",NULL,0));
  if(Case==7)assert(!fdt_setprop_u32(Tree,Rtc,"nvmem-cells",42));
  assert(PianoRtcOffsetApplyDtb(Tree,sizeof(Tree))==EFI_SUCCESS);
  Rtc=fdt_path_offset(Tree,"/rtc");int Bytes;
  const fdt32_t *Reference=fdt_getprop(Tree,Rtc,"nvmem-cells",&Bytes);
  if(Case==6){assert(!Reference);return 0;}
  if(Case==7){assert(Reference&&fdt32_to_cpu(*Reference)==42);return 0;}
  assert(Reference&&Bytes==4&&fdt32_to_cpu(*Reference)>42);
  int Cell=fdt_node_offset_by_phandle(Tree,fdt32_to_cpu(*Reference));assert(Cell>=0);
  assert(!strcmp(fdt_get_name(Tree,Cell,NULL),"offset@0"));
  int Layout=fdt_parent_offset(Tree,Cell);assert(Layout>=0);
  assert(!fdt_node_check_compatible(Tree,Layout,"fixed-layout"));
  int Provider=fdt_parent_offset(Tree,Layout);assert(Provider>=0);
  assert(!fdt_node_check_compatible(Tree,Provider,"nvmem-rmem"));
  assert(fdt_getprop(Tree,Provider,"no-map",NULL)&&fdt_getprop(Tree,Provider,"read-only",NULL));
  const fdt64_t *Reg=fdt_getprop(Tree,Provider,"reg",&Bytes);
  assert(Reg&&Bytes==16&&fdt64_ld(Reg)==(UINTN)Page&&fdt64_ld(Reg+1)==sizeof(Page));
  assert(PianoRtcOffsetApplyDtb(Tree,sizeof(Tree))==EFI_SUCCESS);
  assert(Allocations==1&&Reads==1);
  return 0;
}
