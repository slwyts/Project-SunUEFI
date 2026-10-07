// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed native Env identity and audited SM8750 ABI, read-only inventory.
#include "PianoRamPartition.h"
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
STATIC EFI_GUID mRam={0x5172FFB5,0x4253,0x7D51,{0xC6,0x41,0xA7,0x01,0xF9,0x73,0x10,0x3C}};
STATIC EFI_GUID mEnv={0x90A49AFD,0x422F,0x08AE,{0x96,0x11,0xE7,0x88,0xD3,0x80,0x48,0x45}};
STATIC CONST UINT8 mHash[]={0x59,0x3d,0x9e,0x76,0x6c,0x00,0x70,0xe0,0x1d,0x4c,0x66,0x84,0xb7,0xbb,0x80,0x50,0xf3,0x2f,0x77,0xea,0x3c,0xa3,0x00,0xc6,0xa1,0x10,0xad,0x39,0x03,0xde,0x17,0xe3};
STATIC CONST UINTN mRva[]={0x2268,0x228c,0x22f4,0x22a8,0x23d4,0x2420};
_Static_assert(sizeof(UINTN)==8,"native ABI requires 64bit pointer/count platform");
_Static_assert(sizeof(PIANO_RAM_BANK)==16,"native bank output size");
_Static_assert(sizeof(PIANO_RAM_PRELOADED)==24,"native preloaded output size");
typedef struct {UINT64 Revision;UINTN Methods[6];} NATIVE_RAM;
typedef EFI_STATUS(EFIAPI *GET_BANKS)(VOID *,PIANO_RAM_BANK *,UINT32 *);
typedef EFI_STATUS(EFIAPI *GET_PRELOADED)(VOID *,PIANO_RAM_PRELOADED *,UINT64 *);
STATIC BOOLEAN mBusy,mBlocked;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Live(VOID){return gST!=NULL&&gBS!=NULL&&gST->BootServices==gBS&&gBS->Hdr.Signature==EFI_BOOT_SERVICES_SIGNATURE;}
STATIC EFI_STATUS FixTextRelocations(VOID *Source,UINTN Base){
  // Pinned PE has DIR64 pointer literals inside .text as well as .data.
  // Normalize those literals in our owned FV copy before comparing live text.
  UINT8 *Raw=Source;UINTN Offset=0x12000;
  while(Offset<0x13000){
    UINT32 Page,Bytes;CopyMem(&Page,Raw+Offset,4);CopyMem(&Bytes,Raw+Offset+4,4);
    if(Bytes==0)break;
    if(Bytes<8||(Bytes&3)||Bytes>0x13000-Offset)return EFI_COMPROMISED_DATA;
    for(UINTN I=Offset+8;I<Offset+Bytes;I+=2){
      UINT16 Entry;CopyMem(&Entry,Raw+I,2);if(!(Entry>>12))continue;
      if((Entry>>12)!=10)return EFI_UNSUPPORTED;
      UINT64 Target=(UINT64)Page+(Entry&0xfff);
      if(Target<0x1000||Target>=0x10000)continue;
      if(Target>0x10000-8)return EFI_COMPROMISED_DATA;
      UINT64 Value;CopyMem(&Value,Raw+(UINTN)Target,8);
      if(Value>MAX_UINT64-Base)return EFI_COMPROMISED_DATA;
      Value+=Base;CopyMem(Raw+(UINTN)Target,&Value,8);
    }
    Offset+=Bytes;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS MatchPath(CONST EFI_DEVICE_PATH_PROTOCOL *Path){
  // LoadedImage.FilePath for dispatched Env is its standard FV-file node.
  if(Path==NULL||Path->Type!=MEDIA_DEVICE_PATH||Path->SubType!=MEDIA_PIWG_FW_FILE_DP||
     ((UINTN)Path->Length[0]|((UINTN)Path->Length[1]<<8))!=sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH))return EFI_NOT_FOUND;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Node;CopyMem(&Node,Path,sizeof(Node));
  return CompareMem(&Node.FvFileName,&mEnv,sizeof(mEnv))==0?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS Fresh(VOID *Interface,EFI_HANDLE Handle,UINTN Base){
  if(!Live())return EFI_ABORTED;
  VOID *Now=NULL;EFI_STATUS S=gBS->LocateProtocol(&mRam,NULL,&Now);
  if(!Live())return EFI_ABORTED;
  if(S!=EFI_SUCCESS||Now!=Interface)return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
  EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;S=gBS->HandleProtocol(Handle,&gEfiLoadedImageProtocolGuid,(VOID **)&Image);
  if(!Live())return EFI_ABORTED;
  if(S!=EFI_SUCCESS||!Image||Image->ImageBase!=(VOID *)Base||Image->ImageSize!=0x13000||MatchPath(Image->FilePath)!=EFI_SUCCESS)return EFI_COMPROMISED_DATA;
  NATIVE_RAM Table;CopyMem(&Table,Interface,sizeof(Table));
  if(Table.Revision!=0x10002)return EFI_UNSUPPORTED;
  for(UINTN I=0;I<6;++I)if(Table.Methods[I]!=Base+mRva[I])return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS CallBanks(VOID *P,UINTN Fn,PIANO_RAM_BANK *B,UINT32 *Count){
#ifdef PIANO_RAM_PARTITION_HOST_TEST
  extern EFI_STATUS PianoRamHostBanks(VOID *,UINTN,PIANO_RAM_BANK *,UINT32 *);
  return PianoRamHostBanks(P,Fn,B,Count);
#else
  return ((GET_BANKS)Fn)(P,B,Count);
#endif
}
STATIC EFI_STATUS CallPreloaded(VOID *P,UINTN Fn,PIANO_RAM_PRELOADED *B,UINT64 *Count){
#ifdef PIANO_RAM_PARTITION_HOST_TEST
  extern EFI_STATUS PianoRamHostPreloaded(VOID *,UINTN,PIANO_RAM_PRELOADED *,UINT64 *);
  return PianoRamHostPreloaded(P,Fn,B,Count);
#else
  return ((GET_PRELOADED)Fn)(P,B,Count);
#endif
}
STATIC BOOLEAN Range(UINT64 Base,UINT64 Bytes){return Bytes!=0&&Base<=MAX_UINT64-Bytes;}
EFI_STATUS PianoRamPartitionInventory(BOOLEAN ExplicitFetch,PIANO_RAM_PARTITION_REPORT *R){
  if(!R)return EFI_INVALID_PARAMETER;
  if(mBusy||mBlocked){R->DataValid=FALSE;R->OwnershipVerified=FALSE;return R->Status=EFI_NOT_READY;}
  ZeroMem(R,sizeof(*R));R->Identity=R->Abi=R->BanksStatus=R->PreloadedStatus=EFI_NOT_STARTED;
  if(!Live()||!gBS->RaiseTPL||!gBS->RestoreTPL||!gBS->LocateProtocol||!gBS->LocateHandleBuffer||!gBS->HandleProtocol||!gBS->FreePool)return R->Status=EFI_UNSUPPORTED;
  mBusy=TRUE;EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  EFI_STATUS S=EFI_SUCCESS;VOID *P=NULL,*Source=NULL;EFI_HANDLE *Handles=NULL,Owner=NULL;UINTN Count=0,Bytes=0,Base=0;BOOLEAN Normalized=FALSE;
  if(!Live()){S=EFI_ABORTED;goto Done;}
  if(Old!=TPL_APPLICATION){S=EFI_UNSUPPORTED;goto Done;}
  S=gBS->LocateProtocol(&mRam,NULL,&P);R->Locate=S;
  if(!Live()){S=EFI_ABORTED;goto Done;}
  if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
  if(!P){S=EFI_COMPROMISED_DATA;goto Done;}R->Present=TRUE;R->InterfaceAddress=(UINTN)P;
  S=GetSectionFromAnyFv(&mEnv,EFI_SECTION_PE32,0,&Source,&Bytes);
  if(!Live()){S=EFI_ABORTED;goto Done;}
  if(S!=EFI_SUCCESS||!Source||Bytes!=0x13000){S=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
  UINT8 Hash[32];if(!Sha256HashAll(Source,Bytes,Hash)||CompareMem(Hash,mHash,sizeof(Hash))){S=EFI_SECURITY_VIOLATION;goto Done;}
  S=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if(!Live()){S=EFI_ABORTED;goto Done;}
  if(S!=EFI_SUCCESS||Count>256||!Count||!Handles){S=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
  for(UINTN I=0;I<Count;++I){
    EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;S=gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image);
    if(!Live()){S=EFI_ABORTED;goto Done;}
    if(S!=EFI_SUCCESS||!Image||Image->ImageSize!=0x13000||MatchPath(Image->FilePath)!=EFI_SUCCESS)continue;
    UINTN Candidate=(UINTN)Image->ImageBase;
    if(!Candidate||(Candidate&0xfff)||Candidate>MAX_UINTN-0x13000||(UINTN)P!=Candidate+0x102e0)continue;
    // Compare relocated pointer literals and actual loaded instructions,
    // not revision alone, before invoking any native callback.
    if(!Normalized){S=FixTextRelocations(Source,Candidate);if(S!=EFI_SUCCESS)goto Done;Normalized=TRUE;}
    if(CompareMem((VOID *)(Candidate+0x1000),(UINT8 *)Source+0x1000,0xf000))continue;
    if(Owner){S=EFI_COMPROMISED_DATA;goto Done;}Owner=Handles[I];Base=Candidate;
  }
  if(!Owner){S=EFI_NOT_FOUND;goto Done;}
  R->ImageBase=Base;R->ImageSize=0x13000;R->Identity=EFI_SUCCESS;R->IdentityVerified=TRUE;
  S=Fresh(P,Owner,Base);R->Abi=S;if(S!=EFI_SUCCESS)goto Done;
  R->Revision=0x10002;R->AbiVerified=TRUE;
  if(!ExplicitFetch){S=EFI_SUCCESS;goto Done;}
  R->FetchAttempted=TRUE;UINT32 Banks=0;S=CallBanks(P,Base+0x22f4,NULL,&Banks);
  if(!Live()){S=EFI_ABORTED;goto Done;}
  R->BanksStatus=S;
  if((S!=EFI_BUFFER_TOO_SMALL&&S!=EFI_SUCCESS)||Banks>PIANO_RAM_PARTITION_MAX||(Banks!=0&&S!=EFI_BUFFER_TOO_SMALL)){S=S==EFI_SUCCESS||S==EFI_BUFFER_TOO_SMALL?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
  if(Banks){
    S=Fresh(P,Owner,Base);if(S!=EFI_SUCCESS)goto Done;
    UINT32 Capacity=Banks;S=CallBanks(P,Base+0x22f4,R->Banks,&Capacity);R->BanksStatus=S;
    if(!Live()){S=EFI_ABORTED;goto Done;}
    if(S!=EFI_SUCCESS||Capacity!=Banks){S=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
    R->BankCount=Banks;
    for(UINTN I=0;I<Banks;++I){
      // The pinned native getter preserves empty current records. They are
      // observations, not ranges which can overlap or authorize allocation.
      if(R->Banks[I].AvailableLength&&!Range(R->Banks[I].Base,R->Banks[I].AvailableLength)){S=EFI_COMPROMISED_DATA;goto Done;}
      for(UINTN J=0;J<I;++J)if(R->Banks[I].AvailableLength&&R->Banks[J].AvailableLength&&
        R->Banks[I].Base<R->Banks[J].Base+R->Banks[J].AvailableLength&&R->Banks[J].Base<R->Banks[I].Base+R->Banks[I].AvailableLength){S=EFI_COMPROMISED_DATA;goto Done;}
    }
  }
  R->PotentialFallback=Banks==1&&R->Banks[0].Base==0x80000000&&R->Banks[0].AvailableLength==0x60000000;
  S=Fresh(P,Owner,Base);if(S!=EFI_SUCCESS)goto Done;
  UINT64 Images=0;S=CallPreloaded(P,Base+0x2420,NULL,&Images);R->PreloadedStatus=S;
  if(!Live()){S=EFI_ABORTED;goto Done;}
  if((S!=EFI_BUFFER_TOO_SMALL&&S!=EFI_SUCCESS)||Images>PIANO_RAM_PARTITION_MAX||(Images!=0&&S!=EFI_BUFFER_TOO_SMALL)){S=S==EFI_SUCCESS||S==EFI_BUFFER_TOO_SMALL?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
  if(Images){
    S=Fresh(P,Owner,Base);if(S!=EFI_SUCCESS)goto Done;
    UINT64 Capacity=Images;S=CallPreloaded(P,Base+0x2420,R->Preloaded,&Capacity);R->PreloadedStatus=S;
    if(!Live()){S=EFI_ABORTED;goto Done;}
    if(S!=EFI_SUCCESS||Capacity!=Images){S=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);goto Done;}
    R->PreloadedCount=Images;
    for(UINTN I=0;I<Images;++I)if(!Range(R->Preloaded[I].Base,R->Preloaded[I].Size)){S=EFI_COMPROMISED_DATA;goto Done;}
  }
  S=Fresh(P,Owner,Base);
Done:
  // Temporary source/handle buffers only. Target DDR is never mapped/allocated.
  if(!Live()){R->RetainedSource=Source;R->RetainedHandles=Handles;R->Retained=Source!=NULL||Handles!=NULL;S=EFI_ABORTED;}
  else {
    if(Source){EFI_STATUS F=gBS->FreePool(Source);R->Release=F;if(F!=EFI_SUCCESS){R->RetainedSource=Source;R->Retained=TRUE;S=Exact(F);}}
    if(!Live()){R->RetainedHandles=Handles;R->Retained=TRUE;S=EFI_ABORTED;}
    else if(Handles){EFI_STATUS F=gBS->FreePool(Handles);R->Release=F;if(F!=EFI_SUCCESS){R->RetainedHandles=Handles;R->Retained=TRUE;S=Exact(F);}}
    if(!Live()){R->Retained=TRUE;S=EFI_ABORTED;}
  }
  R->DataValid=S==EFI_SUCCESS&&ExplicitFetch&&!R->Retained;
  R->OwnershipVerified=FALSE;mBlocked=R->Retained||S==EFI_ABORTED||(R->FetchAttempted&&S!=EFI_SUCCESS);
  mBusy=FALSE;R->Status=Exact(S);return R->Status;
}
