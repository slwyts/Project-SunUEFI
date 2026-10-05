// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/MemoryMapLib.h>
#include "PianoRamPartition.h"
#define PIANO_PLATFORM_MEMORY_MAX_OWNERS 64U
typedef struct {UINT64 Base,Bytes;INT32 DynamicNode;} PIANO_PLATFORM_MEMORY_OWNER;
typedef struct {
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Native;UINTN NativeCount;
  CONST PIANO_RAM_PARTITION_REPORT *Inventory;
  CONST VOID *Fdt;UINTN FdtBytes;
  CONST PIANO_PLATFORM_MEMORY_OWNER *KnownOwners;UINTN OwnerCount;
  UINT64 CpuArenaBase,CpuArenaBytes; // optional, occupied CPU-only arena request
} PIANO_PLATFORM_MEMORY_INPUT;
typedef struct {
  EFI_MEMORY_REGION_DESCRIPTOR Rows[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 Count;UINT32 FixedReservations,UnplacedDynamicConstraints;
  UINT64 AddedOccupiedBytes,CpuArenaBase,CpuArenaBytes;
  UINT64 InputFingerprint;
  BOOLEAN Composed,ReadyForMemoryPeim,AuthorizationAttempted,Exposed;
  EFI_STATUS Status,Authorization;
} PIANO_PLATFORM_MEMORY_CONTRACT;
typedef EFI_STATUS(*PIANO_PLATFORM_MEMORY_AUTHORIZE)(VOID *,CONST PIANO_PLATFORM_MEMORY_INPUT *,CONST PIANO_PLATFORM_MEMORY_CONTRACT *);
// Bounded actual EFI descriptor construction. No BS, MMU, high memory access,
// allocations or fake Native provider. Composition alone grants no authority.
EFI_STATUS PianoPlatformMemoryCompose(CONST PIANO_PLATFORM_MEMORY_INPUT *Input,PIANO_PLATFORM_MEMORY_CONTRACT *Contract);
// Root's real early phase/ownership authority must authorize the same inputs.
// A DXE inventory cannot by itself authorize calling SEC MemoryPeim again.
EFI_STATUS PianoPlatformMemoryAuthorizeCold(CONST PIANO_PLATFORM_MEMORY_INPUT *Input,PIANO_PLATFORM_MEMORY_CONTRACT *Contract,
  PIANO_PLATFORM_MEMORY_AUTHORIZE Authorize,VOID *Context);
EFI_STATUS PianoPlatformMemoryAcquireForMemoryPeim(PIANO_PLATFORM_MEMORY_CONTRACT *Contract,
  EFI_MEMORY_REGION_DESCRIPTOR **Rows,UINT8 *Count);
