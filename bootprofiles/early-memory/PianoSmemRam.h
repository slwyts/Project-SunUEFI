// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_SMEM_BASE          0x81D00000ULL
#define PIANO_SMEM_BYTES         0x00200000U
#define PIANO_SMEM_COOKIE_LOW    0x01FD4000ULL
#define PIANO_SMEM_COOKIE_HIGH   0x01FD4004ULL
#define PIANO_SMEM_RAM_ITEM      402U
#define PIANO_SMEM_RAM_MAX       64U
#define PIANO_SMEM_PAYLOAD_MAX   8192U
#define PIANO_SMEM_TRACE_MAX     32768U
#define PIANO_SMEM_READ_MAX      256U
#define PIANO_SMEM_CALLS_MAX     2048U
#define PIANO_SMEM_TOTAL_MAX     262144U

typedef enum {
  PianoSmemReasonNone, PianoSmemReasonArguments, PianoSmemReasonRead,
  PianoSmemReasonBudget, PianoSmemReasonUninitialized,
  PianoSmemReasonMajor, PianoSmemReasonBounds, PianoSmemReasonMissing,
  PianoSmemReasonAuxRegion, PianoSmemReasonTable, PianoSmemReasonPartition,
  PianoSmemReasonEntry, PianoSmemReasonDuplicate, PianoSmemReasonUnstable,
  PianoSmemReasonRamMagic, PianoSmemReasonRamVersion, PianoSmemReasonRamCount,
  PianoSmemReasonRamType, PianoSmemReasonRamRange, PianoSmemReasonRamOverlap
} PIANO_SMEM_REASON;

typedef EFI_STATUS (EFIAPI *PIANO_SMEM_TRY_READ)(VOID *Context,
  UINT64 PhysicalAddress, UINTN Bytes, VOID *Destination);
typedef struct {
  VOID *Context;
  // Must perform a bounded, exception-recoverable read and return exact SUCCESS
  // only for a complete copy. No native initialization/lock/allocation/write.
  PIANO_SMEM_TRY_READ TryRead;
  UINT32 MaxReadCalls, MaxReadBytes; // Nonzero, at most the hard limits above.
} PIANO_SMEM_READER;

typedef struct {
  UINT64 Base, RawSize, AvailableLength;
  UINT32 RawType, SourceIndex;
} PIANO_SMEM_RAM_ENTRY;
typedef struct {
  EFI_STATUS Status, CallbackStatus, CookieStatus;
  PIANO_SMEM_REASON Reason;
  UINT32 ReadCalls, ReadBytes, SmemVersion, RamVersion, RawEntryCount;
  UINT32 BankCount, PreloadedCount, OtherCategoryCount;
  UINT64 CookieValue, PayloadAddress;
  UINT32 PayloadBytes, PayloadCrc32, MetadataBytes;
  // Exact metadata and payload comparisons from two sequential observations.
  // These are not an atomic snapshot, hardware ownership or memory-map proof.
  BOOLEAN RepeatedMetadataEqual, RepeatedPayloadEqual, CookieRepeatedEqual;
  BOOLEAN Parsed;
  PIANO_SMEM_RAM_ENTRY Banks[PIANO_SMEM_RAM_MAX];
  PIANO_SMEM_RAM_ENTRY Preloaded[PIANO_SMEM_RAM_MAX];
} PIANO_SMEM_RAM_REPORT;
typedef struct {
  // Zero initialize once; keep in producer-lifetime static storage, NOT SEC
  // stack. One producer, serialized calls. Same-workspace reentry is rejected.
  BOOLEAN Busy;
  UINT8 Payload[2][PIANO_SMEM_PAYLOAD_MAX];
  UINT8 Metadata[PIANO_SMEM_TRACE_MAX];
} PIANO_SMEM_RAM_WORK;

// Pure parser. Input/output must not alias. RAM402 exact versions1/2/3 accepted;
// raw SDRAM claims are preserved, never retagged or published as usable memory.
EFI_STATUS PianoSmemRamParse(CONST VOID *Payload, UINTN Bytes,
  PIANO_SMEM_RAM_REPORT *Report);
// Fixed-window, no allocation/no write/no native calls. Cookie reads report raw
// observations only: cookie/SIII never grants permission to dereference outside
// the proven SMEM window. No platform MMIO binding is supplied by this module.
// Report/Work/Reader must be disjoint. Failure clears parsed bank/preloaded data;
// raw observations/counters remain diagnostic. SUCCESS is not full-DDR Ready.
EFI_STATUS PianoSmemRamCollect(CONST PIANO_SMEM_READER *Reader,
  PIANO_SMEM_RAM_WORK *Work, PIANO_SMEM_RAM_REPORT *Report);
