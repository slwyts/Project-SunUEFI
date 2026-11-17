// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute Mu's patched refresh function, including an absent BootOrder.
#define MDEPKG_NDEBUG
#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Protocol/PlatformBootManager.h>
#include <Protocol/DevicePath.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEdkiiPlatformBootManagerProtocolGuid={.Data1=1};
static EFI_GUID mBmAutoCreateBootOptionGuid={.Data1=2};
static VOID (*mBmRefreshLegacyBootOption)(VOID);
static EFI_BOOT_MANAGER_LOAD_OPTION Discovered[2],Existing[1];
static UINTN ExistingCount,Added,Deleted;
static BOOLEAN NullOrder;
UINT8 EFIAPI DevicePathType(CONST VOID *Path){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Path)->Type;}
UINT8 EFIAPI DevicePathSubType(CONST VOID *Path){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Path)->SubType;}

static EFI_BOOT_MANAGER_LOAD_OPTION *BmEnumerateBootOptions(UINTN *Count){
  *Count=2;return Discovered;
}
static BOOLEAN BmIsAutoCreateBootOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option){
  return Option->OptionalDataSize==sizeof(EFI_GUID);
}
VOID *EFIAPI AllocateCopyPool(UINTN Bytes,CONST VOID *Data){
  VOID *Copy=malloc(Bytes);assert(Copy);return memcpy(Copy,Data,Bytes);
}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Count,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){
  assert(Type==LoadOptionTypeBoot);*Count=ExistingCount;return NullOrder?NULL:Existing;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *Options,UINTN Count){
  for(UINTN I=0;I<Count;++I)if(Options==Discovered){free(Options[I].OptionalData);Options[I].OptionalData=NULL;}
  return EFI_SUCCESS;
}
INTN EFIAPI EfiBootManagerFindLoadOption(CONST EFI_BOOT_MANAGER_LOAD_OPTION *Key,
  CONST EFI_BOOT_MANAGER_LOAD_OPTION *Options,UINTN Count){
  for(UINTN I=0;I<Count;++I)if(Key->OptionNumber==Options[I].OptionNumber)return (INTN)I;
  return -1;
}
EFI_STATUS EFIAPI EfiBootManagerDeleteLoadOptionVariable(UINTN Number,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){
  assert(Number==7 && Type==LoadOptionTypeBoot);++Deleted;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerAddLoadOptionVariable(EFI_BOOT_MANAGER_LOAD_OPTION *Option,UINTN Position){
  assert(Position==MAX_UINTN && Option->OptionalDataSize==sizeof(EFI_GUID));
  assert(!memcmp(Option->OptionalData,&mBmAutoCreateBootOptionGuid,sizeof(EFI_GUID)));
  ++Added;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){
  assert(Guid==&gEdkiiPlatformBootManagerProtocolGuid && !Registration && Interface);return EFI_NOT_FOUND;
}
#include "PianoActualBootOptionsRefresh.h"

int main(VOID){
  EFI_BOOT_SERVICES Services={.LocateProtocol=Locate};gBS=&Services;
  EFI_DEVICE_PATH_PROTOCOL Path={HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{4,0}};
  Discovered[0].OptionNumber=1;Discovered[1].OptionNumber=2;
  Existing[0].OptionNumber=7;Existing[0].FilePath=&Path;
  for(UINTN Case=0;Case<4;++Case){
    Added=Deleted=0;NullOrder=Case==0;ExistingCount=Case<2?0:1;
    Existing[0].OptionalDataSize=Case==3?sizeof(EFI_GUID):0;
    EfiBootManagerRefreshAllBootOption();
    assert(Added==2 && Deleted==(Case==3?1U:0U));
  }
  return 0;
}
