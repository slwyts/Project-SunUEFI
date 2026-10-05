// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoSmemRam.h"
#define PIANO_SMEM_DESCRIPTOR_MAX 2048U
typedef struct {
 EFI_STATUS Status;UINT64 Address,SmemBase;UINT32 SmemBytes;
 UINT16 ItemCount,TlvCount,HostInfoBytes;UINT32 SnapshotBytes,Crc32;
 BOOLEAN RepeatedEqual,RegionMatchesKnownWindow;
 UINT8 Prefix[64];
} PIANO_SMEM_DESCRIPTOR_REPORT;
typedef struct {UINT8 Bytes[2][PIANO_SMEM_DESCRIPTOR_MAX];BOOLEAN Busy;} PIANO_SMEM_DESCRIPTOR_WORK;
// Exact native SIII fields/TLV lengths; source pointer must remain inside the
// already permitted fixedSMEM window. No cookie-derived external permission.
EFI_STATUS PianoSmemDescriptorCollect(CONST PIANO_SMEM_READER *,UINT64 Cookie,
 PIANO_SMEM_DESCRIPTOR_WORK *,PIANO_SMEM_DESCRIPTOR_REPORT *);
