// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsBoundedBlock.h"
// Exactly one physical 4096-byte gap block. No arbitrary-LU/write builder.
EFI_STATUS PianoUfsBoundedBuildWrite10(VOID *,UINTN,VOID *,UINTN,UINT64,UINT64,UINT8,UINT8,EFI_LBA,UINTN);
// Fixed complete-window range, no IMMED, no data PRDT.
EFI_STATUS PianoUfsBoundedBuildSync10(VOID *,UINTN,VOID *,UINTN,UINT64,UINT8,UINT8,EFI_LBA,UINTN);
