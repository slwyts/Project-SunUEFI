// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/EFIClock.h>
// Called with both controller and PHY domains/clocks owned and DWC3 halted.
EFI_STATUS PianoUsbPhyStart(EFI_CLOCK_PROTOCOL *Clock);
// Called only after the device core has stopped DMA. No clock release here.
EFI_STATUS PianoUsbPhyStop(VOID);
