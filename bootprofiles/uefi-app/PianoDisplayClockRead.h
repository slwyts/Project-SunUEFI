// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <PiDxe.h>
#include "PianoDisplayClockLease.h"
#include "PianoGuardedRead.h"
#define PIANO_DISPLAY_CLOCK_READ_REVISION 1U
#define PIANO_DISPLAY_CLOCK_READ_MAP_BYTES 65536U
typedef enum {
  PianoClockReadNone=0,PianoClockReadText,PianoClockReadImageData,
  PianoClockReadGlobal,PianoClockReadClient,PianoClockReadModules,
  PianoClockReadModule,PianoClockReadParent,PianoClockReadClientRef,
  PianoClockReadRegistryEntry // internal verification only; no public read role
} PIANO_DISPLAY_CLOCK_READ_ROLE;
typedef struct {
  VOID *Context;EFI_BOOT_SERVICES *Services;EFI_DXE_SERVICES *DxeServices;
  BOOLEAN (*BootServicesAlive)(VOID *);
  // Actual driver-lifetime Lease state. Its discovery sets NativeImage before
  // Source calls ReadCpu; no caller-supplied "verified"/ownership boolean.
  PIANO_DISPLAY_CLOCK_LEASE *Lease;
} PIANO_DISPLAY_CLOCK_READ_ENV;
typedef struct {
  UINT32 Revision;EFI_STATUS Status,PinStatus,IdentityStatus,MapStatus,EndStatus;
  BOOLEAN Busy,Retained,ServicesLost,TextVerified,MemoryOwnershipGranted;
  PIANO_DISPLAY_CLOCK_READ_ROLE Role;
  UINT64 Address,ObjectBase,ObjectBytes,ProducerAnchor;
  UINT32 Sessions,Words,AnchorSnapshots,ClientNodes;
  PIANO_GUARDED_REPORT Guard;
} PIANO_DISPLAY_CLOCK_READ_REPORT;
typedef struct {
  UINT32 Signature;PIANO_DISPLAY_CLOCK_READ_ENV Env;
  PIANO_DISPLAY_CLOCK_READ_REPORT Report;
  EFI_HANDLE ImageHandle;EFI_LOADED_IMAGE_PROTOCOL *ImageIdentity;
  UINT64 ImageBase;VOID *PinnedCopy;UINTN PinnedBytes,NextText;
  UINT64 Map[PIANO_DISPLAY_CLOCK_READ_MAP_BYTES/8];
} PIANO_DISPLAY_CLOCK_READ;
// Initialize authenticates the exact FV Clock PE. No native Clock invocation,
// mapping, allocation permission, DMA permission or MMIO session is created.
EFI_STATUS PianoDisplayClockReadInitialize(PIANO_DISPLAY_CLOCK_READ *,CONST PIANO_DISPLAY_CLOCK_READ_ENV *);
// Lease-compatible callback. Every protected session ends before returning.
// Only exact pinned image text/data or the bounded native typed graph qualifies.
// Destination is unchanged unless read + anchor/identity/map + End all succeed.
EFI_STATUS PianoDisplayClockReadCpu(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Destination);
// CPU-only lifetime fence; never attempts EFI cleanup after ExitBootServices.
VOID PianoDisplayClockReadFenceExit(PIANO_DISPLAY_CLOCK_READ *);
// Release the FV copy after an early failure. Retained state is terminal.
EFI_STATUS PianoDisplayClockReadClose(PIANO_DISPLAY_CLOCK_READ *);
