// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoBluetoothAddress.h"
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/EFIChipInfo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC UINT8 mAddress[6];
STATIC BOOLEAN mAttempted,mAvailable;
STATIC CONST CHAR8 *mSource="unavailable";
STATIC EFI_GUID mChipInfoGuid=EFI_CHIPINFO_PROTOCOL_GUID;

STATIC BOOLEAN ValidAddress(CONST UINT8 *Address,BOOLEAN Public){
  // QTI NVM placeholders in the MSB-first order used by both callers.
  STATIC CONST UINT8 Defaults[][6]={
    {0x00,0xbd,0x00,0x00,0xbd,0x00},
    {0x66,0x55,0x44,0x33,0x22,0x11}
  };
  UINT8 Any=0,NotFf=0;
  for(UINTN I=0;I<6;++I){Any|=Address[I];NotFf|=Address[I]^0xff;}
  if(!Any||!NotFf||(Address[0]&(Public?3:1)))return FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(Defaults);++I)
    if(!CompareMem(Address,Defaults[I],sizeof(Defaults[I])))return FALSE;
  return TRUE;
}

STATIC EFI_STATUS FactoryAddress(UINT8 Address[6]){
  EFI_HANDLE *Handles=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Filesystem=NULL;
  UINTN Count=0,Matches=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status))return Status;
  if(!Handles||Count>256){if(Handles)FreePool(Handles);return EFI_COMPROMISED_DATA;}
  for(UINTN I=0;I<Count;++I){
    EFI_PARTITION_INFO_PROTOCOL *Partition=NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Candidate=NULL;
    EFI_BLOCK_IO_PROTOCOL *Block=NULL;
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
  if(EFI_ERROR(Status))return Status;
  Status=Root->Open(Root,&File,L"\\bluetooth\\.bt_nv.bin",EFI_FILE_MODE_READ,0);
  EFI_STATUS CloseStatus=Root->Close(Root);
  if(Status==EFI_SUCCESS)Status=CloseStatus;
  if(EFI_ERROR(Status)){if(File)File->Close(File);return Status;}

  UINT64 InfoStorage[(SIZE_OF_EFI_FILE_INFO+256+7)/8];
  UINTN Bytes=sizeof(InfoStorage);
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Bytes,InfoStorage);
  if(Status==EFI_SUCCESS){
    EFI_FILE_INFO *Info=(VOID *)InfoStorage;
    if((Info->Attribute&EFI_FILE_DIRECTORY)||Info->FileSize!=6)Status=EFI_VOLUME_CORRUPTED;
  }
  if(Status==EFI_SUCCESS){
    Bytes=6;
    Status=File->Read(File,&Bytes,Address);
    if(Status==EFI_SUCCESS&&(Bytes!=6||!ValidAddress(Address,TRUE)))Status=EFI_COMPROMISED_DATA;
  }
  CloseStatus=File->Close(File);
  return Status==EFI_SUCCESS?CloseStatus:Status;
}

STATIC EFI_STATUS ChipAddress(UINT8 Address[6]){
  EFI_CHIPINFO_PROTOCOL *ChipInfo=NULL;
  EFIChipInfoSerialNumType Serial=0;
  EFIChipInfoIdType Chip=0;
  EFI_STATUS Status=gBS->LocateProtocol(&mChipInfoGuid,NULL,(VOID **)&ChipInfo);
  if(Status!=EFI_SUCCESS)return Status;
  if(!ChipInfo||ChipInfo->Revision<EFI_CHIPINFO_PROTOCOL_REVISION_3||
     !ChipInfo->GetSerialNumber||!ChipInfo->GetChipId)return EFI_UNSUPPORTED;
  Status=ChipInfo->GetSerialNumber(ChipInfo,&Serial);
  if(Status==EFI_SUCCESS)Status=ChipInfo->GetChipId(ChipInfo,&Chip);
  if(Status!=EFI_SUCCESS)return Status;
  if(!Serial||Serial==MAX_UINT32||!Chip||(UINT32)Chip==MAX_UINT32)return EFI_NOT_FOUND;

  // Preserve the existing SoC-derived fallback identity across rootfs changes.
  STATIC CONST CHAR8 Domain[]="SunUEFI piano Bluetooth address v1";
  UINT8 Input[sizeof(Domain)+8],Digest[32];
  CopyMem(Input,Domain,sizeof(Domain));
  for(UINTN I=0;I<4;++I){Input[sizeof(Domain)+I]=(UINT8)((UINT32)Chip>>(I*8));Input[sizeof(Domain)+4+I]=(UINT8)(Serial>>(I*8));}
  if(!Sha256HashAll(Input,sizeof(Input),Digest))return EFI_DEVICE_ERROR;
  CopyMem(Address,Digest,6);Address[0]=(Address[0]&0xfc)|2;
  return EFI_SUCCESS;
}

