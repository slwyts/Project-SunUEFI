// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
EFI_STATUS PianoPogoHardwarePrepare(VOID);
EFI_STATUS PianoPogoHardwareStart(CONST VOID *Fdt);
EFI_STATUS PianoPogoHardwarePump(VOID);
EFI_STATUS PianoPogoHardwareStop(VOID);
BOOLEAN PianoPogoHardwareRetained(VOID);
