// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_MEMORY_LAYOUT_MAX_RAM 32U
#define PIANO_MEMORY_LAYOUT_MAX_RESERVED 96U
#define PIANO_MEMORY_LAYOUT_CMA 1U
// Frozen, page-aligned physical intervals. Dynamic reserved-memory requests
// have no physical placement until Linux allocates them after ExitBootServices.
typedef struct {UINT64 Base,Bytes;UINT32 Flags,Reserved;} PIANO_MEMORY_SPAN;
typedef struct {
 UINT32 RamCount,ReservedCount,FutureLinuxDynamicConstraints,Reserved;
 UINT64 DramBytes;
 PIANO_MEMORY_SPAN Ram[PIANO_MEMORY_LAYOUT_MAX_RAM];
 PIANO_MEMORY_SPAN Fixed[PIANO_MEMORY_LAYOUT_MAX_RESERVED];
} PIANO_MEMORY_LAYOUT;

// Read only the already-authorized tree; this callback never admits a physical
// pointer by itself. The SEC adapter retains its original guarded read window.
typedef EFI_STATUS (*PIANO_MEMORY_LAYOUT_READ)(VOID *,UINT32,UINTN,VOID *);
EFI_STATUS PianoMemoryLayoutRead(PIANO_MEMORY_LAYOUT_READ Read,VOID *Context,
 UINT32 Bytes,PIANO_MEMORY_LAYOUT *Layout);
