// SPDX-License-Identifier: BSD-2-Clause-Patent
// The standard firmware boot option queues the product menu's Linux action.
#include <Uefi.h>
#include <Library/PianoProductPumpLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI PianoLinuxBootEntry(EFI_HANDLE Image, EFI_SYSTEM_TABLE *Table) {
  if(!Image || !Table || Table->BootServices!=gBS)return EFI_INVALID_PARAMETER;
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Runtime);
  if(Status!=EFI_SUCCESS)return Status;
  if(!Runtime || Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION)return EFI_UNSUPPORTED;
  return PianoProductRequestNavigation(PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE);
}
