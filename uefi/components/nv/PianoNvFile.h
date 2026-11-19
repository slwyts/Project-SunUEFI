// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
EFI_STATUS PianoNvFileStart(VOID);
EFI_STATUS PianoNvFileSave(BOOLEAN Freeze);
// Save and freeze only if this is the partition backing the current store.
