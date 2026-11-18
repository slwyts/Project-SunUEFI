// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoBootOptions.h"
#include "PianoNvFile.h"
#ifndef PIANO_SIMPLEINIT_BOOT_GUID_HEADER
#define PIANO_SIMPLEINIT_BOOT_GUID_HEADER "PianoSimpleInitBootGuid.h"
#endif
#include PIANO_SIMPLEINIT_BOOT_GUID_HEADER
#include <PiDxe.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

STATIC EFI_STATUS mOptionStatus=EFI_NOT_STARTED;
STATIC UINTN mOptionNumber=LoadOptionNumberUnassigned;
STATIC EFI_DEVICE_PATH_PROTOCOL *mSimpleInitPath;
// Use the marker from the linked BootManager library rather than copying its
// GUID or passing its private bookkeeping data to an OS as load options.
extern EFI_GUID mBmAutoCreateBootOptionGuid;

VOID PianoReportLinuxBootOption(VOID) {
  DEBUG((DEBUG_WARN,"PIANO_BOOT_OPTION number=%u status=%r\n",(UINT32)mOptionNumber,mOptionStatus));
}

STATIC BOOLEAN IsAutomatic(CONST EFI_BOOT_MANAGER_LOAD_OPTION *Option) {
  return Option->OptionalDataSize==sizeof(EFI_GUID) && Option->OptionalData &&
    CompareGuid((CONST EFI_GUID *)Option->OptionalData,&mBmAutoCreateBootOptionGuid);
}

BOOLEAN PianoBootOptionIsSimpleInit(CONST EFI_DEVICE_PATH_PROTOCOL *Path) {
  if(!mSimpleInitPath || !Path)return FALSE;
  UINTN Bytes=GetDevicePathSize(mSimpleInitPath);
  return Bytes && GetDevicePathSize(Path)==Bytes && !CompareMem(mSimpleInitPath,Path,Bytes);
}

STATIC BOOLEAN IsFvFile(CONST EFI_DEVICE_PATH_PROTOCOL *Path,CONST EFI_GUID *File) {
  return DevicePathType(Path)==MEDIA_DEVICE_PATH &&
    DevicePathSubType(Path)==MEDIA_PIWG_FW_FILE_DP &&
    DevicePathNodeLength(Path)==sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH) &&
    CompareGuid(&((CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Path)->FvFileName,File) &&
    IsDevicePathEnd(NextDevicePathNode(Path));
}

STATIC BOOLEAN IsBootNumber(CONST CHAR16 *Name,UINTN Bytes) {
  if(Bytes!=9*sizeof(CHAR16) || Name[8] || Name[0]!=L'B' || Name[1]!=L'o' ||
     Name[2]!=L'o' || Name[3]!=L't')return FALSE;
  for(UINTN I=4;I<8;++I) {
    CHAR16 C=Name[I];
    if(!((C>=L'0'&&C<=L'9') || (C>=L'a'&&C<=L'f') || (C>=L'A'&&C<=L'F')))return FALSE;
  }
  return TRUE;
}

STATIC EFI_STATUS HaveSimpleInitVariable(BOOLEAN *Found) {
  CHAR16 Name[1024]={0};
  EFI_GUID Vendor={0};
  *Found=FALSE;
  // Moving an entry out of BootOrder is a user choice, not a missing entry.
  for(UINTN I=0;I<4096;++I) {
    UINTN Bytes=sizeof(Name);
    EFI_STATUS Status=gRT->GetNextVariableName(&Bytes,Name,&Vendor);
    if(Status==EFI_NOT_FOUND)return EFI_SUCCESS;
    if(Status!=EFI_SUCCESS)return Status;
    if(!CompareGuid(&Vendor,&gEfiGlobalVariableGuid) || !IsBootNumber(Name,Bytes))continue;
    EFI_BOOT_MANAGER_LOAD_OPTION Option;
    Status=EfiBootManagerVariableToLoadOption(Name,&Option);
    if(Status!=EFI_SUCCESS)continue;
    *Found=PianoBootOptionIsSimpleInit(Option.FilePath);
    EfiBootManagerFreeLoadOption(&Option);
    if(*Found)return EFI_SUCCESS;
  }
  return EFI_OUT_OF_RESOURCES;
}

