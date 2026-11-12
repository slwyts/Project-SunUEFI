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
typedef enum {
  PianoClockEfiMapNotObserved=0,PianoClockEfiMapReady,
  PianoClockEfiMapServicesLost,PianoClockEfiMapGetMap,
  PianoClockEfiMapFormat,PianoClockEfiMapInvalidDescriptor,
  PianoClockEfiMapOverlap,PianoClockEfiMapNoCoverage,
  PianoClockEfiMapWrongType,PianoClockEfiMapCache,
  PianoClockEfiMapReadProtected,PianoClockEfiMapRuntime,
  PianoClockEfiMapNonIdentityVirtual
} PIANO_DISPLAY_CLOCK_EFI_MAP_REASON;
typedef struct {
  EFI_STATUS Status,GetMapStatus;
  PIANO_DISPLAY_CLOCK_EFI_MAP_REASON Reason;
  UINT32 DescriptorIndex,DescriptorType,DescriptorVersion,ConflictIndex;
  UINT64 DescriptorBase,DescriptorPages,DescriptorAttributes,DescriptorVirtual;
  UINT64 Cursor,ConflictBase,ConflictPages;
  UINTN MapBytes,DescriptorBytes,MapKey;
} PIANO_DISPLAY_CLOCK_EFI_MAP_DIAGNOSTIC;
typedef struct {
  UINT32 Revision;EFI_STATUS Status,PinStatus,IdentityStatus,MapStatus,EndStatus;
  BOOLEAN Busy,Retained,ServicesLost,TextVerified,MemoryOwnershipGranted;
  PIANO_DISPLAY_CLOCK_READ_ROLE Role;
  UINT64 Address,ObjectBase,ObjectBytes,ProducerAnchor;
  UINT32 Sessions,Words,AnchorSnapshots,ClientNodes;
  PIANO_GUARDED_REPORT Guard;
  // Last actual GetMemoryMap decision; descriptor fields are raw EFI metadata,
  // never current-cache/ownership proof. Reason disambiguates absent fields.
  PIANO_DISPLAY_CLOCK_EFI_MAP_DIAGNOSTIC EfiMap;
} PIANO_DISPLAY_CLOCK_READ_REPORT;
typedef struct {
  UINT32 Signature;PIANO_DISPLAY_CLOCK_READ_ENV Env;
  PIANO_DISPLAY_CLOCK_READ_REPORT Report;
  EFI_HANDLE ImageHandle;EFI_LOADED_IMAGE_PROTOCOL *ImageIdentity;
  UINT64 ImageBase;VOID *PinnedCopy;UINTN PinnedBytes,NextText;
  UINT64 ReadSequence,LastReadAddress;UINTN LastReadBytes;EFI_STATUS LastReadStatus;
  UINT64 Map[PIANO_DISPLAY_CLOCK_READ_MAP_BYTES/8];
} PIANO_DISPLAY_CLOCK_READ;
typedef enum {PianoClockSelectGccAhb=0,PianoClockSelectNonGdscAhb=1} PIANO_DISPLAY_CLOCK_SELECTOR;
typedef enum {PianoClockConfigUnobserved=0,PianoClockConfigPinned,PianoClockConfigOtherProducer} PIANO_DISPLAY_CLOCK_CONFIG_OBSERVATION;
#define PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT_REVISION 1U
// Read-only diagnostic identity/reference snapshot. ExpectedClockId is a fixed
// pinned selector, not an observed GetID result or a clock/rail owner grant.
typedef struct {
 UINT32 Revision;PIANO_DISPLAY_CLOCK_SELECTOR Selector;EFI_STATUS Status,Identity;
 VOID *ReaderContext,*LeaseContext;EFI_HANDLE NativeImage;UINT64 NativeBase,ImageSize;
 UINT32 MatchingSnapshots,ExpectedClockId,Provider,Index,ModuleCount,ClockCount;
 UINT64 Global,Client,Module,Array,Node,Name,Parent,ClientRef;
 UINT32 GlobalFlags,NodeFlags,ParentFlags,ParentRailMask;UINT8 ClientFlags;
 UINT16 Total[2],PerClient[2],ParentRefs[2];BOOLEAN ClientRefPresent;
 UINT64 ParentCurrentConfig;UINT32 ParentCachedCorner;UINT8 ParentVoteAlternate;
 PIANO_DISPLAY_CLOCK_CONFIG_OBSERVATION ConfigObservation;UINT32 CurrentCorner;
 UINT64 MmClient,MxClient;
} PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT;
// Initialize authenticates the exact FV Clock PE. No native Clock invocation,
// mapping, allocation permission, DMA permission or MMIO session is created.
EFI_STATUS PianoDisplayClockReadInitialize(PIANO_DISPLAY_CLOCK_READ *,CONST PIANO_DISPLAY_CLOCK_READ_ENV *);
// Lease-compatible callback. Every protected session ends before returning.
// Only exact pinned image text/data or the bounded native typed graph qualifies.
// Destination is unchanged unless read + anchor/identity/map + End all succeed.
EFI_STATUS PianoDisplayClockReadCpu(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Destination);
// Fixed GCC and second CESTA non-GDSC AHB selectors only. Requires completed
// actual image/text verification; no GetID, Enable, NPA request or MMIO occurs.
// Two complete matching observations, output unchanged on failure. A missing
// client ref or unknown runtime configuration stays an explicit observation.
EFI_STATUS PianoDisplayClockReadSnapshotClock(PIANO_DISPLAY_CLOCK_READ *,PIANO_DISPLAY_CLOCK_SELECTOR,PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *);
// Fresh observation of this reader's immediately preceding first-text refusal.
// Does not call EFI, authorize a memory read, or certify a native reference.
EFI_STATUS PianoDisplayClockReadFailureEvidence(VOID *Context,UINT64 Address,UINTN Bytes,EFI_STATUS Status,PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE *);
// CPU-only lifetime fence; never attempts EFI cleanup after ExitBootServices.
VOID PianoDisplayClockReadFenceExit(PIANO_DISPLAY_CLOCK_READ *);
// Release the FV copy after an early failure. Retained state is terminal.
EFI_STATUS PianoDisplayClockReadClose(PIANO_DISPLAY_CLOCK_READ *);
