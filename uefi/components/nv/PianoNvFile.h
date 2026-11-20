// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
EFI_STATUS PianoNvFileStart(VOID);
EFI_STATUS PianoNvFileSave(BOOLEAN Freeze);
STATIC inline BOOLEAN PianoNvFileSaveOptionalError(EFI_STATUS Status) {
  return Status==EFI_OUT_OF_RESOURCES || Status==EFI_VOLUME_FULL ||
         Status==EFI_WRITE_PROTECTED;
}
BOOLEAN PianoNvFileCanConsumeBootNext(UINT16 Number);
