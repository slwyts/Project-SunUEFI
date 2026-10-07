// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_NV_READY_PROTOCOL_GUID {0x9565c3ad,0xd53b,0x491f,{0x9e,0x52,0x4c,0xe2,0xd9,0x48,0xee,0x03}}
typedef struct {
 UINT32 Revision;
 EFI_PHYSICAL_ADDRESS VariableBase,WorkingBase,SpareBase;
 UINT64 VariableBytes,WorkingBytes,SpareBytes;
 // Descriptive result of journal recovery/commit, not an authorization flag.
 UINT64 RecoveredSequence;
 UINT8 VolumeUuid[16];
} PIANO_NV_READY_PROTOCOL;
