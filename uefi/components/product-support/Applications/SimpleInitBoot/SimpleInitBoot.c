// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/PianoProductPumpLib.h>

EFI_STATUS EFIAPI SimpleInitBootEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)Image;
  (VOID)SystemTable;
  return PianoProductOpenSimpleInit();
}
