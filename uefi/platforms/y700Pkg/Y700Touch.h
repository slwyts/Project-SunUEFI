// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <Uefi.h>
EFI_STATUS Y700TouchStart(CONST VOID *Fdt);
EFI_STATUS Y700TouchStop(VOID);
BOOLEAN Y700TouchRetained(VOID);
VOID Y700TouchReplay(VOID);
