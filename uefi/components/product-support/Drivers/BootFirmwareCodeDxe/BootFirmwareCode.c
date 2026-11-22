// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <PiDxe.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "FirmwareVolume.h"

EFI_STATUS
EFIAPI
BootFirmwareCodeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  // FD_Reserved contains the original SEC and its kernel/scheduler services;
  // UEFI_FD contains this boot's SEC and its configuration and serial services.
  STATIC CHAR8 *CONST Volumes[] = { "FD_Reserved", "UEFI_FD" };
  EFI_MEMORY_ATTRIBUTE_PROTOCOL *Memory;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiMemoryAttributeProtocolGuid, NULL, (VOID **)&Memory);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  EFI_STATUS FirstError = EFI_SUCCESS;
  for (UINTN Index = 0; Index < ARRAY_SIZE (Volumes); ++Index) {
    EFI_MEMORY_REGION_DESCRIPTOR Region;
    Status = LocateMemoryRegionByName (Volumes[Index], &Region);
    if (!EFI_ERROR (Status)) {
      Status = BootFirmwareProtectSec (Region.Address, Region.Length, Memory);
    }
    DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
            "Boot firmware code: %a: %r\n", Volumes[Index], Status));
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }
  return FirstError;
}
