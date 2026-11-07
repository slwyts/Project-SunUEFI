// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDma.h"
// APP only. Caller holds GDSC, clocks, PHY and the USB0 SMMU domain and has
// stopped the device service (DCTL.RUN_STOP clear)
EFI_STATUS PianoUsbHostStart(PIANO_DMA_DEVICE *Device);
// APP only. EFI_NOT_READY: a consumer still holds the controller and nothing
// was released; any other error leaves the session retained
EFI_STATUS PianoUsbHostStop(VOID);
BOOLEAN PianoUsbHostActive(VOID);
BOOLEAN PianoUsbHostRetained(VOID);
// MMIO only, for ExitBootServices or reset: halt the xHC and keep every page
EFI_STATUS PianoUsbHostFenceExit(VOID);
