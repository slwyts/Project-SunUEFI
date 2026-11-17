// SPDX-License-Identifier: BSD-2-Clause-Patent
// Boot-service file-backed variables, using the standard variable API for
// restore. Never replace an initialized VariableRuntimeDxe cache or gRT table.
#include "PianoNvFile.h"
#include <Protocol/PianoNvFile.h>
#include "PianoGpt.h"
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PartitionInfo.h>
#include <Guid/FileInfo.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#define LIMIT (256U*1024U)
#define HEADER 40U
#define NAME_BYTES 2048U
STATIC EFI_GUID mGuid=PIANO_NV_FILE_PROTOCOL_GUID;
STATIC EFI_HANDLE mHandle,mVolumeHandle;
STATIC PIANO_NV_FILE_PROTOCOL mProtocol={1,0,FALSE};
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *mFs;
STATIC UINT64 mSequence,mSavedChanges;
STATIC UINT32 mActive;
STATIC BOOLEAN mStarted,mBusy;
STATIC BOOLEAN mUnavailable,mExited;
STATIC EFI_EVENT mExit;
STATIC CHAR16 *mNames[]={L"\\sunuefi\\nvram-a.bin",L"\\sunuefi\\nvram-b.bin"};
STATIC UINT32 U32(CONST UINT8 *P){UINT32 V;CopyMem(&V,P,4);return V;}
STATIC UINT64 U64(CONST UINT8 *P){UINT64 V;CopyMem(&V,P,8);return V;}
STATIC VOID W32(UINT8 *P,UINT32 V){CopyMem(P,&V,4);}
STATIC VOID W64(UINT8 *P,UINT64 V){CopyMem(P,&V,8);}
STATIC EFI_STATUS Volume(VOID){
  EFI_HANDLE *H=NULL;UINTN N=0,Found=0;EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&N,&H);
  if(S!=EFI_SUCCESS)return S;
  EFI_HANDLE Selected=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Filesystem=NULL;
  for(UINTN I=0;I<N;++I){EFI_PARTITION_INFO_PROTOCOL *P=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F=NULL;
    if(gBS->HandleProtocol(H[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&P)==EFI_SUCCESS&&P&&P->Type==PARTITION_TYPE_GPT&&
       !StrnCmp(P->Info.Gpt.PartitionName,L"persist",36)&&
       gBS->HandleProtocol(H[I],&gEfiSimpleFileSystemProtocolGuid,(VOID **)&F)==EFI_SUCCESS&&F){
      Selected=H[I];Filesystem=F;++Found;
    }
  }
  FreePool(H);
  if(Found!=1)return Found?EFI_COMPROMISED_DATA:EFI_NOT_FOUND;
  mVolumeHandle=Selected;mFs=Filesystem;
  return EFI_SUCCESS;
}
STATIC BOOLEAN Valid(UINT8 *Data,UINTN Bytes){
  if(Bytes<HEADER||Bytes>LIMIT||CompareMem(Data,"SUNUEFI-NVRAM-v1",16)||U32(Data+16)!=1||U32(Data+20)!=Bytes||!U64(Data+24))return FALSE;
  UINT32 C=U32(Data+32);W32(Data+32,0);UINT32 Actual=PianoGptCrc32(Data,Bytes);W32(Data+32,C);if(C!=Actual)return FALSE;
  UINTN At=HEADER;while(At<Bytes){
    if(Bytes-At<28)return FALSE;UINT32 Name=U32(Data+At+16),Size=U32(Data+At+20),Attr=U32(Data+At+24);
    if(Name<4||Name>NAME_BYTES||(Name&1)||Size>LIMIT||!(Attr&EFI_VARIABLE_NON_VOLATILE)||
       (Attr&(EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS|EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS|EFI_VARIABLE_APPEND_WRITE))||
       Name>Bytes-At-28||Size>Bytes-At-28-Name)return FALSE;
    CONST UINT8 *P=Data+At+28;for(UINT32 I=0;I<Name;I+=2)if((P[I]|P[I+1])==0&&I!=Name-2)return FALSE;
    if(P[Name-2]||P[Name-1])return FALSE;At+=28+Name+Size;
  }return At==Bytes;
}
STATIC EFI_STATUS ReadSlot(UINT32 Slot,UINT8 **Data,UINTN *Bytes){
  *Data=NULL;*Bytes=0;
  EFI_FILE_PROTOCOL *Root=NULL,*F=NULL;EFI_STATUS S=mFs->OpenVolume(mFs,&Root);if(S!=EFI_SUCCESS)return S;
  S=Root->Open(Root,&F,mNames[Slot],EFI_FILE_MODE_READ,0);EFI_STATUS C=Root->Close(Root);
  if(S==EFI_SUCCESS)S=C;if(S!=EFI_SUCCESS){if(F)F->Close(F);return S;}
  UINT8 InfoBuffer[SIZE_OF_EFI_FILE_INFO+128];UINTN N=sizeof(InfoBuffer);S=F->GetInfo(F,&gEfiFileInfoGuid,&N,InfoBuffer);
  if(S==EFI_SUCCESS){EFI_FILE_INFO *I=(VOID *)InfoBuffer;
    if(I->Attribute&EFI_FILE_DIRECTORY||I->FileSize<HEADER||I->FileSize>LIMIT)S=EFI_VOLUME_CORRUPTED;
    else{*Bytes=(UINTN)I->FileSize;*Data=AllocatePool(*Bytes);if(!*Data)S=EFI_OUT_OF_RESOURCES;
      else{N=*Bytes;S=F->Read(F,&N,*Data);if(S==EFI_SUCCESS&&(N!=*Bytes||!Valid(*Data,N)))S=EFI_VOLUME_CORRUPTED;}}
  }C=F->Close(F);return S==EFI_SUCCESS?C:S;
}
STATIC EFI_STATUS Restore(UINT8 *Data,UINTN Bytes){
  CHAR16 Name[NAME_BYTES/2];
  for(UINTN At=HEADER;At<Bytes;){EFI_GUID Guid;CopyMem(&Guid,Data+At,16);UINT32 N=U32(Data+At+16),Size=U32(Data+At+20),Attr=U32(Data+At+24);
    CopyMem(Name,Data+At+28,N);EFI_STATUS S=gRT->SetVariable(Name,&Guid,Attr,Size,Data+At+28+N);
    if(S!=EFI_SUCCESS&&S!=EFI_WRITE_PROTECTED)return S;At+=28+N+Size;
  }return EFI_SUCCESS;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){
  (VOID)Event;(VOID)Context;
  mExited=TRUE;mProtocol.Frozen=TRUE;mFs=NULL;mVolumeHandle=NULL;
}
EFI_STATUS PianoNvFileStart(VOID){
  if(mExited)return EFI_UNSUPPORTED;if(mStarted)return EFI_ALREADY_STARTED;EFI_STATUS S=Volume();if(S!=EFI_SUCCESS)return S;
  UINT8 *Data[2]={0};UINTN Bytes[2]={0};EFI_STATUS R[2];UINT32 Best=MAX_UINT32;
  for(UINT32 I=0;I<2;++I){R[I]=ReadSlot(I,&Data[I],&Bytes[I]);if(R[I]==EFI_SUCCESS&&(Best==MAX_UINT32||U64(Data[I]+24)>U64(Data[Best]+24)))Best=I;}
  if(R[0]==EFI_SUCCESS&&R[1]==EFI_SUCCESS&&U64(Data[0]+24)==U64(Data[1]+24)&&
     (Bytes[0]!=Bytes[1]||CompareMem(Data[0],Data[1],Bytes[0]))){for(UINTN I=0;I<2;++I)FreePool(Data[I]);return EFI_VOLUME_CORRUPTED;}
  if(Best!=MAX_UINT32){mActive=Best;mSequence=U64(Data[Best]+24);S=Restore(Data[Best],Bytes[Best]);}
  else S=R[0]==EFI_NOT_FOUND&&R[1]==EFI_NOT_FOUND?EFI_SUCCESS:EFI_VOLUME_CORRUPTED;
  for(UINTN I=0;I<2;++I)if(Data[I])FreePool(Data[I]);if(S!=EFI_SUCCESS)return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,&gEfiEventExitBootServicesGuid,&mExit);if(S!=EFI_SUCCESS)return S;
  S=gBS->InstallProtocolInterface(&mHandle,&mGuid,EFI_NATIVE_INTERFACE,&mProtocol);
  if(S!=EFI_SUCCESS){gBS->CloseEvent(mExit);mExit=NULL;return S;}
  mStarted=TRUE;mSavedChanges=mProtocol.Changes;
  DEBUG((DEBUG_WARN,"PIANO_NV_FILE_START status=%r sequence=%lu backend=persist boot_services_only=1\n",S,mSequence));return S;
}
STATIC EFI_STATUS Collect(UINT8 *Data,UINTN *Bytes){
  CHAR16 Name[NAME_BYTES/2]={0};EFI_GUID Guid={0};UINTN At=HEADER;
  for(UINTN Count=0;Count<4096;++Count){UINTN N=sizeof(Name);EFI_STATUS S=gRT->GetNextVariableName(&N,Name,&Guid);
    if(S==EFI_NOT_FOUND){*Bytes=At;return EFI_SUCCESS;}if(S!=EFI_SUCCESS)return S;
    UINT32 Attr=0;UINTN Size=0;S=gRT->GetVariable(Name,&Guid,&Attr,&Size,NULL);if(S!=EFI_BUFFER_TOO_SMALL&&S!=EFI_SUCCESS)return S;
    if(!(Attr&EFI_VARIABLE_NON_VOLATILE)||(Attr&(EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS|EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)))continue;
    N=StrSize(Name);if(N>NAME_BYTES||At>LIMIT-28||N>LIMIT-At-28||Size>LIMIT-At-28-N)return EFI_OUT_OF_RESOURCES;
    CopyMem(Data+At,&Guid,16);W32(Data+At+16,(UINT32)N);W32(Data+At+20,(UINT32)Size);W32(Data+At+24,Attr);CopyMem(Data+At+28,Name,N);
    S=gRT->GetVariable(Name,&Guid,&Attr,&Size,Data+At+28+N);if(S!=EFI_SUCCESS)return S;At+=28+N+Size;
  }return EFI_OUT_OF_RESOURCES;
}
EFI_STATUS PianoNvFileSave(BOOLEAN Freeze){
  if(mExited)return EFI_UNSUPPORTED;if(!mStarted)return EFI_NOT_STARTED;if(mUnavailable)return EFI_DEVICE_ERROR;
  if(mProtocol.Frozen)return Freeze?EFI_SUCCESS:EFI_NOT_READY;if(mBusy)return EFI_NOT_READY;
  if(mSavedChanges==mProtocol.Changes&&mSequence){if(Freeze)mProtocol.Frozen=TRUE;return EFI_SUCCESS;}
  if(mSequence==MAX_UINT64)return EFI_OUT_OF_RESOURCES;mBusy=TRUE;
  UINT64 Changes=mProtocol.Changes;UINT8 *Data=AllocateZeroPool(LIMIT);UINTN Bytes=0;EFI_STATUS S=Data?Collect(Data,&Bytes):EFI_OUT_OF_RESOURCES;
  if(S==EFI_SUCCESS){CopyMem(Data,"SUNUEFI-NVRAM-v1",16);W32(Data+16,1);W32(Data+20,(UINT32)Bytes);W64(Data+24,mSequence+1);W32(Data+32,PianoGptCrc32(Data,Bytes));
    EFI_FILE_PROTOCOL *Root=NULL,*Dir=NULL,*F=NULL;S=mFs->OpenVolume(mFs,&Root);
    if(S==EFI_SUCCESS)S=Root->Open(Root,&Dir,L"\\sunuefi",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
    if(Dir){EFI_STATUS C=Dir->Close(Dir);if(S==EFI_SUCCESS)S=C;}UINT32 Slot=mSequence?mActive^1:0;
    if(S==EFI_SUCCESS)S=Root->Open(Root,&F,mNames[Slot],EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(Root){EFI_STATUS C=Root->Close(Root);if(S==EFI_SUCCESS)S=C;}
    if(S==EFI_SUCCESS){UINTN N=Bytes;S=F->Write(F,&N,Data);if(S==EFI_SUCCESS&&N!=Bytes)S=EFI_DEVICE_ERROR;
      if(S==EFI_SUCCESS){UINT8 Info[SIZE_OF_EFI_FILE_INFO+128];N=sizeof(Info);S=F->GetInfo(F,&gEfiFileInfoGuid,&N,Info);if(S==EFI_SUCCESS){((EFI_FILE_INFO *)(VOID *)Info)->FileSize=Bytes;S=F->SetInfo(F,&gEfiFileInfoGuid,N,Info);}}
      if(S==EFI_SUCCESS)S=F->Flush(F);EFI_STATUS C=F->Close(F);if(S==EFI_SUCCESS)S=C;
    }else if(F)F->Close(F);
    if(S==EFI_SUCCESS){UINT8 *Check=NULL;UINTN N=0;S=ReadSlot(Slot,&Check,&N);if(S==EFI_SUCCESS&&(N!=Bytes||CompareMem(Check,Data,Bytes)))S=EFI_CRC_ERROR;if(Check)FreePool(Check);}
    if(S==EFI_SUCCESS){mActive=Slot;++mSequence;mSavedChanges=Changes;if(Freeze)mProtocol.Frozen=TRUE;}
  }
  if(Data){ZeroMem(Data,LIMIT);FreePool(Data);}mBusy=FALSE;
  if(S!=EFI_SUCCESS)mUnavailable=TRUE;
  DEBUG((DEBUG_WARN,"PIANO_NV_FILE_SAVE status=%r sequence=%lu frozen=%u\n",S,mSequence,mProtocol.Frozen));return S;
}
EFI_STATUS PianoNvFilePreparePartitionWrite(EFI_HANDLE Partition){
  if(!Partition)return EFI_INVALID_PARAMETER;
  if(mExited)return EFI_ABORTED;
  if(!mStarted || Partition!=mVolumeHandle || mProtocol.Frozen)return EFI_SUCCESS;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Filesystem=NULL;
  EFI_STATUS Status=gBS->HandleProtocol(Partition,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Filesystem);
  if(mExited)return EFI_ABORTED;
  if(Status!=EFI_SUCCESS || !Filesystem || Filesystem!=mFs)return EFI_MEDIA_CHANGED;
  return PianoNvFileSave(TRUE);
}
