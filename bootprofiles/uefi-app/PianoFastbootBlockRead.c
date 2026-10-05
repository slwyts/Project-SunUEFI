// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFastbootBlockRead.h"
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#define MAX_PARTITIONS 160U
typedef struct {
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_BLOCK_READ Read;
  EFI_PARTITION_ENTRY Gpt;
  HARDDRIVE_DEVICE_PATH Hd;
  CHAR8 Name[37];
  UINT64 Bytes;
  UINT32 MediaId;
  UINT32 IoAlign;
  UINT8 Lun;
  VOID *Token;
} PARTITION;
STATIC PARTITION mPartitions[MAX_PARTITIONS];
STATIC UINTN mCount;
STATIC BOOLEAN mReady;
// Opaque cookies never repeat after Stop/Init; a stale Info must not become
// valid merely because a new partition occupies the same static array slot.
STATIC UINTN mNextToken=1;
STATIC CONST EFI_GUID mVendor={0xA8675600,0x87D0,0x4A29,{0x9B,0x40,0x60,0,0,0,0,1}};
STATIC BOOLEAN OwnPath(EFI_DEVICE_PATH_PROTOCOL *P,UINT8 *Lun,HARDDRIVE_DEVICE_PATH *Hd){
  if(P==NULL || P->Type!=HARDWARE_DEVICE_PATH || P->SubType!=HW_VENDOR_DP ||
     (P->Length[0]|((UINT16)P->Length[1]<<8))!=sizeof(VENDOR_DEVICE_PATH))return FALSE;
  VENDOR_DEVICE_PATH *V=(VOID *)P;if(!CompareGuid(&V->Guid,&mVendor))return FALSE;
  UFS_DEVICE_PATH *U=(VOID *)((UINT8 *)P+sizeof(*V));
  if(U->Header.Type!=MESSAGING_DEVICE_PATH || U->Header.SubType!=MSG_UFS_DP ||
     (U->Header.Length[0]|((UINT16)U->Header.Length[1]<<8))!=sizeof(*U) || U->Pun!=0 || U->Lun>5)return FALSE;
  HARDDRIVE_DEVICE_PATH *H=(VOID *)((UINT8 *)U+sizeof(*U));
  if(H->Header.Type!=MEDIA_DEVICE_PATH || H->Header.SubType!=MEDIA_HARDDRIVE_DP ||
     (H->Header.Length[0]|((UINT16)H->Header.Length[1]<<8))!=sizeof(*H) ||
     H->MBRType!=MBR_TYPE_EFI_PARTITION_TABLE_HEADER || H->SignatureType!=SIGNATURE_TYPE_GUID)return FALSE;
  EFI_DEVICE_PATH_PROTOCOL *E=(VOID *)((UINT8 *)H+sizeof(*H));
  if(E->Type!=END_DEVICE_PATH_TYPE || E->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE ||
     (E->Length[0]|((UINT16)E->Length[1]<<8))!=sizeof(*E))return FALSE;
  // Exact vendor/UFS/HD/END_ENTIRE layout: reject an inserted node, a second
  // instance, and extended/truncated node lengths. The protocol does not
  // expose allocator size; bytes after END_ENTIRE are outside this path.
  if(!H->PartitionNumber)return FALSE;
  *Lun=U->Lun;CopyMem(Hd,H,sizeof(*Hd));return TRUE;
}
STATIC BOOLEAN Geometry(CONST HARDDRIVE_DEVICE_PATH *Hd,CONST EFI_PARTITION_ENTRY *Gpt,UINT64 LastBlock) {
  EFI_GUID Zero={0};
  if(CompareGuid(&Gpt->PartitionTypeGUID,&Zero) || CompareGuid(&Gpt->UniquePartitionGUID,&Zero) ||
     !Gpt->StartingLBA || Gpt->EndingLBA<Gpt->StartingLBA || LastBlock>=MAX_UINT64/4096)return FALSE;
  UINT64 Blocks=Gpt->EndingLBA-Gpt->StartingLBA+1;
  return Blocks==LastBlock+1 && Hd->PartitionStart==Gpt->StartingLBA && Hd->PartitionSize==Blocks &&
    !CompareMem(Hd->Signature,&Gpt->UniquePartitionGUID,sizeof(EFI_GUID));
}
STATIC BOOLEAN NameOf(CONST CHAR16 *Wide,CHAR8 Out[37]){
  ZeroMem(Out,37);
  UINTN N=0;for(;N<36 && Wide[N];++N){
    UINT16 C=Wide[N];if(!((C>='a' && C<='z') || (C>='A' && C<='Z') ||
       (C>='0' && C<='9') || C=='_' || C=='-' || C=='.'))return FALSE;
    Out[N]=(CHAR8)C;
  }
  Out[N]=0;
  for(UINTN I=N;I<36;++I)if(Wide[I]!=0)return FALSE;
  return N>0;
}
STATIC EFI_STATUS Current(PARTITION *P){
  EFI_BLOCK_IO_PROTOCOL *B=NULL;EFI_PARTITION_INFO_PROTOCOL *I=NULL;EFI_DEVICE_PATH_PROTOCOL *D=NULL;UINT8 Lun;HARDDRIVE_DEVICE_PATH Hd;
  if(gBS->HandleProtocol(P->Handle,&gEfiBlockIoProtocolGuid,(VOID **)&B)!=EFI_SUCCESS ||
     gBS->HandleProtocol(P->Handle,&gEfiPartitionInfoProtocolGuid,(VOID **)&I)!=EFI_SUCCESS ||
     gBS->HandleProtocol(P->Handle,&gEfiDevicePathProtocolGuid,(VOID **)&D)!=EFI_SUCCESS)return EFI_NO_MEDIA;
  if(B!=P->Block || B==NULL || B->Revision<EFI_BLOCK_IO_PROTOCOL_REVISION || B->ReadBlocks!=P->Read || B->ReadBlocks==NULL ||
     B->Media==NULL || B->Media->MediaPresent!=TRUE || B->Media->ReadOnly!=TRUE || B->Media->LogicalPartition!=TRUE ||
     B->Media->MediaId!=P->MediaId || B->Media->IoAlign!=P->IoAlign || B->Media->IoAlign>1 ||
     B->Media->BlockSize!=4096 || B->Media->LastBlock!=(P->Bytes/4096)-1 ||
     I==NULL || I->Revision<EFI_PARTITION_INFO_PROTOCOL_REVISION || I->Type!=PARTITION_TYPE_GPT || CompareMem(&I->Info.Gpt,&P->Gpt,sizeof(P->Gpt)) ||
     !OwnPath(D,&Lun,&Hd) || Lun!=P->Lun || CompareMem(&Hd,&P->Hd,sizeof(Hd)) ||
     !Geometry(&Hd,&I->Info.Gpt,B->Media->LastBlock))return EFI_MEDIA_CHANGED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Ready(VOID *Context){
  (VOID)Context;if(!mReady || !mCount)return EFI_NOT_READY;
  for(UINTN N=0;N<mCount;++N){EFI_STATUS S=Current(&mPartitions[N]);if(S!=EFI_SUCCESS)return S;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Out){
  (VOID)Context;if(Name==NULL || Out==NULL)return EFI_INVALID_PARAMETER;ZeroMem(Out,sizeof(*Out));
  if(!mReady)return EFI_NOT_READY;
  PARTITION *Found=NULL;for(UINTN N=0;N<mCount;++N)if(!AsciiStrCmp(Name,mPartitions[N].Name)){
    if(Found!=NULL)return EFI_NO_MAPPING;
    Found=&mPartitions[N];
  }
  if(Found==NULL)return EFI_NOT_FOUND;
  EFI_STATUS S=Current(Found);if(S!=EFI_SUCCESS)return S;
  CopyMem(Out->Name,Found->Name,sizeof(Out->Name));Out->Bytes=Found->Bytes;
  Out->BlockSize=4096;Out->ReadOnly=TRUE;Out->Token=Found->Token;return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadBlocks(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){
  (VOID)Context;if(!mReady)return EFI_NOT_READY;
  if(Info==NULL || Buffer==NULL || !Bytes || Bytes%4096 || Bytes>65536)return EFI_INVALID_PARAMETER;
  PARTITION *P=NULL;
  for(UINTN N=0;N<mCount;++N)if(Info->Token==mPartitions[N].Token){P=&mPartitions[N];break;}
  if(P==NULL)return EFI_INVALID_PARAMETER;
  if(Info->Bytes!=P->Bytes || Info->BlockSize!=4096 || Info->ReadOnly!=TRUE ||
     AsciiStrnCmp(Info->Name,P->Name,sizeof(Info->Name)) || Lba>=P->Bytes/4096 || Bytes/4096>P->Bytes/4096-Lba)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Current(P);if(S!=EFI_SUCCESS)return S;
  // The actual Piano provider advertises IoAlign1 and does shared-DMA bounce.
  // Do not claim that alignment on arbitrary third-party BlockIO providers.
  if(P->Block->Media->IoAlign>1)return EFI_UNSUPPORTED;
  VOID *Token=P->Token;
  S=P->Block->ReadBlocks(P->Block,P->MediaId,Lba,Bytes,Buffer);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  if(!mReady || P->Token!=Token)return EFI_MEDIA_CHANGED;
  return Current(P); // Revoke a snapshot changed during the synchronous read.
}
STATIC CONST PIANO_FB_STORAGE mStorage={NULL,Ready,Info,ReadBlocks};
EFI_STATUS PianoFastbootBlockReadInit(VOID){
  if(mReady)return EFI_ALREADY_STARTED;
  mCount=0;ZeroMem(mPartitions,sizeof(mPartitions));
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Count,&Handles);
  if(S!=EFI_SUCCESS){if(Handles!=NULL)FreePool(Handles);return S;}
  if((Count && Handles==NULL) || Count>MAX_UINTN/sizeof(EFI_HANDLE)){if(Handles!=NULL)FreePool(Handles);return EFI_COMPROMISED_DATA;}
  for(UINTN N=0;N<Count;++N){
    EFI_DEVICE_PATH_PROTOCOL *D=NULL;EFI_PARTITION_INFO_PROTOCOL *I=NULL;EFI_BLOCK_IO_PROTOCOL *B=NULL;UINT8 Lun;HARDDRIVE_DEVICE_PATH Hd;
    if(gBS->HandleProtocol(Handles[N],&gEfiDevicePathProtocolGuid,(VOID **)&D)!=EFI_SUCCESS || !OwnPath(D,&Lun,&Hd))continue;
    if(gBS->HandleProtocol(Handles[N],&gEfiPartitionInfoProtocolGuid,(VOID **)&I)!=EFI_SUCCESS || I==NULL ||
       I->Revision<EFI_PARTITION_INFO_PROTOCOL_REVISION || I->Type!=PARTITION_TYPE_GPT ||
       gBS->HandleProtocol(Handles[N],&gEfiBlockIoProtocolGuid,(VOID **)&B)!=EFI_SUCCESS || B==NULL || B->Revision<EFI_BLOCK_IO_PROTOCOL_REVISION ||
       B->ReadBlocks==NULL || B->Media==NULL || B->Media->MediaPresent!=TRUE || B->Media->ReadOnly!=TRUE ||
       B->Media->LogicalPartition!=TRUE || B->Media->BlockSize!=4096 || B->Media->IoAlign>1 ||
       !Geometry(&Hd,&I->Info.Gpt,B->Media->LastBlock))continue;
    CHAR8 Name[37];if(!NameOf(I->Info.Gpt.PartitionName,Name))continue;
    if(mCount==MAX_PARTITIONS || mNextToken==MAX_UINTN){S=EFI_OUT_OF_RESOURCES;break;}
    PARTITION *P=&mPartitions[mCount++];P->Handle=Handles[N];P->Block=B;P->Gpt=I->Info.Gpt;
    P->Hd=Hd;P->Read=B->ReadBlocks;P->IoAlign=B->Media->IoAlign;P->Token=(VOID *)mNextToken++;
    CopyMem(P->Name,Name,sizeof(P->Name));P->Bytes=(B->Media->LastBlock+1)*4096;P->MediaId=B->Media->MediaId;P->Lun=Lun;
  }
  if(Handles!=NULL)FreePool(Handles);
  if(S!=EFI_SUCCESS || !mCount){mCount=0;ZeroMem(mPartitions,sizeof(mPartitions));return S!=EFI_SUCCESS?S:EFI_NOT_FOUND;}
  mReady=TRUE;return EFI_SUCCESS;
}
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID){return mReady?&mStorage:NULL;}
VOID PianoFastbootBlockReadStop(VOID){mReady=FALSE;mCount=0;ZeroMem(mPartitions,sizeof(mPartitions));}
