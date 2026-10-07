// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the genuine loader source; no UiApp GUI or hardware is emulated.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <PiDxe.h>
static BOOLEAN emu_enabled=TRUE;
#define _PCD_GET_MODE_BOOL_PcdEmuVariableNvModeEnable emu_enabled
#include "../../uefi/core/PianoLaunchSetup.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiFirmwareVolume2ProtocolGuid,gEfiLoadedImageProtocolGuid;
EFI_GUID gEfiHiiDatabaseProtocolGuid,gEfiHiiStringProtocolGuid,gEfiHiiFontProtocolGuid,gEfiHiiConfigRoutingProtocolGuid;
EFI_GUID gEfiFormBrowser2ProtocolGuid,gEdkiiFormDisplayEngineProtocolGuid,gEfiVariableArchProtocolGuid,gEfiVariableWriteArchProtocolGuid;
static EFI_BOOT_SERVICES bs;
static EFI_FIRMWARE_VOLUME2_PROTOCOL fv;
static EFI_LOADED_IMAGE_PROTOCOL loaded;
static EFI_DEVICE_PATH_PROTOCOL base_path[]={{4,7,{4,0}},{0x7f,0xff,{4,0}}};
static unsigned allocations,queries,loads,starts,unloads,prints;
static EFI_GUID *missing,*null_service;
static BOOLEAN no_volumes,no_path,empty_section;
static EFI_STATUS load_status,start_status;
static BOOLEAN standard_navigation;
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable){BOOLEAN Previous=standard_navigation;standard_navigation=Enable;return Previous;}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){++allocations;return malloc(N);}
VOID EFIAPI FreePool(VOID *P){assert(P && allocations);--allocations;free(P);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
UINTN EFIAPI Print(CONST CHAR16 *Format,...){assert(Format);++prints;return 1;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(EFI_HANDLE Handle){assert(Handle==(void *)2);return no_path?NULL:base_path;}
UINT16 EFIAPI SetDevicePathNodeLength(VOID *Node,UINTN N){EFI_DEVICE_PATH_PROTOCOL *P=Node;P->Length[0]=(UINT8)N;P->Length[1]=(UINT8)(N>>8);return (UINT16)N;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(CONST EFI_DEVICE_PATH_PROTOCOL *Base,CONST EFI_DEVICE_PATH_PROTOCOL *Node){
  assert(Base==base_path && Node->Type==MEDIA_DEVICE_PATH && Node->SubType==MEDIA_PIWG_FW_FILE_DP);
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *File=(void *)Node;assert(memcmp(&File->FvFileName,&mUiAppFile,sizeof(EFI_GUID))==0);
  UINTN N=sizeof(*File);UINT8 *P=AllocatePool(4+N+4);memcpy(P,Base,4);memcpy(P+4,Node,N);memcpy(P+4+N,Base+1,4);return (void *)P;
}
static EFI_STATUS EFIAPI locate_protocol(EFI_GUID *Guid,VOID *Registration,VOID **Protocol){
  ++queries;if(Guid==missing)return EFI_NOT_FOUND;
  *Protocol=Guid==null_service || Guid==&gEfiVariableArchProtocolGuid || Guid==&gEfiVariableWriteArchProtocolGuid?NULL:(void *)99;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate_volumes(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles){
  assert(Type==ByProtocol && Guid==&gEfiFirmwareVolume2ProtocolGuid);
  if(no_volumes)return EFI_NOT_FOUND;
  *Count=1;*Handles=AllocatePool(sizeof(EFI_HANDLE));(*Handles)[0]=(void *)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Protocol){
  if(Guid==&gEfiFirmwareVolume2ProtocolGuid){assert(Handle==(void *)2);*Protocol=&fv;return EFI_SUCCESS;}
  assert(Handle==(void *)3 && Guid==&gEfiLoadedImageProtocolGuid);*Protocol=&loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI section(CONST EFI_FIRMWARE_VOLUME2_PROTOCOL *This,CONST EFI_GUID *Guid,EFI_SECTION_TYPE Type,UINTN Instance,VOID **Source,UINTN *Bytes,UINT32 *Authentication){
  assert(This==&fv && Type==EFI_SECTION_PE32 && Instance==0 && !memcmp(Guid,&mUiAppFile,sizeof(EFI_GUID)));
  *Source=AllocatePool(64);memset(*Source,0,64);*Bytes=empty_section?0:64;*Authentication=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI load_image(BOOLEAN BootPolicy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Image){
  assert(!BootPolicy && Parent==(void *)1 && Path && Path->Type==MEDIA_DEVICE_PATH && Source && Bytes==64);
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *File=(void *)((UINT8 *)Path+4);assert(!memcmp(&File->FvFileName,&mUiAppFile,sizeof(EFI_GUID)));
  ++loads;*Image=(void *)3;return load_status;
}
static EFI_STATUS EFIAPI start_image(EFI_HANDLE Image,UINTN *ExitDataSize,CHAR16 **ExitData){
  assert(standard_navigation && mSetupStartCalled && !mSetupStartReturned);
  assert(Image==(void *)3 && loaded.ImageCodeType==EfiLoaderCode && loaded.LoadOptions==NULL && !loaded.LoadOptionsSize);
  ++starts;return start_status;
}
static EFI_STATUS EFIAPI unload_image(EFI_HANDLE Image){assert(Image==(void *)3 && !starts);++unloads;return EFI_SUCCESS;}
static VOID initialize(VOID){
  queries=loads=starts=unloads=prints=0;assert(!allocations);missing=null_service=NULL;
  no_volumes=no_path=empty_section=FALSE;load_status=start_status=EFI_SUCCESS;emu_enabled=TRUE;loaded.ImageCodeType=EfiLoaderCode;
}
int main(void){
  gBS=&bs;bs.LocateProtocol=locate_protocol;bs.LocateHandleBuffer=locate_volumes;bs.HandleProtocol=handle_protocol;
  bs.LoadImage=load_image;bs.StartImage=start_image;bs.UnloadImage=unload_image;fv.ReadSection=section;
  initialize();emu_enabled=FALSE;assert(PianoLaunchSetup((void *)1)==EFI_ACCESS_DENIED && !queries && !loads);
  initialize();missing=&gEfiFormBrowser2ProtocolGuid;assert(PianoLaunchSetup((void *)1)==EFI_NOT_FOUND && queries==8 && !loads);
  initialize();null_service=&gEfiHiiFontProtocolGuid;assert(PianoLaunchSetup((void *)1)==EFI_DEVICE_ERROR && !loads);
  initialize();no_volumes=TRUE;assert(PianoLaunchSetup((void *)1)==EFI_NOT_FOUND && !loads && !allocations);
  initialize();no_path=TRUE;assert(PianoLaunchSetup((void *)1)==EFI_NOT_FOUND && !loads && !allocations);
  initialize();empty_section=TRUE;assert(PianoLaunchSetup((void *)1)==EFI_COMPROMISED_DATA && !loads && !allocations);
  initialize();load_status=EFI_SECURITY_VIOLATION;assert(PianoLaunchSetup((void *)1)==EFI_SECURITY_VIOLATION && loads==1 && !starts && !allocations);
  initialize();loaded.ImageCodeType=EfiBootServicesCode;assert(PianoLaunchSetup((void *)1)==EFI_UNSUPPORTED && unloads==1 && !starts && !allocations);
  initialize();start_status=EFI_ABORTED;assert(PianoLaunchSetup((void *)1)==EFI_ABORTED && starts==1 && !unloads && prints==2 && !allocations);
  initialize();assert(PianoLaunchSetup((void *)1)==EFI_SUCCESS && loads==1 && starts==1 && !unloads && prints==2 && !allocations);
  assert(mSetupRequested && mSetupServices==EFI_SUCCESS && mSetupLoad==EFI_SUCCESS && mSetupStartReturned && mSetupStart==EFI_SUCCESS && !standard_navigation);
  puts("Pinned UiApp loader: RAM backend gate, real HII prerequisites/marker protocols, FV file path, load/type/start errors and allocation cleanup passed.");
}
