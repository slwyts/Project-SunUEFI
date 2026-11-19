// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoProductOwners.h"
EFI_STATUS PianoResetInitialize(CONST VOID *Fdt);
EFI_STATUS PianoResetExecute(CONST PIANO_PRODUCT_OWNERS *Owners,EFI_HANDLE Image);
