// SPDX-License-Identifier: BSD-2-Clause-Patent
// Official cover keyboard and touchpad
#pragma once
#include <Uefi.h>
#include "PianoProductOwners.h"

// Only checks the board DT; EFI_NOT_FOUND means the owner is absent
EFI_STATUS PianoPogoStart(IN CONST VOID *Fdt);
// APP-only: one bring-up step or one report read per call. Any failure
// releases the resources already acquired.
VOID PianoPogoPump(VOID);
VOID PianoPogoReplay(VOID);
// Puts the MCU back in reset and restores RPMh votes, pins, SE6 registers and
// clock references. No DMA is involved, so an incomplete release is logged
// (PIANO_POGO_RELEASE) and handoff is refused.
EFI_STATUS PianoPogoStop(IN VOID *Context,OUT PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report);
