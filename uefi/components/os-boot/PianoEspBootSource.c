// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoEspBootSource.h"
#include <Protocol/PartitionInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
STATIC CONST EFI_GUID mEspType={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}};
STATIC CONST EFI_GUID mVendor={0xa8675600,0x87d0,0x4a29,{0x9b,0x40,0x60,0,0,0,0,1}};
#pragma pack(1)
typedef struct {VENDOR_DEVICE_PATH Vendor;UFS_DEVICE_PATH Ufs;HARDDRIVE_DEVICE_PATH Hd;EFI_DEVICE_PATH_PROTOCOL End;} ESP_PATH;
#pragma pack()
typedef struct {
 EFI_PARTITION_INFO_PROTOCOL *Partition;EFI_BLOCK_IO_PROTOCOL *Block,*Parent;
 EFI_BLOCK_IO_MEDIA *Media,*ParentMedia;EFI_DEVICE_PATH_PROTOCOL *Path,*ParentPath;
 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;EFI_BLOCK_READ Read,ParentRead;
 EFI_STATUS (EFIAPI *Open)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,EFI_FILE_PROTOCOL **);
 EFI_PARTITION_ENTRY Entry;ESP_PATH DevicePath;
 UINT32 MediaId,ParentMediaId,BlockBytes;UINT64 LastBlock,ParentLastBlock,SfsRevision;
 BOOLEAN ReadOnly,ParentReadOnly;
} ESP_IDENTITY;
STATIC BOOLEAN Node(CONST EFI_DEVICE_PATH_PROTOCOL *P,UINT8 T,UINT8 S,UINTN N){
 return P&&P->Type==T&&P->SubType==S&&(UINTN)(P->Length[0]|((UINT16)P->Length[1]<<8))==N;
}
STATIC BOOLEAN Parent(ESP_IDENTITY *Id){
 EFI_HANDLE *Handles=NULL;UINTN Count=0,Matches=0;
 EFI_STATUS E=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Count,&Handles);
 if(E!=EFI_SUCCESS)return FALSE;
 if(!Handles||!Count||Count>256){if(Handles)gBS->FreePool(Handles);return FALSE;}
 for(UINTN I=0;I<Count;++I){
  EFI_BLOCK_IO_PROTOCOL *B=NULL;EFI_DEVICE_PATH_PROTOCOL *D=NULL;
  if(gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&B)!=EFI_SUCCESS||!B||!B->ReadBlocks||!B->Media||
     !B->Media->MediaPresent||B->Media->LogicalPartition||B->Media->BlockSize!=Id->BlockBytes||
     B->Media->LastBlock>=MAX_UINT64/Id->BlockBytes||Id->Entry.EndingLBA>B->Media->LastBlock)continue;
  if(gBS->HandleProtocol(Handles[I],&gEfiDevicePathProtocolGuid,(VOID **)&D)!=EFI_SUCCESS||
     !Node(D,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(VENDOR_DEVICE_PATH)))continue;
  UFS_DEVICE_PATH *U=(VOID *)((UINT8 *)D+sizeof(VENDOR_DEVICE_PATH));
  if(!Node(&U->Header,MESSAGING_DEVICE_PATH,MSG_UFS_DP,sizeof(*U))||
     CompareMem(D,&Id->DevicePath.Vendor,sizeof(VENDOR_DEVICE_PATH))||CompareMem(U,&Id->DevicePath.Ufs,sizeof(*U))||
     !Node((VOID *)((UINT8 *)U+sizeof(*U)),END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,sizeof(EFI_DEVICE_PATH_PROTOCOL)))continue;
  Id->Parent=B;Id->ParentMedia=B->Media;Id->ParentPath=D;Id->ParentRead=B->ReadBlocks;
  Id->ParentMediaId=B->Media->MediaId;Id->ParentLastBlock=B->Media->LastBlock;Id->ParentReadOnly=B->Media->ReadOnly;++Matches;
 }
 E=gBS->FreePool(Handles);return E==EFI_SUCCESS&&Matches==1;
}
STATIC BOOLEAN Volume(EFI_HANDLE H,ESP_IDENTITY *Id){
 EFI_PARTITION_INFO_PROTOCOL *P=NULL;EFI_BLOCK_IO_PROTOCOL *B=NULL;EFI_DEVICE_PATH_PROTOCOL *D=NULL;
 ZeroMem(Id,sizeof(*Id));
 if(gBS->HandleProtocol(H,&gEfiPartitionInfoProtocolGuid,(VOID **)&P)!=EFI_SUCCESS||!P||
    P->Revision<EFI_PARTITION_INFO_PROTOCOL_REVISION||P->Type!=PARTITION_TYPE_GPT||
    !CompareGuid(&P->Info.Gpt.PartitionTypeGUID,&mEspType))return FALSE;
 STATIC CONST CHAR16 Name[36]=L"sunuefi_esp";
 STATIC CONST EFI_GUID ZeroGuid={0};
 if(CompareMem(P->Info.Gpt.PartitionName,Name,sizeof(Name))||CompareGuid(&P->Info.Gpt.UniquePartitionGUID,&ZeroGuid)||
    !P->Info.Gpt.StartingLBA||P->Info.Gpt.EndingLBA<P->Info.Gpt.StartingLBA||P->Info.Gpt.EndingLBA==MAX_UINT64)return FALSE;
 UINT64 Blocks=P->Info.Gpt.EndingLBA-P->Info.Gpt.StartingLBA+1;
 if(gBS->HandleProtocol(H,&gEfiBlockIoProtocolGuid,(VOID **)&B)!=EFI_SUCCESS||!B||!B->ReadBlocks||!B->Media||
    !B->Media->MediaPresent||!B->Media->LogicalPartition||B->Media->BlockSize!=4096||
    Blocks>MAX_UINT64/B->Media->BlockSize||B->Media->LastBlock!=Blocks-1)return FALSE;
 if(gBS->HandleProtocol(H,&gEfiDevicePathProtocolGuid,(VOID **)&D)!=EFI_SUCCESS||!Node(D,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(VENDOR_DEVICE_PATH)))return FALSE;
 VENDOR_DEVICE_PATH *V=(VOID *)D;if(!CompareGuid(&V->Guid,&mVendor))return FALSE;
 UFS_DEVICE_PATH *U=(VOID *)((UINT8 *)V+sizeof(*V));
 if(!Node(&U->Header,MESSAGING_DEVICE_PATH,MSG_UFS_DP,sizeof(*U))||U->Pun||U->Lun)return FALSE;
 HARDDRIVE_DEVICE_PATH *Hd=(VOID *)((UINT8 *)U+sizeof(*U));
 if(!Node(&Hd->Header,MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,sizeof(*Hd))||!Hd->PartitionNumber||
    Hd->MBRType!=MBR_TYPE_EFI_PARTITION_TABLE_HEADER||Hd->SignatureType!=SIGNATURE_TYPE_GUID||
    CompareMem(Hd->Signature,&P->Info.Gpt.UniquePartitionGUID,sizeof(EFI_GUID))||
    Hd->PartitionStart!=P->Info.Gpt.StartingLBA||Hd->PartitionSize!=Blocks)return FALSE;
 if(!Node((VOID *)((UINT8 *)Hd+sizeof(*Hd)),END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,sizeof(EFI_DEVICE_PATH_PROTOCOL)))return FALSE;
 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F=NULL;
 if(gBS->HandleProtocol(H,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&F)!=EFI_SUCCESS||!F||!F->OpenVolume)return FALSE;
 Id->Partition=P;Id->Block=B;Id->Media=B->Media;Id->Path=D;Id->Sfs=F;Id->Open=F->OpenVolume;Id->SfsRevision=F->Revision;Id->Read=B->ReadBlocks;
 CopyMem(&Id->Entry,&P->Info.Gpt,sizeof(Id->Entry));CopyMem(&Id->DevicePath,D,sizeof(Id->DevicePath));
 Id->MediaId=B->Media->MediaId;Id->BlockBytes=B->Media->BlockSize;Id->LastBlock=B->Media->LastBlock;Id->ReadOnly=B->Media->ReadOnly;
 return Parent(Id);
}
EFI_STATUS PianoEspBootLoad(PIANO_ESP_BOOT_SOURCE *S,CONST PIANO_CPU_INPUT_ENV *Cpu){
 if(!S||!Cpu||!Cpu->BootServicesAlive||!Cpu->ServiceSlice)return EFI_INVALID_PARAMETER;
 if(S->Loaded||S->Released||S->Retained||S->File.Retained||S->File.ServicesLost)return EFI_ALREADY_STARTED;
 if(S->File.Signature){
   EFI_STATUS Cleanup=PianoBootFileDispose(&S->File);
   if(Cleanup!=EFI_SUCCESS){S->Retained=S->File.Retained;return Cleanup;}
   if(!S->File.Consumed||S->File.File||S->File.Root||S->File.Data||S->File.Exit||S->File.Owner||S->File.Loan||S->File.Taken||S->Owner||S->Loan||S->View)return EFI_ACCESS_DENIED;
   ZeroMem(S,sizeof(*S));
 }
 if(!Cpu->BootServicesAlive(Cpu->Context))return EFI_ABORTED;
 EFI_HANDLE *Handles=NULL,Found=NULL;UINTN Count=0,Matches=0;ESP_IDENTITY Before={0},After;
 EFI_STATUS E=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
 if(E!=EFI_SUCCESS)return E;
 if(!Handles||!Count||Count>256){if(Handles)gBS->FreePool(Handles);return EFI_COMPROMISED_DATA;}
 for(UINTN I=0;I<Count;++I){ESP_IDENTITY Candidate;if(Volume(Handles[I],&Candidate)){Found=Handles[I];Before=Candidate;++Matches;}}
 E=gBS->FreePool(Handles);if(E!=EFI_SUCCESS)return E;
 if(!Cpu->BootServicesAlive(Cpu->Context))return EFI_ABORTED;
 if(Matches!=1)return Matches?EFI_COMPROMISED_DATA:EFI_NOT_FOUND;
 S->Cpu=*Cpu;S->Env=(PIANO_BOOT_FILE_ENV){.Context=Cpu->Context,.Services=gBS,.BootServicesAlive=Cpu->BootServicesAlive,.Cpu=&S->Cpu};
 UINT64 Capacity=(Before.LastBlock+1)*Before.BlockBytes;
 PIANO_BOOT_FILE_SPEC Spec={Found,L"\\EFI\\Piano\\stable\\boot.img",
   Capacity<PIANO_BOOT_FILE_LOW_BUDGET?Capacity:PIANO_BOOT_FILE_LOW_BUDGET,NULL};
 E=PianoBootFileLoad(&S->File,&S->Env,&Spec,&S->Reader,&S->Blob);
 if(E!=EFI_SUCCESS){S->Retained=S->File.Retained;return E;}
 if(!Volume(Found,&After)||CompareMem(&Before,&After,sizeof(Before))){E=PianoBootFileDispose(&S->File);S->Retained=E!=EFI_SUCCESS;return E==EFI_SUCCESS?EFI_MEDIA_CHANGED:E;}
 E=S->Blob.Take(S->Blob.Context,&S->Owner);
 if(E!=EFI_SUCCESS){EFI_STATUS R=PianoBootFileDispose(&S->File);S->Retained=R!=EFI_SUCCESS;return R==EFI_SUCCESS?E:R;}
 S->Loaded=TRUE;return EFI_SUCCESS;
}
BOOLEAN PianoEspBootOwned(CONST PIANO_ESP_BOOT_SOURCE *S,VOID *Owner){
 return S&&S->Loaded&&!S->Released&&!S->Retained&&Owner&&Owner==S->Owner&&S->File.Owner==Owner&&
 S->File.Ready&&S->File.Taken&&!S->File.Consumed&&!S->File.Retained&&!S->File.ServicesLost&&
 !S->File.File&&!S->File.Root&&S->File.Data&&S->File.Exit&&S->Env.BootServicesAlive(S->Env.Context);
}
EFI_STATUS PianoEspBootRelease(PIANO_ESP_BOOT_SOURCE *S){
 if(!S)return EFI_INVALID_PARAMETER;if(S->Released)return EFI_SUCCESS;
 if(!PianoEspBootOwned(S,S->Owner))return EFI_ACCESS_DENIED;
 if(S->Loan){EFI_STATUS E=S->Blob.Unborrow(S->Blob.Context,S->Owner,S->Loan);if(E!=EFI_SUCCESS){S->Retained=TRUE;return E;}S->Loan=NULL;S->View=NULL;}
 EFI_STATUS E=S->Blob.ZeroRelease(S->Blob.Context,S->Owner);if(E!=EFI_SUCCESS){S->Retained=TRUE;return E;}
 S->Owner=NULL;S->Loaded=FALSE;S->Released=TRUE;return EFI_SUCCESS;
}
BOOLEAN PianoEspBootReleased(CONST PIANO_ESP_BOOT_SOURCE *S){
 return S&&S->Released&&!S->Loaded&&!S->Retained&&!S->Owner&&!S->Loan&&!S->View&&S->File.Consumed&&
 !S->File.Taken&&!S->File.Owner&&!S->File.Loan&&!S->File.File&&!S->File.Root&&!S->File.Exit&&!S->File.Data&&!S->File.Retained;
}
