// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFvApplication.h"
#include <Guid/GlobalVariable.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

STATIC EFI_STATUS mOptionStatus=EFI_NOT_STARTED;
STATIC UINTN mOptionNumber=LoadOptionNumberUnassigned;
// Use the marker from the linked BootManager library rather than copying its
// GUID or passing its private bookkeeping data to an OS as load options.
extern EFI_GUID mBmAutoCreateBootOptionGuid;

VOID PianoReportLinuxBootOption(VOID) {
  DEBUG((DEBUG_WARN,"PIANO_BOOT_OPTION number=%u status=%r\n",(UINT32)mOptionNumber,mOptionStatus));
}

// Upstream enumerates devices and supplies the architecture's standard
// removable-media path. No loader filename or partition label is a policy.
EFI_STATUS PianoRefreshBootOptions(VOID) {
  // Remove only the retired product FV entry, never disk-installed options.
  STATIC CONST EFI_GUID Retired={0x9ddefc13,0x4746,0x441b,{0x87,0xe9,0x4c,0xd3,0xe1,0x21,0xb0,0x84}};
  UINTN Count=0;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options=EfiBootManagerGetLoadOptions(&Count,LoadOptionTypeBoot);
  for(UINTN I=0;Options && I<Count;++I) {
    EFI_DEVICE_PATH_PROTOCOL *Node=Options[I].FilePath;
    if(!IsDevicePathValid(Node,0))continue;
    while(!IsDevicePathEnd(Node)) {
      if(DevicePathType(Node)==MEDIA_DEVICE_PATH && DevicePathSubType(Node)==MEDIA_PIWG_FW_FILE_DP &&
         DevicePathNodeLength(Node)==sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH) &&
         CompareGuid(&((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Node)->FvFileName,&Retired)) {
        EFI_STATUS Status=EfiBootManagerDeleteLoadOptionVariable(Options[I].OptionNumber,LoadOptionTypeBoot);
        if(EFI_ERROR(Status)){EfiBootManagerFreeLoadOptions(Options,Count);return Status;}
        break;
      }
      Node=NextDevicePathNode(Node);
    }
  }
  EfiBootManagerFreeLoadOptions(Options,Count);
  EfiBootManagerRefreshAllBootOption();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS RunOption(EFI_HANDLE Parent,EFI_BOOT_MANAGER_LOAD_OPTION *Option,
                           EFI_HANDLE *Device,PIANO_FV_APPLICATION *Application,BOOLEAN BootNext) {
  // BootNext is an explicit one-time choice; ACTIVE filters BootOrder only.
  if(!BootNext && (!(Option->Attributes&LOAD_OPTION_ACTIVE) ||
     (Option->Attributes&LOAD_OPTION_CATEGORY)!=LOAD_OPTION_CATEGORY_BOOT))
    return EFI_NOT_FOUND;
  if(!Option->FilePath || !IsDevicePathValid(Option->FilePath,0))return EFI_COMPROMISED_DATA;
  EfiBootManagerConnectDevicePath(Option->FilePath,NULL);
  EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
  EFI_STATUS Status=EFI_NOT_FOUND;
  for(UINTN Attempt=0;Attempt<256;++Attempt) {
    // Expand short-form GPT/file paths and device-only fallback options.
    EFI_DEVICE_PATH_PROTOCOL *Next=EfiBootManagerGetNextLoadOptionDevicePath(Option->FilePath,Path);
    if(Path)FreePool(Path);
    Path=Next;
    if(!Path)break;
    EFI_DEVICE_PATH_PROTOCOL *Remaining=Path;
    EFI_HANDLE Volume=NULL;
    Status=gBS->LocateDevicePath(&gEfiSimpleFileSystemProtocolGuid,&Remaining,&Volume);
    if(Status!=EFI_SUCCESS || !Volume)continue;
    *Device=Volume;mOptionNumber=Option->OptionNumber;
    UINT16 Current=(UINT16)Option->OptionNumber;
    Status=gRT->SetVariable(L"BootCurrent",&gEfiGlobalVariableGuid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Current),&Current);
    if(Status!=EFI_SUCCESS)break;
    EfiSignalEventReadyToBoot();
    BOOLEAN Automatic=Option->OptionalDataSize==sizeof(EFI_GUID) && Option->OptionalData &&
      CompareGuid((CONST EFI_GUID *)(CONST VOID *)Option->OptionalData,&mBmAutoCreateBootOptionGuid);
    Status=PianoApplicationRunPath(Parent,Volume,Path,Automatic?NULL:Option->OptionalData,
      Automatic?0:Option->OptionalDataSize,0x10000000,Application);
    if(Application->Retained || Application->EbsObserved){FreePool(Path);return Status;}
    EFI_STATUS Clear=gRT->SetVariable(L"BootCurrent",&gEfiGlobalVariableGuid,0,0,NULL);
    if(Clear!=EFI_SUCCESS && Clear!=EFI_NOT_FOUND){Status=Clear;break;}
    if(Application->StartCalled)break;
    *Device=NULL;
  }
  if(Path)FreePool(Path);
  return mOptionStatus=Status;
}

EFI_STATUS PianoStartBootOption(EFI_HANDLE Parent,EFI_HANDLE *Device,PIANO_FV_APPLICATION *Application) {
  if(!Parent || !Device || !Application)return EFI_INVALID_PARAMETER;
  *Device=NULL;
  UINT16 Next=0;UINTN Bytes=sizeof(Next);
  EFI_STATUS Status=gRT->GetVariable(L"BootNext",&gEfiGlobalVariableGuid,NULL,&Bytes,&Next);
  if(Status==EFI_SUCCESS && Bytes==sizeof(Next)) {
    // Consume BootNext before attempting it, including failure paths.
    Status=gRT->SetVariable(L"BootNext",&gEfiGlobalVariableGuid,0,0,NULL);
    if(Status!=EFI_SUCCESS)return mOptionStatus=Status;
    CHAR16 Name[9];
    UnicodeSPrint(Name,sizeof(Name),L"Boot%04x",Next);
    EFI_BOOT_MANAGER_LOAD_OPTION Option;
    Status=EfiBootManagerVariableToLoadOption(Name,&Option);
    if(Status==EFI_SUCCESS) {
      Status=RunOption(Parent,&Option,Device,Application,TRUE);
      EfiBootManagerFreeLoadOption(&Option);
      if(Application->StartCalled || Application->Retained)return Status;
    }
  } else if(Status!=EFI_NOT_FOUND) {
    return mOptionStatus=Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status;
  }
  UINTN Count=0;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options=EfiBootManagerGetLoadOptions(&Count,LoadOptionTypeBoot);
  if(Count>1024 || (Count&&!Options)) {
    if(Options)EfiBootManagerFreeLoadOptions(Options,Count);
    return mOptionStatus=EFI_COMPROMISED_DATA;
  }
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    Status=RunOption(Parent,&Options[I],Device,Application,FALSE);
    if(Application->StartCalled || Application->Retained)break;
  }
  EfiBootManagerFreeLoadOptions(Options,Count);
  return mOptionStatus=Status;
}
