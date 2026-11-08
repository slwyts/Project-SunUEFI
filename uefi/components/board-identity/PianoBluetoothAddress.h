// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS PianoBluetoothAddressInitialize(VOID);
EFI_STATUS PianoBluetoothAddressApplyDtb(VOID *Tree,UINTN Capacity);
