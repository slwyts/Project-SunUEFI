// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inspect only SimpleFileSystem handles below the real read-only UFS parents.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include "PianoGpt.h"

#define MAX_REPORTS 32
#define MAX_INFO_BYTES 65536
#define MAX_DIRECTORY_ENTRIES 64
typedef struct {
  EFI_HANDLE Handle;
  UINTN Parent;
  EFI_STATUS Open,Info,Directory,File;
  UINTN Entries,ReadBytes;
  UINT32 Crc;
  BOOLEAN ReadOnly;
} FS_REPORT;
STATIC FS_REPORT mReports[MAX_REPORTS];
STATIC UINTN mAllFileSystems,mUfsFileSystems,mReportCount;
STATIC EFI_STATUS mLocateStatus=EFI_NOT_STARTED;

STATIC BOOLEAN Descendant(EFI_DEVICE_PATH_PROTOCOL *Path,EFI_DEVICE_PATH_PROTOCOL *Parent) {
  if(Path==NULL || Parent==NULL)return FALSE;
  // Compare complete nodes; exclude the parent's end node. The bounded node
  // count also rejects malformed paths instead of following them indefinitely.
  for(UINTN N=0;N<64;++N) {
    UINTN PBytes=Parent->Length[0]|((UINTN)Parent->Length[1]<<8);
    UINTN Bytes=Path->Length[0]|((UINTN)Path->Length[1]<<8);
    if(PBytes<sizeof(*Parent) || Bytes<sizeof(*Path))return FALSE;
    if(Parent->Type==END_DEVICE_PATH_TYPE)return Parent->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE;
    if(Path->Type==END_DEVICE_PATH_TYPE || Bytes!=PBytes || CompareMem(Path,Parent,PBytes))return FALSE;
    Parent=(VOID *)((UINT8 *)Parent+PBytes);Path=(VOID *)((UINT8 *)Path+Bytes);
  }
  return FALSE;
}

STATIC EFI_STATUS GetInfo(EFI_FILE_PROTOCOL *File,EFI_GUID *Guid,VOID **Info,UINTN *InfoBytes) {
  UINTN Bytes=0;*Info=NULL;*InfoBytes=0;
  EFI_STATUS Status=File->GetInfo(File,Guid,&Bytes,NULL);
  if(Status!=EFI_BUFFER_TOO_SMALL)return EFI_ERROR(Status)?Status:EFI_COMPROMISED_DATA;
  if(Bytes==0 || Bytes>MAX_INFO_BYTES)return EFI_BAD_BUFFER_SIZE;
  VOID *Buffer=AllocateZeroPool(Bytes);if(Buffer==NULL)return EFI_OUT_OF_RESOURCES;
  UINTN Capacity=Bytes;Status=File->GetInfo(File,Guid,&Bytes,Buffer);
  if(!EFI_ERROR(Status) && Bytes>Capacity)Status=EFI_COMPROMISED_DATA;
  if(EFI_ERROR(Status)){FreePool(Buffer);return Status;}
  *Info=Buffer;*InfoBytes=Bytes;return EFI_SUCCESS;
}

