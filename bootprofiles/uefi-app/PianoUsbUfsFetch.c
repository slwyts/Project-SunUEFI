// SPDX-License-Identifier: BSD-2-Clause-Patent
// Foreground, read-only coexistence diagnostic. Never publishes a write API.
#include "PianoUsbUfsFetch.h"
#include "PianoFastbootBlockRead.h"
#include "PianoUsbStorageExperiment.h"
#include "PianoUfsShutdown.h"
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
STATIC EFI_STATUS FetchFailure(EFI_STATUS Status) {
  gBS->RaiseTPL(TPL_CALLBACK);
  DEBUG((DEBUG_WARN,"SUNUEFI_FETCH_FAILSTOP status=%r owners_retained=1\n",Status));
  CpuDeadLoop();return Status==EFI_SUCCESS || !EFI_ERROR(Status)?EFI_DEVICE_ERROR:Status;
}
EFI_STATUS PianoRunUsbUfsFetch(CONST VOID *Fdt) {
  EFI_STATUS Status=PianoFastbootBlockReadInit();
  DEBUG((DEBUG_WARN,"SUNUEFI_FETCH_BLOCK_BACKEND_INIT %r\n",Status));
  // Init failure (including ALREADY_STARTED) did not acquire this backend.
  // Never Stop/revoke another caller's tokens or permit the automatic timer.
  if(Status!=EFI_SUCCESS)return FetchFailure(Status);
  CONST PIANO_FB_STORAGE *Storage=PianoFastbootBlockReadStorage();
  if(Storage==NULL){PianoFastbootBlockReadStop();return FetchFailure(EFI_NOT_READY);}
  BOOLEAN Reboot=FALSE;
  Status=PianoUsbControllerRunWithStorage(Fdt,Storage,&Reboot);
  PianoFastbootBlockReadStop();
  DEBUG((DEBUG_WARN,"SUNUEFI_FETCH_USB_RETURN status=%r reboot=%u\n",Status,Reboot));
#ifdef __aarch64__
  // Re-emit late so high-RAM translation results survive long transfer logs.
  // AT does not dereference the target and never grants allocator ownership.
  STATIC CONST UINT64 Candidates[]={0xA00000000ULL,0xA00001000ULL,0xA3FFFF000ULL,0xA3FFFFFFFULL};
  for(UINTN I=0;I<ARRAY_SIZE(Candidates);++I){UINT64 Par;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(Par):"r"(Candidates[I]):"memory");
    DEBUG((DEBUG_WARN,"SUNUEFI_DRAM_AT_FINAL va=%lx par=%lx translated=%u target_dereferenced=0 ownership_verified=0\n",Candidates[I],Par,(UINT32)((Par&1)==0)));
  }
#endif
  // A failed USB teardown must not free UFS while its snapshot still contains
  // that other stream or accidentally fall through into an OS/automatic reset.
  if(Status!=EFI_SUCCESS)return FetchFailure(Status);
  // USB has already closed. Own an outer lease before typed UFS retirement;
  // its internal CALLBACK lease therefore never releases this caller's lease.
  EFI_TPL Old=gBS->RaiseTPL(TPL_CALLBACK);
  EFI_STATUS Prepare=PianoUfsBlockIoPrepareForReset();
  EFI_STATUS Shutdown=Prepare==EFI_SUCCESS?PianoUfsBlockIoShutdownForReset():Prepare;
  CONST PIANO_UFS_RESET_REPORT *Report=PianoUfsResetShutdownReport();
  BOOLEAN Clean=Prepare==EFI_SUCCESS && Shutdown==EFI_SUCCESS && Report!=NULL &&
    Report->Started==TRUE && Report->Prepared==TRUE && Report->Returned==TRUE && Report->Clean==TRUE &&
    Report->Failed==FALSE && Report->TplHeld==TRUE && Report->Result==EFI_SUCCESS &&
    Report->Disconnect==EFI_SUCCESS && Report->Halt==EFI_SUCCESS && Report->Bases==EFI_SUCCESS &&
    Report->Dma==EFI_SUCCESS && Report->Domain==EFI_SUCCESS && Report->Protocols==EFI_SUCCESS &&
    Report->Clocks==EFI_SUCCESS && Report->TransferDoorbell==0 &&
    Report->TaskDoorbell==0 && Report->TransferRun==0 && Report->TaskRun==0 && Report->Interrupt==0;
  DEBUG((DEBUG_WARN,"SUNUEFI_FETCH_ALL_OWNERS_STOPPED usb=%r ufs_prepare=%r ufs_shutdown=%r clean=%u reboot=%u\n",
    Status,Prepare,Shutdown,Clean,Reboot));
  if(!Clean)return FetchFailure(Prepare!=EFI_SUCCESS?Prepare:Shutdown!=EFI_SUCCESS?Shutdown:EFI_DEVICE_ERROR);
  if(Reboot) {
    DEBUG((DEBUG_WARN,"SUNUEFI_FETCH_REBOOT_CLEAN all_owners_retired=1\n"));
    gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);CpuDeadLoop();
  }
  // No owner remains and no reboot was requested. Only now may the normal
  // post-return timer run; the generated profile still never loads an OS.
  gBS->RestoreTPL(Old);
  return EFI_SUCCESS;
}
