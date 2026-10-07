// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoGuardedRead.h"
#define PIANO_DISPLAY_SMMU_PHASES 32U
#define PIANO_DISPLAY_SMMU_ROUTES 127U
typedef BOOLEAN (EFIAPI *PIANO_DISPLAY_SMMU_ALIVE)(VOID);
typedef enum {
  PianoDisplaySmmuNone,PianoDisplaySmmuGuard,PianoDisplaySmmuGeometry,
  PianoDisplaySmmuMissing,PianoDisplaySmmuAmbiguous,PianoDisplaySmmuShape,
  PianoDisplaySmmuRead,PianoDisplaySmmuChanged,PianoDisplaySmmuComplete,
  PianoDisplaySmmuCleanup,PianoDisplaySmmuLost
} PIANO_DISPLAY_SMMU_REASON;
typedef struct {
  UINT32 Control,Id0,Id1,Id2,GlobalFault;
  UINT32 Smr[PIANO_DISPLAY_SMMU_ROUTES],S2cr[PIANO_DISPLAY_SMMU_ROUTES];
  UINT32 Matches,Slot,SelectedSmr,SelectedS2cr;
  UINT32 Sctlr,Cbar,Cba2r,Tcr,Tcr2,Mair0,Mair1,Fsr,Fsynr;
  UINT64 Ttbr0,Ttbr1,Far;
} PIANO_DISPLAY_SMMU_REGISTERS;
typedef struct {
  CHAR8 Phase[32];EFI_STATUS Status,BeginStatus,EndStatus;
  PIANO_DISPLAY_SMMU_REASON Reason;
  BOOLEAN BankRead,Coherent,Retained,ServicesLost;
  PIANO_DISPLAY_SMMU_REGISTERS Registers;
  PIANO_GUARDED_REPORT Guard;
} PIANO_DISPLAY_SMMU_SNAPSHOT;
typedef struct {
  UINT32 Revision,Count;BOOLEAN Retained,ServicesLost;
  PIANO_DISPLAY_SMMU_SNAPSHOT Snapshot[PIANO_DISPLAY_SMMU_PHASES];
} PIANO_DISPLAY_SMMU_REPORT;
/* Metadata/register observation only; fixed CB2 whitelist, never write/repair.
 * Caller halts on Retained/ServicesLost. Native StartImage must be outside every
 * Begin/End interval. This is not translation, ownership or display readiness. */
EFI_STATUS PianoDisplaySmmuObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_SMMU_ALIVE Alive);
EFI_STATUS PianoDisplaySmmuReemit(PIANO_DISPLAY_SMMU_ALIVE Alive);
BOOLEAN PianoDisplaySmmuRetained(VOID);
CONST PIANO_DISPLAY_SMMU_REPORT *PianoDisplaySmmuGetReport(VOID);
