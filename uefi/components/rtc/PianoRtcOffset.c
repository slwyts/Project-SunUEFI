// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoRtcOffset.h"
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_PHYSICAL_ADDRESS mPage;
STATIC UINT32 mOffset;
STATIC BOOLEAN mAttempted;
STATIC EFI_STATUS mStatus=EFI_NOT_STARTED;

STATIC EFI_STATUS ReadFactoryOffset(UINT64 *Milliseconds) {
  EFI_HANDLE *Handles=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Filesystem=NULL;
  UINTN Count=0,Matches=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if(Status!=EFI_SUCCESS)return Status;
  if(!Handles||Count>256){if(Handles)FreePool(Handles);return EFI_COMPROMISED_DATA;}
  for(UINTN I=0;I<Count;++I){
    EFI_PARTITION_INFO_PROTOCOL *Partition=NULL;
    EFI_BLOCK_IO_PROTOCOL *Block=NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Candidate=NULL;
    if(gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&Partition)!=EFI_SUCCESS||
       !Partition||Partition->Revision<EFI_PARTITION_INFO_PROTOCOL_REVISION||Partition->Type!=PARTITION_TYPE_GPT||
       StrnCmp(Partition->Info.Gpt.PartitionName,L"persist",36))continue;
    if(gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block)!=EFI_SUCCESS||
       !Block||!Block->Media||!Block->Media->MediaPresent||!Block->Media->LogicalPartition||!Block->Media->ReadOnly)continue;
    if(gBS->HandleProtocol(Handles[I],&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Candidate)!=EFI_SUCCESS||
       !Candidate||!Candidate->OpenVolume)continue;
    Filesystem=Candidate;++Matches;
  }
  FreePool(Handles);
  if(Matches!=1)return Matches?EFI_COMPROMISED_DATA:EFI_NOT_FOUND;

  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  Status=Filesystem->OpenVolume(Filesystem,&Root);
  if(Status!=EFI_SUCCESS)return Status;
  Status=Root->Open(Root,&File,L"\\time\\ats_2",EFI_FILE_MODE_READ,0);
  EFI_STATUS Closed=Root->Close(Root);
  if(Status==EFI_SUCCESS)Status=Closed;
  if(Status!=EFI_SUCCESS){if(File)File->Close(File);return Status;}

  UINT64 InfoStorage[(SIZE_OF_EFI_FILE_INFO+128+7)/8];
  UINTN Bytes=sizeof(InfoStorage);
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Bytes,InfoStorage);
  if(Status==EFI_SUCCESS){
    EFI_FILE_INFO *Info=(VOID *)InfoStorage;
    if((Info->Attribute&EFI_FILE_DIRECTORY)||Info->FileSize!=8)Status=EFI_VOLUME_CORRUPTED;
  }
  if(Status==EFI_SUCCESS){
    UINT8 Data[8];Bytes=sizeof(Data);
    Status=File->Read(File,&Bytes,Data);
    if(Status==EFI_SUCCESS&&Bytes!=sizeof(Data))Status=EFI_COMPROMISED_DATA;
    if(Status==EFI_SUCCESS){
      UINT64 Value=0;
      for(UINTN I=0;I<sizeof(Data);++I)Value|=(UINT64)Data[I]<<(I*8);
      // A zero/uninitialised or signed-negative Android offset is not UTC.
      if(Value<315964800000ULL||Value/1000>MAX_UINT32)Status=EFI_COMPROMISED_DATA;
      else *Milliseconds=Value;
    }
  }
  Closed=File->Close(File);
  return Status==EFI_SUCCESS?Closed:Status;
}

EFI_STATUS PianoRtcOffsetInitialize(VOID) {
  if(mAttempted)return mStatus;
  mAttempted=TRUE;
  UINT64 Milliseconds=0;
  mStatus=ReadFactoryOffset(&Milliseconds);
  if(mStatus==EFI_SUCCESS){
    mStatus=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,1,&mPage);
    if(mStatus==EFI_SUCCESS){
      mOffset=(UINT32)(Milliseconds/1000);
      UINT8 *Data=(VOID *)(UINTN)mPage;
      ZeroMem(Data,EFI_PAGE_SIZE);
      for(UINTN I=0;I<sizeof(mOffset);++I)Data[I]=(UINT8)(mOffset>>(I*8));
      WriteBackDataCacheRange(Data,EFI_PAGE_SIZE);
    }
  }
  DEBUG((DEBUG_WARN,"PIANO_RTC_OFFSET source=persist/time/ats_2 status=%r readonly=1 available=%u\n",mStatus,mStatus==EFI_SUCCESS));
  return mStatus;
}

STATIC INT32 Put32(VOID *Tree,INT32 Node,CONST CHAR8 *Name,UINT32 Value) {
  Value=SwapBytes32(Value);
  return FdtSetProp(Tree,Node,Name,&Value,sizeof(Value));
}

