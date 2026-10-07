// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoSmmu.h"
#include "PianoIoPageTable.h"
#define PIANO_SMMU_INDEPENDENT_SIGNATURE SIGNATURE_32('S','I','N','D')
typedef enum {PianoIndependentUfs=0,PianoIndependentUsb=1} PIANO_SMMU_INDEPENDENT_MASTER;
typedef struct {
  VOID *Context;
  EFI_STATUS (*Read32)(VOID *,UINTN Address,UINT32 *Value);
  EFI_STATUS (*Write32)(VOID *,UINTN Address,UINT32 Value);
  EFI_STATUS (*Fence)(VOID *);
  EFI_STATUS (*Clean)(VOID *,CONST VOID *Memory,UINTN Bytes);
  EFI_STATUS (*NowUs)(VOID *,UINT64 *Value);
  EFI_STATUS (*Pause)(VOID *);
} PIANO_SMMU_INDEPENDENT_OPS;
typedef struct {UINT32 Cbar,Cba2r,Sctlr,Actlr,Tcr2,Ttbr0Low,Ttbr0High,Ttbr1Low,Ttbr1High,Tcr,Mair0,Mair1,Fsr,FarLow,FarHigh,Fsynr;} PIANO_SMMU_INDEPENDENT_BANK;
typedef struct {
  UINT32 Global,Id0,Id1,Id2,GlobalFault,Smr[127],S2cr[127];
  PIANO_SMMU_INDEPENDENT_BANK Banks[83];
} PIANO_SMMU_INDEPENDENT_SNAPSHOT;
typedef struct {
  UINT32 Signature;
  BOOLEAN Attached,Retained,Closed,RegisterConfigurationVerified,TablesUnreachable;
  PIANO_SMMU_INDEPENDENT_MASTER Master;
  UINT16 Sid,Slot;UINT8 Bank;
  UINT32 WritesAttempted,Syncs;
  EFI_STATUS Status;
  PIANO_SMMU_INDEPENDENT_OPS Ops;
  PIANO_IO_PAGE_TABLE *PageTable;PIANO_DMA_BUFFER *Tables;
  PIANO_SMMU_INDEPENDENT_SNAPSHOT Before,Expected;
} PIANO_SMMU_INDEPENDENT;
// Prototype uses injected status-returning register/cache operations only.
// Caller supplies exclusive driver-lifetime state and existing shared PT/DMA.
// No HAL, allocator, global reset, master enable or product integration exists.
EFI_STATUS PianoSmmuIndependentOpen(PIANO_SMMU_INDEPENDENT *,PIANO_SMMU_INDEPENDENT_MASTER,
  CONST PIANO_SMMU_INDEPENDENT_OPS *,PIANO_IO_PAGE_TABLE *,PIANO_DMA_BUFFER *);
EFI_STATUS PianoSmmuIndependentSync(PIANO_SMMU_INDEPENDENT *);
EFI_STATUS PianoSmmuIndependentClose(PIANO_SMMU_INDEPENDENT *);

#define PIANO_SMMU_PROBE_MAX_READS 4096U
#define PIANO_SMMU_PROBE_MAX_US 1000000U
typedef enum {PianoProbeRoutesOnly=0,PianoProbeCandidates=1} PIANO_SMMU_PROBE_PHASE;
typedef enum {
  PianoProbeObservationOnly=0,PianoProbeMasterBusy,PianoProbeTargetSidExists,
  PianoProbeAllCbBusy,PianoProbeReadFailed,PianoProbeGlobalGeometry,
  PianoProbeNoRouteSlot,PianoProbePeerVisible,PianoProbeActlrUnknown,
  PianoProbeBudgetExceeded,PianoProbeTopologyChanged,PianoProbeInvalidArguments
} PIANO_SMMU_PROBE_REASON;
typedef struct {
  PIANO_SMMU_PROBE_PHASE Phase;
  UINT32 FirstBank,BankCount,MaxReads,MaxUs;
} PIANO_SMMU_PROBE_CONFIG;
typedef struct {
  UINT32 Revision;
  EFI_STATUS ReadStatus;
  PIANO_SMMU_PROBE_REASON Reason;
  PIANO_SMMU_PROBE_PHASE RequestedPhase;
  BOOLEAN IdleObserved,RoutesObserved,CandidatesObserved,RecheckObserved;
  // Observation cannot grant an owner or DMA-ready result.
  BOOLEAN OwnershipGranted,DmaReady;
  UINT16 Sid,FirstInvalidSlot;
  UINT32 ReadAttempts,BanksRead,CandidateCount,TargetMatches,PeerRoutes;
  UINT32 FirstBank,BankCount;
  UINTN FailureAddress;
  UINT64 ElapsedUs;
  BOOLEAN Referenced[83],CandidateRead[83],Candidate[83];
  PIANO_SMMU_INDEPENDENT_SNAPSHOT Initial,Recheck;
} PIANO_SMMU_PROBE_REPORT;
// Default Config=NULL reads idle+known routes only, no context banks. Explicit
// candidate range is required to read CBs. Only Read32/Fence/NowUs are needed;
// Write32/Clean/Pause are never called. Report is caller-supplied (large), so
// the implementation uses small stack state and no allocation. Return/status
// describe read completion/errors; Reason describes refusal/unknown evidence.
EFI_STATUS PianoSmmuIndependentReadOnlyProbe(CONST PIANO_SMMU_INDEPENDENT_OPS *,
  PIANO_SMMU_INDEPENDENT_MASTER,CONST PIANO_SMMU_PROBE_CONFIG *,PIANO_SMMU_PROBE_REPORT *);
CONST CHAR8 *PianoSmmuIndependentProbeReasonText(PIANO_SMMU_PROBE_REASON);