EFI_STATUS PianoBluetoothAddressInitialize(VOID){
  if(mAttempted)return mAvailable?EFI_SUCCESS:EFI_NOT_FOUND;
  mAttempted=TRUE;
  UINT8 Address[6];
  EFI_STATUS Factory=FactoryAddress(Address),Status=Factory;
  if(Status==EFI_SUCCESS)mSource="persist";
  else{Status=ChipAddress(Address);if(Status==EFI_SUCCESS)mSource="soc";}
  if(Status==EFI_SUCCESS){
    // Both source formats are MSB-first; the Bluetooth DT binding is LSB-first.
    for(UINTN I=0;I<6;++I)mAddress[I]=Address[5-I];
    mAvailable=TRUE;
  }
  DEBUG((DEBUG_WARN,"PIANO_BLUETOOTH_ADDRESS source=%a status=%r factory=%r readonly=1\n",mSource,Status,Factory));
  return Status;
}

EFI_STATUS PianoBluetoothAddressApplyDtb(VOID *Tree,UINTN Capacity){
  if(!Tree||Capacity<40||Capacity>0x210000||FdtCheckHeader(Tree)||FdtTotalSize(Tree)>Capacity)return EFI_INVALID_PARAMETER;
  if(!mAvailable)return EFI_SUCCESS;
  STATIC CONST CHAR8 *Compatibles[]={"qcom,wcn7850-bt","qcom,wcn7861-bt"};
  INT32 Node=-1;
  for(UINTN I=0;I<ARRAY_SIZE(Compatibles);++I){
    INT32 At=-1;
    while((At=FdtNodeOffsetByCompatible(Tree,At,Compatibles[I]))>=0){
      if(Node>=0&&Node!=At)return EFI_COMPROMISED_DATA;
      Node=At;
    }
  }
  if(Node<0)return EFI_SUCCESS;
  INT32 Length=0;
  CONST UINT8 *Existing=FdtGetProp(Tree,Node,"local-bd-address",&Length);
  if(Existing&&Length==6){
    UINT8 Forward[6];
    for(UINTN I=0;I<6;++I)Forward[I]=Existing[5-I];
    if(ValidAddress(Forward,FALSE))return EFI_SUCCESS;
  }
  INT32 Result=FdtSetProp(Tree,Node,"local-bd-address",mAddress,sizeof(mAddress));
  if(!Result){
    // Newly supplied data follows the binding, so no byte-order workaround applies.
    Result=FdtDelProp(Tree,Node,"qcom,local-bd-address-broken");
    if(Result==-FDT_ERR_NOTFOUND)Result=0;
  }
  DEBUG((DEBUG_WARN,"PIANO_BLUETOOTH_DTB source=%a fdt_status=%d standard_property=local-bd-address\n",mSource,Result));
  return !Result?EFI_SUCCESS:Result==-FDT_ERR_NOSPACE?EFI_BUFFER_TOO_SMALL:EFI_COMPROMISED_DATA;
}