STATIC BOOLEAN TwoCells(CONST VOID *Tree,INT32 Node,CONST CHAR8 *Name) {
  INT32 Bytes=0;
  CONST UINT32 *Value=FdtGetProp(Tree,Node,Name,&Bytes);
  return Value&&Bytes==sizeof(*Value)&&Fdt32ToCpu(*Value)==2;
}

EFI_STATUS PianoRtcOffsetApplyDtb(VOID *Tree,UINTN Capacity) {
  if(!Tree||Capacity<40||Capacity>0x210000||FdtCheckHeader(Tree)||FdtTotalSize(Tree)>Capacity)return EFI_INVALID_PARAMETER;
  if(mStatus!=EFI_SUCCESS)return EFI_SUCCESS;
  if(FdtNodeOffsetByCompatible(Tree,-1,"xiaomi,piano")!=0)return EFI_SUCCESS;

  INT32 Rtc=FdtNodeOffsetByCompatible(Tree,-1,"qcom,pmk8350-rtc");
  if(Rtc<0)return Rtc==-FDT_ERR_NOTFOUND?EFI_SUCCESS:EFI_COMPROMISED_DATA;
  if(FdtNodeOffsetByCompatible(Tree,Rtc,"qcom,pmk8350-rtc")!=-FDT_ERR_NOTFOUND)return EFI_COMPROMISED_DATA;
  // Explicit writable counters and existing storage keep their own policy.
  if(FdtGetProp(Tree,Rtc,"allow-set-time",NULL)||FdtGetProp(Tree,Rtc,"nvmem-cells",NULL)||
     FdtGetProp(Tree,Rtc,"qcom,uefi-rtc-info",NULL))return EFI_SUCCESS;
  UINT32 RtcPhandle=FdtGetPhandle(Tree,Rtc);
  if(!RtcPhandle)return EFI_COMPROMISED_DATA;
  INT32 Reserved=FdtPathOffset(Tree,"/reserved-memory");
  if(Reserved<0||!TwoCells(Tree,Reserved,"#address-cells")||!TwoCells(Tree,Reserved,"#size-cells"))return EFI_COMPROMISED_DATA;
  UINT32 Phandle=0;
  if(FdtFindMaxPhandle(Tree,&Phandle)||Phandle>=MAX_UINT32-1)return EFI_COMPROMISED_DATA;
  ++Phandle;

  CHAR8 Name[48];
  AsciiSPrint(Name,sizeof(Name),"nvram@%lx",mPage);
  for(UINTN I=0;Name[I];++I)if(Name[I]>='A'&&Name[I]<='F')Name[I]+='a'-'A';
  if(FdtSubnodeOffset(Tree,Reserved,Name)!=-FDT_ERR_NOTFOUND)return EFI_COMPROMISED_DATA;
  INT32 Node=FdtAddSubnode(Tree,Reserved,Name);
  if(Node<0)return Node==-FDT_ERR_NOSPACE?EFI_BUFFER_TOO_SMALL:EFI_COMPROMISED_DATA;
  STATIC CONST CHAR8 Compatible[]="xiaomi,piano-rtc-offset\0nvmem-rmem";
  UINT64 Reg[2]={SwapBytes64(mPage),SwapBytes64(EFI_PAGE_SIZE)};
  if(FdtSetProp(Tree,Node,"compatible",Compatible,sizeof(Compatible))||
     FdtSetProp(Tree,Node,"reg",Reg,sizeof(Reg))||FdtSetProp(Tree,Node,"no-map",NULL,0)||
     FdtSetProp(Tree,Node,"read-only",NULL,0))return EFI_BUFFER_TOO_SMALL;
  INT32 Layout=FdtAddSubnode(Tree,Node,"nvmem-layout");
  if(Layout<0||FdtSetProp(Tree,Layout,"compatible","fixed-layout",13)||
     Put32(Tree,Layout,"#address-cells",1)||Put32(Tree,Layout,"#size-cells",1))return EFI_BUFFER_TOO_SMALL;
  INT32 Cell=FdtAddSubnode(Tree,Layout,"offset@0");
  if(Cell<0)return EFI_BUFFER_TOO_SMALL;
  UINT32 CellReg[2]={0,SwapBytes32(sizeof(mOffset))};
  if(FdtSetProp(Tree,Cell,"reg",CellReg,sizeof(CellReg))||Put32(Tree,Cell,"phandle",Phandle))return EFI_BUFFER_TOO_SMALL;

  // Adding nodes moves offsets. Resolve the original RTC by its phandle.
  Rtc=FdtNodeOffsetByPhandle(Tree,RtcPhandle);
  if(Rtc<0)return EFI_COMPROMISED_DATA;
  if(Put32(Tree,Rtc,"nvmem-cells",Phandle)||FdtSetProp(Tree,Rtc,"nvmem-cell-names","offset",7))return EFI_BUFFER_TOO_SMALL;
  DEBUG((DEBUG_WARN,"PIANO_RTC_DTB standard_nvmem=1 readonly=1 page=%lx offset_seconds=%u\n",mPage,mOffset));
  return EFI_SUCCESS;
}