STATIC EFI_STATUS RegisterSimpleInit(BOOLEAN UserOrder) {
  if(!mSimpleInitPath) {
    EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
    EFI_STATUS Status=GetFileDevicePathFromAnyFv(&mPianoSimpleInitBootFileGuid,EFI_SECTION_PE32,0,&Path);
    if(Status!=EFI_SUCCESS)return Status;
    if(!Path)return EFI_NOT_FOUND;
    if(!IsDevicePathValid(Path,0) || DevicePathType(Path)!=MEDIA_DEVICE_PATH ||
       DevicePathSubType(Path)!=MEDIA_PIWG_FW_VOL_DP ||
       DevicePathNodeLength(Path)!=sizeof(MEDIA_FW_VOL_DEVICE_PATH) ||
       !IsFvFile(NextDevicePathNode(Path),&mPianoSimpleInitBootFileGuid)) {
      FreePool(Path);
      return EFI_UNSUPPORTED;
    }
    mSimpleInitPath=Path;
  }
  BOOLEAN Found;
  EFI_STATUS Status=HaveSimpleInitVariable(&Found);
  if(Status!=EFI_SUCCESS || Found)return Status;
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  Status=EfiBootManagerInitializeLoadOption(&Option,LoadOptionNumberUnassigned,LoadOptionTypeBoot,
    LOAD_OPTION_ACTIVE,L"SimpleInit",mSimpleInitPath,NULL,0);
  if(Status!=EFI_SUCCESS)return Status;
  Status=EfiBootManagerAddLoadOptionVariable(&Option,UserOrder?MAX_UINTN:0);
  EfiBootManagerFreeLoadOption(&Option);
  return Status;
}

// Upstream enumerates devices and supplies the architecture's standard
// removable-media path. No loader filename or partition label is a policy.
EFI_STATUS PianoRefreshBootOptions(VOID) {
  // Remove only the retired product FV entry, never disk-installed options.
  STATIC CONST EFI_GUID Retired={0x9ddefc13,0x4746,0x441b,{0x87,0xe9,0x4c,0xd3,0xe1,0x21,0xb0,0x84}};
  UINTN Count=0;
  BOOLEAN UserOrder=FALSE;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options=EfiBootManagerGetLoadOptions(&Count,LoadOptionTypeBoot);
  for(UINTN I=0;Options && I<Count;++I) {
    EFI_DEVICE_PATH_PROTOCOL *Path=Options[I].FilePath;
    if(!Path || !IsDevicePathValid(Path,0) || IsDevicePathEnd(Path))continue;
    BOOLEAN FirmwareVolume=(DevicePathType(Path)==MEDIA_DEVICE_PATH && DevicePathSubType(Path)==MEDIA_PIWG_FW_VOL_DP)
      || (DevicePathType(Path)==HARDWARE_DEVICE_PATH && DevicePathSubType(Path)==HW_MEMMAP_DP);
    if(FirmwareVolume && IsFvFile(NextDevicePathNode(Path),&Retired)) {
      EFI_STATUS Status=EfiBootManagerDeleteLoadOptionVariable(Options[I].OptionNumber,LoadOptionTypeBoot);
      if(EFI_ERROR(Status)){EfiBootManagerFreeLoadOptions(Options,Count);return Status;}
    } else if(!IsAutomatic(&Options[I])) {
      UserOrder=TRUE;
    }
  }
  EfiBootManagerFreeLoadOptions(Options,Count);
  EFI_STATUS Status=RegisterSimpleInit(UserOrder);
  DEBUG((DEBUG_INFO,"PIANO_SIMPLEINIT_BOOT_OPTION status=%r\n",Status));
  EfiBootManagerRefreshAllBootOption();
  return Status;
}

typedef struct {
  BOOLEAN TryNext;
  UINTN ReturnedNumber;
  EFI_STATUS ReturnedError;
} PIANO_BOOT_ATTEMPTS;

