// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoProductOwners.h"
typedef BOOLEAN (EFIAPI *PIANO_PRODUCT_DISPLAY_ALIVE)(VOID);
EFI_STATUS PianoProductDisplayStart(PIANO_PRODUCT_DISPLAY_ALIVE Alive);
EFI_STATUS PianoProductDisplayStartup(PIANO_PRODUCT_DISPLAY_STARTUP_REPORT *Report);
EFI_STATUS PianoProductDisplayStop(VOID *Context,PIANO_PRODUCT_DISPLAY_RETIRE_REPORT *Report);
EFI_STATUS PianoProductDisplayReplay(VOID);
// Uses the resident lease after acquisition; early phases remain observations.
EFI_STATUS PianoProductDisplayClockObserve(CONST CHAR8 *Phase,PIANO_PRODUCT_DISPLAY_ALIVE Alive);
BOOLEAN PianoProductDisplayOwnerRetained(VOID);
VOID PianoProductDisplayFenceExit(VOID);
