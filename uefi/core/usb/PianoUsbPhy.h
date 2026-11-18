// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/EFIClock.h>
// Called with both controller and PHY domains/clocks owned and DWC3 halted.
EFI_STATUS PianoUsbPhyStart(EFI_CLOCK_PROTOCOL *Clock);
// CC1 or CC2 for the running QMP, or zero if no lane is ready.
UINT8 PianoUsbPhyOrientation(VOID);
// Switch only after the device or host controller has fully stopped DMA.
// On failure the old lane is restored if possible.
EFI_STATUS PianoUsbPhySetOrientation(UINT8 Orientation);
// Called only after the device core has stopped DMA. No clock release here.
EFI_STATUS PianoUsbPhyStop(VOID);
