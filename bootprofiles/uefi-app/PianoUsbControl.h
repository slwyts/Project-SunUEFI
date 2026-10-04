// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
typedef struct {UINT8 Address,Configuration,PendingAddress,PendingConfiguration;BOOLEAN SetAddress,SetConfiguration,SuperSpeed;} PIANO_USB_CONTROL;
typedef enum {PianoUsbStall,PianoUsbDataIn,PianoUsbStatusIn} PIANO_USB_CONTROL_ACTION;
EFI_STATUS PianoUsbControlSetup(PIANO_USB_CONTROL *State,CONST UINT8 Setup[8],UINT8 *Data,UINTN Capacity,
                               UINTN *Bytes,PIANO_USB_CONTROL_ACTION *Action);
VOID PianoUsbControlStatusComplete(PIANO_USB_CONTROL *State);
