// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single product FV entry. No diagnostic timer and no per-UI USB owner.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BootLogoLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PianoProductPumpLib.h>
#include <Guid/EventGroup.h>

STATIC volatile BOOLEAN mExited;
STATIC VOID EFIAPI ExitFence(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;mExited=TRUE;}
STATIC VOID RequireAlive(VOID){if(mExited){CpuDeadLoop();}}

EFI_HANDLE EFIAPI DeviceBootManagerBeforeConsole(EFI_DEVICE_PATH_PROTOCOL **Path,BDS_CONSOLE_CONNECT_ENTRY **Consoles){
  if(Path)*Path=NULL;if(Consoles)*Consoles=NULL;return NULL;
}
EFI_DEVICE_PATH_PROTOCOL **EFIAPI DeviceBootManagerAfterConsole(VOID){
  EFI_STATUS S=BootLogoEnableLogo();
  DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_TIANOCORE_SPLASH status=%r diagnostic_timer=0\n",S));
  return NULL;
}
EFI_DEVICE_PATH_PROTOCOL **EFIAPI DeviceBootManagerOnDemandConInConnect(VOID){return NULL;}
VOID EFIAPI DeviceBootManagerProcessBootCompletion(EFI_BOOT_MANAGER_LOAD_OPTION *Option){(VOID)Option;}
EFI_STATUS EFIAPI DeviceBootManagerPriorityBoot(EFI_BOOT_MANAGER_LOAD_OPTION *Option){(VOID)Option;return EFI_NOT_FOUND;}
VOID EFIAPI DeviceBootManagerBdsEntry(VOID){}
VOID EFIAPI DeviceBootManagerUnableToBoot(VOID){
  EFI_GUID File={0x35E0D1B5,0x93CE,0x4D6A,{0x9A,0x93,0x6A,0xDA,0xA3,0xF2,0x6C,0x40}};
  EFI_EVENT Fence=NULL;VOID *Source=NULL;UINTN Bytes=0;EFI_HANDLE Core=NULL;
  EFI_STATUS S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitFence,NULL,&gEfiEventExitBootServicesGuid,&Fence);
  RequireAlive();if(S!=EFI_SUCCESS || Fence==NULL)CpuDeadLoop();
  S=GetSectionFromAnyFv(&File,EFI_SECTION_PE32,0,&Source,&Bytes);RequireAlive();
  if(S==EFI_SUCCESS && Source!=NULL && Bytes>0){
    S=gBS->LoadImage(FALSE,gImageHandle,NULL,Source,Bytes,&Core);RequireAlive();
    FreePool(Source);Source=NULL;RequireAlive();
    if(S==EFI_SUCCESS && Core!=NULL){S=gBS->StartImage(Core,NULL,NULL);RequireAlive();}
  }
  DEBUG((DEBUG_ERROR,"SUNUEFI_PRODUCT_CORE_RETURN status=%r automatic_reset=0\n",S));
  // A started product core is responsible for retaining its own unsafe state
  // instead of returning. Mu unloads returned applications; do not double-unload.
  while(TRUE){
    PianoProductPumpApplication(PIANO_PRODUCT_PUMP_APP,1000);
    if(!PianoProductPumpBootServicesAlive())CpuDeadLoop();
    gBS->Stall(1000);RequireAlive();
  }
}
