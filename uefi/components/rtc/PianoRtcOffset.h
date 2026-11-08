// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS PianoRtcOffsetInitialize(VOID);
EFI_STATUS PianoRtcOffsetApplyDtb(VOID *Tree,UINTN Capacity);