STATIC VOID InspectVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,FS_REPORT *Report) {
  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  Report->Open=Fs->OpenVolume(Fs,&Root);
  if(EFI_ERROR(Report->Open) || Root==NULL){if(!EFI_ERROR(Report->Open))Report->Open=EFI_DEVICE_ERROR;return;}
  EFI_FILE_SYSTEM_INFO *Info=NULL;UINTN Bytes=0;
  Report->Info=GetInfo(Root,&gEfiFileSystemInfoGuid,(VOID **)&Info,&Bytes);
  if(!EFI_ERROR(Report->Info)) {
    if(Bytes<SIZE_OF_EFI_FILE_SYSTEM_INFO || Info->Size>Bytes || Info->Size<SIZE_OF_EFI_FILE_SYSTEM_INFO)
      Report->Info=EFI_COMPROMISED_DATA;
    else Report->ReadOnly=Info->ReadOnly;
  }
  if(Info!=NULL)FreePool(Info);

  // Directory Read is standard EFI_FILE_PROTOCOL, not a fabricated FS mapping.
  UINTN Capacity=4096;EFI_FILE_INFO *Entry=AllocateZeroPool(Capacity);
  if(Entry==NULL){Report->Directory=EFI_OUT_OF_RESOURCES;goto Done;}
  Report->Directory=Root->SetPosition(Root,0);
  if(EFI_ERROR(Report->Directory))goto FreeEntry;
  for(UINTN I=0;I<MAX_DIRECTORY_ENTRIES;++I) {
    Bytes=Capacity;Report->Directory=Root->Read(Root,&Bytes,Entry);
    if(Report->Directory==EFI_BUFFER_TOO_SMALL && Bytes>Capacity && Bytes<=MAX_INFO_BYTES) {
      FreePool(Entry);Capacity=Bytes;Entry=AllocateZeroPool(Capacity);
      if(Entry==NULL){Report->Directory=EFI_OUT_OF_RESOURCES;goto Done;}
      Report->Directory=Root->Read(Root,&Bytes,Entry);
    }
    if(EFI_ERROR(Report->Directory) || Bytes==0)break;
    if(Bytes>Capacity || Bytes<SIZE_OF_EFI_FILE_INFO || Entry->Size>Bytes || Entry->Size<SIZE_OF_EFI_FILE_INFO) {
      Report->Directory=EFI_COMPROMISED_DATA;break;
    }
    UINTN NameCapacity=(Bytes-OFFSET_OF(EFI_FILE_INFO,FileName))/sizeof(CHAR16);
    UINTN NameChars=StrnLenS(Entry->FileName,NameCapacity);
    if(NameChars==0 || NameChars==NameCapacity){Report->Directory=EFI_COMPROMISED_DATA;break;}
    ++Report->Entries;
    if((Entry->Attribute&EFI_FILE_DIRECTORY) || Entry->FileSize==0)continue;
    Report->File=Root->Open(Root,&File,Entry->FileName,EFI_FILE_MODE_READ,0);
    if(EFI_ERROR(Report->File) || File==NULL){if(!EFI_ERROR(Report->File))Report->File=EFI_DEVICE_ERROR;break;}
    UINT8 Data[4096];UINTN Requested=(UINTN)MIN(Entry->FileSize,sizeof(Data));Bytes=Requested;
    Report->File=File->Read(File,&Bytes,Data);
    if(!EFI_ERROR(Report->File) && Bytes>sizeof(Data))Report->File=EFI_COMPROMISED_DATA;
    if(!EFI_ERROR(Report->File) && Bytes!=Requested)Report->File=EFI_BAD_BUFFER_SIZE;
    if(!EFI_ERROR(Report->File)) {
      Report->ReadBytes=Bytes;Report->Crc=PianoGptCrc32(Data,Bytes);
      DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_FILE handle=%p name=%s read_bytes=%lu crc32=%08x mode=read-only\n",
        Report->Handle,Entry->FileName,(UINT64)Bytes,Report->Crc));
    }
    File->Close(File);File=NULL;break;
  }
FreeEntry:
  FreePool(Entry);
Done:
  Root->Close(Root);
}

VOID PianoUfsReportFileSystems(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_REPORT all_sfs=%lu ufs_sfs=%lu recorded=%lu locate=%r fabricated_mappings=0 writes=0\n",
    (UINT64)mAllFileSystems,(UINT64)mUfsFileSystems,(UINT64)mReportCount,mLocateStatus));
  for(UINTN I=0;I<mReportCount;++I) {
    FS_REPORT *R=&mReports[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_VOLUME handle=%p parent_index=%lu open=%r info=%r readonly=%u directory=%r entries=%lu file=%r read_bytes=%lu crc32=%08x\n",
      R->Handle,(UINT64)R->Parent,R->Open,R->Info,R->ReadOnly,R->Directory,(UINT64)R->Entries,R->File,(UINT64)R->ReadBytes,R->Crc));
  }
}

VOID PianoUfsProbeFileSystems(EFI_HANDLE *Parents,UINTN ParentCount) {
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  ZeroMem(mReports,sizeof(mReports));mAllFileSystems=mUfsFileSystems=mReportCount=0;
  mLocateStatus=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(mLocateStatus)){PianoUfsReportFileSystems();return;}
  mAllFileSystems=Count;
  for(UINTN I=0;I<Count;++I) {
    EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiDevicePathProtocolGuid,(VOID **)&Path)))continue;
    UINTN Parent=0;
    for(;Parent<ParentCount;++Parent) {
      EFI_DEVICE_PATH_PROTOCOL *ParentPath=NULL;
      if(!EFI_ERROR(gBS->HandleProtocol(Parents[Parent],&gEfiDevicePathProtocolGuid,(VOID **)&ParentPath)) && Descendant(Path,ParentPath))break;
    }
    if(Parent==ParentCount)continue;
    ++mUfsFileSystems;
    if(mReportCount==MAX_REPORTS)continue;
    FS_REPORT *R=&mReports[mReportCount++];R->Handle=Handles[I];R->Parent=Parent;
    R->Open=R->Info=R->Directory=R->File=EFI_NOT_STARTED;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;EFI_BLOCK_IO_PROTOCOL *Block=NULL;
    R->Open=gBS->HandleProtocol(Handles[I],&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
    // The probe cannot inspect any writable volume, even if another driver
    // accidentally exposes a file system below the same device path.
    EFI_STATUS Status=gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block);
    if(EFI_ERROR(Status) || Block==NULL || Block->Media==NULL || !Block->Media->ReadOnly)R->Open=EFI_ACCESS_DENIED;
    if(!EFI_ERROR(R->Open) && Fs!=NULL)InspectVolume(Fs,R);
  }
  if(Handles!=NULL)FreePool(Handles);PianoUfsReportFileSystems();
}