STATIC BOOLEAN CanTryNext(CONST PIANO_FV_APPLICATION *Application,
                         CONST PIANO_BOOT_DISPATCH *Dispatch,EFI_STATUS Status) {
  return Dispatch->Selection!=PianoBootSelected && Dispatch->CanTryNextSystem &&
    Application->StartReturned && EFI_ERROR(Application->Start) &&
    Application->Start==Status && Application->Cleanup==EFI_SUCCESS &&
    !Application->Busy && Dispatch->CanTryNextSystem(Status);
}

STATIC EFI_STATUS RunOption(EFI_HANDLE Parent,EFI_BOOT_MANAGER_LOAD_OPTION *Option,
                           EFI_HANDLE *Device,PIANO_FV_APPLICATION *Application,
                           PIANO_BOOT_DISPATCH *Dispatch,BOOLEAN Explicit,
                           PIANO_BOOT_ATTEMPTS *Attempts) {
  Attempts->TryNext=TRUE;
  // An explicit one-time choice is valid independently of its order flag.
  if(!Explicit && (!(Option->Attributes&LOAD_OPTION_ACTIVE) ||
     (Option->Attributes&LOAD_OPTION_CATEGORY)!=LOAD_OPTION_CATEGORY_BOOT))
    return EFI_NOT_FOUND;
  if(Option->OptionNumber>MAX_UINT16 || !Option->FilePath ||
     !IsDevicePathValid(Option->FilePath,0))return EFI_COMPROMISED_DATA;
  BOOLEAN Menu=PianoBootOptionIsSimpleInit(Option->FilePath);
  if(Menu && Dispatch->Selection==PianoBootSystems)return EFI_NOT_FOUND;
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
    Status=gBS->LocateDevicePath(Menu?&gEfiFirmwareVolume2ProtocolGuid:&gEfiSimpleFileSystemProtocolGuid,
                                 &Remaining,&Volume);
    if(Status!=EFI_SUCCESS)continue;
    if(!Volume){Status=EFI_NOT_FOUND;continue;}
    if(Menu && (!PianoBootOptionIsSimpleInit(Path) || !IsFvFile(Remaining,&mPianoSimpleInitBootFileGuid)))continue;
    if(!Menu && Dispatch->PrepareSystem) {
      Status=Dispatch->PrepareSystem();
      if(Status!=EFI_SUCCESS){
        Dispatch->SystemBlocked=TRUE;
        Attempts->TryNext=FALSE;
        break;
      }
    }
    *Device=Menu?NULL:Volume;
    mOptionNumber=Option->OptionNumber;
    UINT16 Current=(UINT16)Option->OptionNumber;
    Status=gRT->SetVariable(L"BootCurrent",&gEfiGlobalVariableGuid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Current),&Current);
    if(Status!=EFI_SUCCESS){
      Attempts->TryNext=FALSE;
      break;
    }
    EfiSignalEventReadyToBoot();
    BOOLEAN Automatic=IsAutomatic(Option);
    Application->StartCalled=FALSE;
    Status=PianoApplicationRunPath(Parent,Volume,Path,Automatic?NULL:Option->OptionalData,
      Automatic?0:Option->OptionalDataSize,0x10000000,Application);
    if(Application->StartCalled){
      if(Menu)Dispatch->StartedMenu=TRUE;
      else Dispatch->StartedSystem=TRUE;
    }
    if(Application->Retained || Application->EbsObserved){
      Attempts->TryNext=FALSE;
      return Status;
    }
    EFI_STATUS Clear=gRT->SetVariable(L"BootCurrent",&gEfiGlobalVariableGuid,0,0,NULL);
    if(Clear!=EFI_SUCCESS && Clear!=EFI_NOT_FOUND){
      Status=Clear;
      Attempts->TryNext=FALSE;
      break;
    }
    if(Application->StartCalled){
      // Try another boot option, not another expansion of this device path.
      Attempts->TryNext=!Menu && CanTryNext(Application,Dispatch,Status);
      if(Attempts->TryNext){
        Attempts->ReturnedNumber=Option->OptionNumber;
        Attempts->ReturnedError=Status;
        DEBUG((DEBUG_WARN,"PIANO_BOOT_OPTION_RETURN number=%04x status=%r trying_next=1\n",Current,Status));
        *Device=NULL;
      }
      break;
    }
    *Device=NULL;
  }
  if(Path)FreePool(Path);
  return Status;
}

