// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoGuardedRead.h"
#include "PianoDisplayClockLease.h"
#define PIANO_DISPLAY_CLOCK_PHASES 32U
#define PIANO_DISPLAY_CLOCK_REGISTERS 16U
typedef BOOLEAN (EFIAPI *PIANO_DISPLAY_CLOCK_ALIVE)(VOID);
typedef enum {PianoClockGcc,PianoClockDispcc,PianoClockDpu} PIANO_DISPLAY_CLOCK_GROUP;
typedef enum {PianoClockNotSkipped,PianoClockNoGccObservation,PianoClockGccUnstable,
  PianoClockGccAhbDisabled,PianoClockMappingUnavailable,PianoClockControllerBusUnproven,
  PianoClockDpuLeaseMissing,PianoClockControllerPowerUnproven,
  PianoClockHeldProofUnavailable} PIANO_DISPLAY_CLOCK_SKIP;
typedef struct {CONST CHAR8 *Name;UINT64 Address;PIANO_DISPLAY_CLOCK_GROUP Group;} PIANO_DISPLAY_CLOCK_SPEC;
typedef struct {
  UINT64 Address;UINT32 Value[2],Attempts;
  EFI_STATUS ReadStatus[2];PIANO_DISPLAY_CLOCK_SKIP Skip;
} PIANO_DISPLAY_CLOCK_REGISTER;
typedef struct {
  CHAR8 Phase[32];EFI_STATUS Status,GccBegin,GccEnd,DispccBegin,DispccEnd;
  BOOLEAN GccCoherent,GccAhbEnabled,DispccMappingQualified,Retained,ServicesLost;
  // Real clock-reference proof does not carry an MMCX/rail/DPU access lease.
  BOOLEAN ClockReferenceHeld;
  BOOLEAN ControllerBusHeld,DpuDomainClockHeld;
  EFI_STATUS HeldBorrow,HeldScope,HeldReturn;
  PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF HeldBefore,HeldAfter;
  PIANO_GUARDED_REPORT GccGuard,DispccGuard;
  PIANO_DISPLAY_CLOCK_REGISTER Register[PIANO_DISPLAY_CLOCK_REGISTERS];
} PIANO_DISPLAY_CLOCK_SNAPSHOT;
typedef struct {UINT32 Revision,Count;BOOLEAN Retained,ServicesLost;PIANO_DISPLAY_CLOCK_SNAPSHOT Snapshot[PIANO_DISPLAY_CLOCK_PHASES];} PIANO_DISPLAY_CLOCK_REPORT;
/* Required functionality stays unfinished: real GCC pairs, optional zero-load
 * DISPCC mapping qualification, declared DISPCC/DPU with explicit safety skips.
 * No caller boolean can authorize a controller/domain bus access. */
EFI_STATUS PianoDisplayClockObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive);
/* Borrow the actual already-held product gcc_disp_ahb reference, validate its
 * scope, qualify the fixed mapping and freshly return the borrow. No BOOL or
 * caller snapshot can authorize bus reads. The missing MMCX/rail lifetime is
 * explicit: eight DISPCC registers still skip, and all six DPU registers skip.
 * The supplied lease must be the driver's resident real lease state. */
EFI_STATUS PianoDisplayClockObserveHeld(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive,PIANO_DISPLAY_CLOCK_LEASE *Lease);
EFI_STATUS PianoDisplayClockReemit(PIANO_DISPLAY_CLOCK_ALIVE Alive);
BOOLEAN PianoDisplayClockRetained(VOID);
CONST PIANO_DISPLAY_CLOCK_REPORT *PianoDisplayClockGetReport(VOID);
CONST PIANO_DISPLAY_CLOCK_SPEC *PianoDisplayClockSpecs(VOID);
