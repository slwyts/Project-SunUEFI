// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single product FV entry. No diagnostic timer and no per-UI USB owner.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PianoProductPumpLib.h>
#include <Guid/EventGroup.h>
#include "ProductSplash.h"

STATIC volatile BOOLEAN mExited;
STATIC EFI_EVENT mExitFence;
STATIC BOOLEAN mFenceAttempted;
STATIC EFI_STATUS mFenceStatus;
STATIC VOID EFIAPI ExitFence(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;mExited=TRUE;}
STATIC VOID RequireAlive(VOID){if(mExited){CpuDeadLoop();}}
STATIC BOOLEAN EFIAPI SplashAlive(VOID){return !mExited;}
STATIC EFI_STATUS EnsureFence(VOID){
  if(mExited)return EFI_ABORTED;
  if(mFenceAttempted)return mFenceStatus;
  mFenceAttempted=TRUE;
  mFenceStatus=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitFence,NULL,&gEfiEventExitBootServicesGuid,&mExitFence);
  RequireAlive();
  if(mFenceStatus==EFI_SUCCESS&&mExitFence==NULL)mFenceStatus=EFI_DEVICE_ERROR;
  if(mFenceStatus!=EFI_SUCCESS&&!EFI_ERROR(mFenceStatus))mFenceStatus=EFI_DEVICE_ERROR;
  return mFenceStatus;
}

EFI_HANDLE EFIAPI DeviceBootManagerBeforeConsole(EFI_DEVICE_PATH_PROTOCOL **Path,BDS_CONSOLE_CONNECT_ENTRY **Consoles){
  if(Path)*Path=NULL;if(Consoles)*Consoles=NULL;return NULL;
}
EFI_DEVICE_PATH_PROTOCOL **EFIAPI DeviceBootManagerAfterConsole(VOID){
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;PIANO_SPLASH_REPORT Report={0};
  EFI_STATUS S=EnsureFence();
  if(S==EFI_SUCCESS){S=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);RequireAlive();}
  if(S==EFI_SUCCESS)S=PianoProductDrawSplash(Gop,SplashAlive,&Report);
  RequireAlive();
  DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_SPLASH status=%r backend=gop-blt vector=1 width=%u height=%u logo=%u calls=%u hint_drawn=%u diagnostic_timer=0\n",
    S,Report.Width,Report.Height,Report.LogoSize,Report.DrawCalls,(UINT32)(S==EFI_SUCCESS)));
  return NULL;
}
EFI_DEVICE_PATH_PROTOCOL **EFIAPI DeviceBootManagerOnDemandConInConnect(VOID){return NULL;}
VOID EFIAPI DeviceBootManagerProcessBootCompletion(EFI_BOOT_MANAGER_LOAD_OPTION *Option){(VOID)Option;}
EFI_STATUS EFIAPI DeviceBootManagerPriorityBoot(EFI_BOOT_MANAGER_LOAD_OPTION *Option){(VOID)Option;return EFI_NOT_FOUND;}
VOID EFIAPI DeviceBootManagerBdsEntry(VOID){}
VOID EFIAPI DeviceBootManagerUnableToBoot(VOID){
  EFI_GUID File={0x35E0D1B5,0x93CE,0x4D6A,{0x9A,0x93,0x6A,0xDA,0xA3,0xF2,0x6C,0x40}};
  VOID *Source=NULL;UINTN Bytes=0;EFI_HANDLE Core=NULL;
  EFI_STATUS S=EnsureFence();
  RequireAlive();if(S!=EFI_SUCCESS)CpuDeadLoop();
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