STATIC EFI_STATUS RunNumber(EFI_HANDLE Parent,UINT16 Number,EFI_HANDLE *Device,
  PIANO_FV_APPLICATION *Application,PIANO_BOOT_DISPATCH *Dispatch,PIANO_BOOT_ATTEMPTS *Attempts) {
  Attempts->TryNext=TRUE;
  CHAR16 Name[9];
  UnicodeSPrint(Name,sizeof(Name),L"Boot%04x",Number);
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  EFI_STATUS Status=EfiBootManagerVariableToLoadOption(Name,&Option);
  if(Status==EFI_SUCCESS) {
    Status=RunOption(Parent,&Option,Device,Application,Dispatch,TRUE,Attempts);
    if(!Application->EbsObserved)EfiBootManagerFreeLoadOption(&Option);
  }
  return Status;
}

EFI_STATUS PianoStartBootOption(EFI_HANDLE Parent,EFI_HANDLE *Device,
  PIANO_FV_APPLICATION *Application,PIANO_BOOT_DISPATCH *Dispatch) {
  if(!Parent || !Device || !Application || !Dispatch || (UINTN)Dispatch->Selection>PianoBootSelected)
    return EFI_INVALID_PARAMETER;
  if(Application->Busy || Application->Retained)return EFI_ALREADY_STARTED;
  Application->StartCalled=FALSE;
  *Device=NULL;
  Dispatch->StartedMenu=Dispatch->StartedSystem=Dispatch->SystemBlocked=FALSE;
  mOptionNumber=LoadOptionNumberUnassigned;
  PIANO_BOOT_ATTEMPTS Attempts={.TryNext=TRUE};
  if(Dispatch->Selection==PianoBootSelected)
    return mOptionStatus=RunNumber(Parent,Dispatch->Number,Device,Application,Dispatch,&Attempts);

  UINT16 Next=0;
  BOOLEAN TriedNext=FALSE;
  EFI_STATUS Status=EFI_NOT_FOUND;
  if(Dispatch->Selection==PianoBootAutomatic) {
    UINTN Bytes=sizeof(Next);
    Status=gRT->GetVariable(L"BootNext",&gEfiGlobalVariableGuid,NULL,&Bytes,&Next);
    if(Status!=EFI_NOT_FOUND && (Status!=EFI_SUCCESS || Bytes!=sizeof(Next)))
      return mOptionStatus=Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status;
  }
  if(Status==EFI_SUCCESS) {
    // Persist deletion before loading the once-only target, even if it hangs.
    Status=gRT->SetVariable(L"BootNext",&gEfiGlobalVariableGuid,0,0,NULL);
    if(Status!=EFI_SUCCESS)return mOptionStatus=Status;
    Status=PianoNvFileSave(FALSE);
    if(Status!=EFI_SUCCESS && Status!=EFI_NOT_STARTED)return mOptionStatus=Status;
    Status=RunNumber(Parent,Next,Device,Application,Dispatch,&Attempts);
    TriedNext=TRUE;
    if(!Attempts.TryNext)return mOptionStatus=Status;
  }
  UINTN Count=0;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options=EfiBootManagerGetLoadOptions(&Count,LoadOptionTypeBoot);
  if(Count>1024 || (Count&&!Options)) {
    if(Options)EfiBootManagerFreeLoadOptions(Options,Count);
    return mOptionStatus=EFI_COMPROMISED_DATA;
  }
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    if(TriedNext && Options[I].OptionNumber==Next)continue;
    Status=RunOption(Parent,&Options[I],Device,Application,Dispatch,FALSE,&Attempts);
    if(!Attempts.TryNext)break;
  }
  if(!Application->EbsObserved)EfiBootManagerFreeLoadOptions(Options,Count);
  if(Attempts.TryNext && EFI_ERROR(Attempts.ReturnedError)){
    mOptionNumber=Attempts.ReturnedNumber;
    Status=Attempts.ReturnedError;
  }
  return mOptionStatus=Status;
}
