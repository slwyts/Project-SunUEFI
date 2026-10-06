// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoGuardedRead.h"
#define PIANO_DISPLAY_CLOCK_PHASES 32U
#define PIANO_DISPLAY_CLOCK_REGISTERS 16U
typedef BOOLEAN (EFIAPI *PIANO_DISPLAY_CLOCK_ALIVE)(VOID);
typedef enum {PianoClockGcc,PianoClockDispcc,PianoClockDpu} PIANO_DISPLAY_CLOCK_GROUP;
typedef enum {PianoClockNotSkipped,PianoClockNoGccObservation,PianoClockGccUnstable,
  PianoClockGccAhbDisabled,PianoClockMappingUnavailable,PianoClockControllerBusUnproven,
  PianoClockDpuLeaseMissing} PIANO_DISPLAY_CLOCK_SKIP;
typedef struct {CONST CHAR8 *Name;UINT64 Address;PIANO_DISPLAY_CLOCK_GROUP Group;} PIANO_DISPLAY_CLOCK_SPEC;
typedef struct {
  UINT64 Address;UINT32 Value[2],Attempts;
  EFI_STATUS ReadStatus[2];PIANO_DISPLAY_CLOCK_SKIP Skip;
} PIANO_DISPLAY_CLOCK_REGISTER;
typedef struct {
  CHAR8 Phase[32];EFI_STATUS Status,GccBegin,GccEnd,DispccBegin,DispccEnd;
  BOOLEAN GccCoherent,GccAhbEnabled,DispccMappingQualified,Retained,ServicesLost;
  // These remain FALSE: mapping or an observed enable bit is not a lease.
  BOOLEAN ControllerBusHeld,DpuDomainClockHeld;
  PIANO_GUARDED_REPORT GccGuard,DispccGuard;
  PIANO_DISPLAY_CLOCK_REGISTER Register[PIANO_DISPLAY_CLOCK_REGISTERS];
} PIANO_DISPLAY_CLOCK_SNAPSHOT;
typedef struct {UINT32 Revision,Count;BOOLEAN Retained,ServicesLost;PIANO_DISPLAY_CLOCK_SNAPSHOT Snapshot[PIANO_DISPLAY_CLOCK_PHASES];} PIANO_DISPLAY_CLOCK_REPORT;
/* Required functionality stays unfinished: real GCC pairs, optional zero-load
 * DISPCC mapping qualification, declared DISPCC/DPU with explicit safety skips.
 * No caller boolean can authorize a controller/domain bus access. */
EFI_STATUS PianoDisplayClockObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive);
EFI_STATUS PianoDisplayClockReemit(PIANO_DISPLAY_CLOCK_ALIVE Alive);
BOOLEAN PianoDisplayClockRetained(VOID);
CONST PIANO_DISPLAY_CLOCK_REPORT *PianoDisplayClockGetReport(VOID);
CONST PIANO_DISPLAY_CLOCK_SPEC *PianoDisplayClockSpecs(VOID);
