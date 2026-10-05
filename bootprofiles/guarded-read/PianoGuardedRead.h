// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <PiDxe.h>
#define PIANO_GUARDED_RANGE_MAX 4U
#define PIANO_GUARDED_COPY_MAX 256U
#define PIANO_GUARDED_READ_MAX 65536U
#define PIANO_GUARDED_PAGE_MAX 1024U
#define PIANO_GUARDED_USECS_MAX 100000U
typedef struct {
  UINT64 Base,Bytes;
  EFI_GCD_MEMORY_TYPE GcdType;
  UINT64 Cache; // exactly EFI_MEMORY_UC or WB, never a mapping request
  UINT8 ParAttribute; // explicit device 00/04/08/0c, normal NC44 or WBff
} PIANO_GUARDED_RANGE;
typedef struct {
  VOID *Context;
  EFI_BOOT_SERVICES *Services;EFI_DXE_SERVICES *DxeServices;
  BOOLEAN(*BootServicesAlive)(VOID *); // genuine CPU-only lifetime fence
  PIANO_GUARDED_RANGE Ranges[PIANO_GUARDED_RANGE_MAX];
  UINT32 RangeCount,MaxReads,MaxUsecs;
} PIANO_GUARDED_CONFIG;
typedef struct {
  EFI_STATUS Status,CleanupStatus;
  BOOLEAN Active,Retained,ServicesLost,Fatal,SyncOwned,SErrorOwned;
  UINT32 Reads,RecoveredFaults,PagesValidated;
  UINT64 LastAddress,Elr,Esr,Far,Spsr,Resume;
  UINT64 LastMappingPage,LastPar,LastGcdAttributes;
  UINT32 LastGcdType;EFI_STATUS MappingStatus;
  // Always false: successful observations confer no allocation/DMA ownership.
  BOOLEAN MemoryOwnershipGranted;
} PIANO_GUARDED_REPORT;
// APP-only, singleton serialized DXE adapter. Caller must reserve exclusive CPU
// handler registration for this session; never unregister a foreign handler.
// Ranges are trusted platform inputs and cannot be expanded by a cookie/data.
EFI_STATUS PianoGuardedReadBegin(CONST PIANO_GUARDED_CONFIG *,VOID **Context);
EFI_STATUS EFIAPI PianoGuardedRead32(VOID *Context,UINT64 Address,UINT32 *Value);
// SMEM collector-compatible TryRead: aligned PA/length, <=256 bytes. A failed
// multiword copy leaves Destination unchanged. Destination is valid CPU storage.
EFI_STATUS EFIAPI PianoGuardedTryRead(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Destination);
EFI_STATUS PianoGuardedReadEnd(VOID *Context);
CONST PIANO_GUARDED_REPORT *PianoGuardedReadReport(VOID);
