// SPDX-License-Identifier: BSD-2-Clause-Patent
// NT36532 touchscreen as an EFI AbsolutePointer
#pragma once
#include <Uefi.h>
#include "PianoProductOwners.h"

// Only checks the board DT; EFI_NOT_FOUND means the owner is absent
EFI_STATUS PianoTouchStart(IN CONST VOID *Fdt);
// APP-only: one bounded bring-up step or one frame read per call
VOID PianoTouchPump(VOID);
// No DMA is involved, so an incomplete restore is logged and handoff continues
EFI_STATUS PianoTouchStop(IN VOID *Context,OUT PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report);
