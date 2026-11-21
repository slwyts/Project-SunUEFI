// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS PianoRtcOffsetInitialize(UINT32 *OffsetSeconds);
EFI_STATUS PianoRtcOffsetApplyDtb(VOID *Tree,UINTN Capacity);
EFI_STATUS PianoRtcConfigureClock(CONST VOID *Fdt,UINT32 OffsetSeconds);
