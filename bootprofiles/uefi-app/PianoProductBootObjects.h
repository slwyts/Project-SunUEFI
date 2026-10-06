// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include "PianoColdBootObjects.h"
// Capture the unique cold GUID HOB once, then replay only cached typed metadata.
// Caller supplies the real CPU-only BootServices fence. Never target-reads.
EFI_STATUS PianoProductBootObjectsReemit(BOOLEAN (*Alive)(VOID));
EFI_STATUS PianoProductBootObjectsStatus(VOID);
CONST PIANO_COLD_BOOT_OBJECT_REPORT *PianoProductBootObjectsSnapshot(VOID);
