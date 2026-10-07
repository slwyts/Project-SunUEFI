// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only: exercise the actual SFS probe with mocked UEFI protocols.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoGpt.c"
#include "../../uefi/core/PianoUfsFileSystemProbe.c"

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid,gEfiSimpleFileSystemProtocolGuid;
EFI_GUID gEfiFileInfoGuid,gEfiFileSystemInfoGuid;
static EFI_BOOT_SERVICES bs;
static EFI_FILE_PROTOCOL root,file;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_BLOCK_IO_MEDIA media={.ReadOnly=TRUE};
static EFI_BLOCK_IO_PROTOCOL block={.Media=&media};
static EFI_DEVICE_PATH_PROTOCOL parent_path[]={{3,0x19,{4,0}},{0x7f,0xff,{4,0}}};
static EFI_DEVICE_PATH_PROTOCOL child_path[]={{3,0x19,{4,0}},{4,1,{4,0}},{0x7f,0xff,{4,0}}};
static EFI_DEVICE_PATH_PROTOCOL foreign_path[]={{3,0x20,{4,0}},{0x7f,0xff,{4,0}}};
static EFI_HANDLE parent=(void *)1,child=(void *)2,foreign=(void *)3;
static unsigned opened,closed,read_calls,allocations;
static BOOLEAN no_sfs,bad_info,bad_directory;

BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N && S[I])++I;return I;}
VOID *EFIAPI AllocateZeroPool(UINTN N){++allocations;return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){assert(P && allocations);--allocations;free(P);}

static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles){
  assert(Type==ByProtocol && Guid==&gEfiSimpleFileSystemProtocolGuid);
  if(no_sfs)return EFI_NOT_FOUND;
  *Count=2;*Handles=AllocateZeroPool(2*sizeof(EFI_HANDLE));(*Handles)[0]=child;(*Handles)[1]=foreign;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE H,EFI_GUID *Guid,VOID **Interface){
  if(Guid==&gEfiDevicePathProtocolGuid){*Interface=H==parent?(void *)parent_path:H==child?(void *)child_path:(void *)foreign_path;return EFI_SUCCESS;}
  assert(H==child);
  if(Guid==&gEfiSimpleFileSystemProtocolGuid){*Interface=&fs;return EFI_SUCCESS;}
  if(Guid==&gEfiBlockIoProtocolGuid){*Interface=&block;return EFI_SUCCESS;}
  assert(!"Unexpected protocol");return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Root){assert(This==&fs);++opened;*Root=&root;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI info(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN *Bytes,VOID *Data){
  assert(This==&root && Guid==&gEfiFileSystemInfoGuid);
  UINTN N=SIZE_OF_EFI_FILE_SYSTEM_INFO+sizeof(CHAR16);
  if(Data==NULL){*Bytes=N;return EFI_BUFFER_TOO_SMALL;}
  assert(*Bytes==N);EFI_FILE_SYSTEM_INFO *I=Data;I->Size=bad_info?N+1:N;I->ReadOnly=TRUE;*Bytes=N;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI position(EFI_FILE_PROTOCOL *This,UINT64 Position){assert(This==&root && Position==0);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI read_file(EFI_FILE_PROTOCOL *This,UINTN *Bytes,VOID *Data){
  ++read_calls;
  if(This==&file){assert(*Bytes==3);memcpy(Data,"abc",3);return EFI_SUCCESS;}
  assert(This==&root && *Bytes>=sizeof(EFI_FILE_INFO)+20);
  EFI_FILE_INFO *E=Data;UINTN N=SIZE_OF_EFI_FILE_INFO+20;
  E->Size=bad_directory?N+1:N;E->FileSize=3;memcpy(E->FileName,L"probe.efi",20);*Bytes=N;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI open_file(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **File,CHAR16 *Name,UINT64 Mode,UINT64 Attributes){
  assert(This==&root && Mode==EFI_FILE_MODE_READ && Attributes==0 && Name[0]=='p');++opened;*File=&file;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_file(EFI_FILE_PROTOCOL *This){assert(This==&root || This==&file);++closed;return EFI_SUCCESS;}
int main(void){
  gBS=&bs;bs.LocateHandleBuffer=locate;bs.HandleProtocol=handle;fs.OpenVolume=volume;
  root.GetInfo=info;root.SetPosition=position;root.Read=read_file;root.Open=open_file;root.Close=close_file;file.Read=read_file;file.Close=close_file;
  assert(Descendant(child_path,parent_path) && !Descendant(foreign_path,parent_path));
  no_sfs=TRUE;PianoUfsProbeFileSystems(&parent,1);assert(mLocateStatus==EFI_NOT_FOUND && mUfsFileSystems==0 && !opened && !allocations);
  no_sfs=FALSE;PianoUfsProbeFileSystems(&parent,1);
  assert(mAllFileSystems==2 && mUfsFileSystems==1 && mReportCount==1);
  assert(mReports[0].Open==EFI_SUCCESS && mReports[0].Info==EFI_SUCCESS && mReports[0].ReadOnly);
  assert(mReports[0].Directory==EFI_SUCCESS && mReports[0].Entries==1 && mReports[0].File==EFI_SUCCESS);
  assert(mReports[0].ReadBytes==3 && mReports[0].Crc==0x352441c2 && opened==2 && closed==2 && !allocations);
  media.ReadOnly=FALSE;PianoUfsProbeFileSystems(&parent,1);assert(mReports[0].Open==EFI_ACCESS_DENIED && opened==2 && read_calls==2 && !allocations);
  media.ReadOnly=TRUE;bad_info=TRUE;PianoUfsProbeFileSystems(&parent,1);assert(mReports[0].Info==EFI_COMPROMISED_DATA && opened==closed && !allocations);
  bad_info=FALSE;bad_directory=TRUE;PianoUfsProbeFileSystems(&parent,1);assert(mReports[0].Directory==EFI_COMPROMISED_DATA && mReports[0].File==EFI_NOT_STARTED && opened==closed && !allocations);
  puts("Real SFS probe: UFS ancestry, honest absent-volume result, writable-volume rejection, OpenVolume/GetInfo/directory/file read+CRC, malformed metadata and handle cleanup passed.");
}
