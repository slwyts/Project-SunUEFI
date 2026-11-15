// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastbootBoot.h"
// The caller retains the original firmware-loaded UKI and its allocation.
// Return only the embedded .linux file view; do not trust a child-supplied path.
EFI_STATUS PianoUkiImageKernel(CONST VOID *LoadedBase,UINTN LoadedBytes,PIANO_BOOT_SOURCE *Kernel);
// Destination RAM must have been checked against the live allocation map.
// EFI stub data may change; every executable section must match the UKI file.
EFI_STATUS PianoUkiImageVerifyCode(CONST PIANO_BOOT_SOURCE *Kernel,CONST VOID *Base,UINTN Bytes);
