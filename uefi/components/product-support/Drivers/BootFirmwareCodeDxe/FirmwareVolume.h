// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/MemoryAttribute.h>

// The volume can extend beyond the preserved window; the complete SEC image
// and every page changed must be inside it.
EFI_STATUS
BootFirmwareProtectSec (
  IN EFI_PHYSICAL_ADDRESS Base,
  IN UINTN Size,
  IN EFI_MEMORY_ATTRIBUTE_PROTOCOL *Memory
  );
