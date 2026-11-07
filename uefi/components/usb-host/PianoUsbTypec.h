// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
typedef struct {
  BOOLEAN Attached;
  BOOLEAN PartnerSink;  // Rd and no Rp: a USB device is attached, this port is DFP
  UINT8 Orientation;    // 1 = CC1, 2 = CC2
  UINT8 Raw[6];         // Type-C block +0x06..+0x0B
} PIANO_USB_TYPEC_STATE;
// Read-only. EFI_UNSUPPORTED: unknown PMIC/layout, keep the device role
EFI_STATUS PianoUsbTypecRead(CONST VOID *Fdt,PIANO_USB_TYPEC_STATE *State);
