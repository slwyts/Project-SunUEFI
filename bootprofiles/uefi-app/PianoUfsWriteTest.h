// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// This is a transaction test, not BlockIO and not a general WRITE interface.
#define PIANO_UFS_WRITE_TEST_LUN 4U
#define PIANO_UFS_WRITE_TEST_LBA 375040ULL
#define PIANO_UFS_WRITE_TEST_BYTES 4096U
#define PIANO_UFS_WRITE_TEST_CAPACITY 1551892480ULL
#define PIANO_UFS_WRITE_TEST_GAP_LAST 378623ULL
#define PIANO_UFS_WRITE_TEST_ARRAY_BYTES 12288U
#define PIANO_UFS_WRITE_TEST_GUARD_COLLECTED 0x1FU

typedef struct {CONST VOID *Data;UINTN Bytes;} PIANO_UFS_WRITE_BLOB;
typedef struct {
  PIANO_UFS_WRITE_BLOB PrimaryHeader,PrimaryEntries,BackupHeader,BackupEntries,OriginalBlock;
  // Caller attestation is necessary in addition to actual byte/hash checks.
  // It refers to the durable, independently read-back PC capture, not RAM.
  BOOLEAN ExternalArchiveVerified;
  UINT64 ExternalGapBytes;
  UINT8 ExternalGapSha256[32];
} PIANO_UFS_WRITE_BASELINE;
typedef struct {
  UINT8 Lun,Collected; // capacity, mode sense, unit descriptor, permanent/power flags
  UINT64 CapacityBytes;
  EFI_STATUS CapacityStatus,ModeSenseStatus,UnitStatus,PermanentFlagStatus,PowerOnFlagStatus;
  BOOLEAN Fua,ModeWriteProtected,PermanentEnabled,PowerOnEnabled;
  UINT8 UnitWriteProtect; // 0=none, 1=power-on, 2=permanent; flags determine enablement
} PIANO_UFS_WRITE_GUARD;
typedef struct {
  UINT8 Lun;EFI_LBA Lba;UINTN Bytes;
  BOOLEAN Authorized; // defaults false; explicit restore-test profile gate
  PIANO_UFS_WRITE_BASELINE Baseline;
} PIANO_UFS_WRITE_REQUEST;
typedef EFI_STATUS (*PIANO_UFS_TEST_READ)(VOID *,UINT8,EFI_LBA,UINTN,VOID *,UINTN *);
typedef EFI_STATUS (*PIANO_UFS_TEST_WRITE_FUA)(VOID *,UINT8,EFI_LBA,UINTN,CONST VOID *,UINTN *);
typedef EFI_STATUS (*PIANO_UFS_TEST_SYNC)(VOID *,UINT8,EFI_LBA,UINTN);
typedef EFI_STATUS (*PIANO_UFS_TEST_GUARD)(VOID *,UINT8,PIANO_UFS_WRITE_GUARD *);
typedef EFI_STATUS (*PIANO_UFS_TEST_QUIET)(VOID *,BOOLEAN *);
typedef struct {
  VOID *Context;
  PIANO_UFS_TEST_READ Read;
  // Must report success only after FUA command OCS, tag, SCSI GOOD and zero
  // residual are checked. Never return success on an accepted/in-flight request.
  PIANO_UFS_TEST_WRITE_FUA WriteFua;
  PIANO_UFS_TEST_SYNC Sync;
  // Fresh same-session read queries, repeated before the restore WRITE.
  PIANO_UFS_TEST_GUARD ReadGuard;
  // Bounded hardware queue/run readback. A return code alone is insufficient.
  PIANO_UFS_TEST_QUIET Quiesced;
} PIANO_UFS_WRITE_IO;
typedef enum {
  PianoWriteGuard,PianoWritePrimaryHeader,PianoWritePrimaryEntries,PianoWriteBackupHeader,PianoWriteBackupEntries,
  PianoWriteGapScan,PianoWriteInitialReadA,PianoWriteInitialReadB,PianoWriteTestWrite,PianoWriteTestSync,PianoWriteTestRead,
  PianoWriteRestoreGuard,PianoWriteRestorePrimaryHeader,PianoWriteRestorePrimaryEntries,PianoWriteRestoreBackupHeader,PianoWriteRestoreBackupEntries,
  PianoWriteRestoreWrite,PianoWriteRestoreSync,PianoWriteRestoreRead,PianoWriteStepCount
} PIANO_UFS_WRITE_STEP_ID;
typedef struct {
  BOOLEAN Attempted,QuietAttempted,Quiet;
  UINTN Calls,Transferred;
  EFI_STATUS Status,QuietStatus;
} PIANO_UFS_WRITE_STEP;
typedef struct {
  BOOLEAN Attempted,Returned,Restore;
  UINT8 Lun;EFI_LBA Lba;UINTN Bytes,Transferred;
  EFI_STATUS Status;
} PIANO_UFS_WRITE_ATTEMPT;
typedef enum {PianoWriteRefused,PianoWriteRestored,PianoWriteTestFailedRestored,PianoWriteRestoreFailed,PianoWriteQuarantined,PianoWritePreflightPassed} PIANO_UFS_WRITE_OUTCOME;
typedef struct {
  PIANO_UFS_WRITE_OUTCOME Outcome;
  EFI_STATUS GateStatus,FirstFailure,FinalStatus;
  PIANO_UFS_WRITE_STEP Step[PianoWriteStepCount];
  PIANO_UFS_WRITE_ATTEMPT Write[2]; // marked before each callback, even on timeout
  UINTN WriteAttempts;
  BOOLEAN TestReadMatched,RestoreReadMatched,RestoredVerified,DataUnchanged,SafeToContinue,RequiresRecovery,Quarantined;
  UINT8 TestSha256[32],RestoreSha256[32];
} PIANO_UFS_WRITE_RESULT;
typedef struct {
  // Zero-initialize a new workspace. A failed/in-flight transaction cannot be
  // retried by reusing it; retain it until explicit external recovery/reset.
  UINT32 StateSignature;
  BOOLEAN Running,NeedsRecovery;
  UINT8 PrimaryHeader[4096],BackupHeader[4096],PrimaryEntries[12288],BackupEntries[12288];
  UINT8 GapScratch[4096],InitialA[4096],InitialB[4096],Original[4096],TestPattern[4096],TestRead[4096],RestoreRead[4096];
} PIANO_UFS_WRITE_WORK;

// SafeToContinue concerns this storage transaction, not complete OS handoff.
// All read buffers are disjoint from TX/rollback bytes. Live adapters must also
// clear/reissue an independent READ and invalidate DMA RX before copying back.
EFI_STATUS PianoUfsWriteTestRun(CONST PIANO_UFS_WRITE_REQUEST *,CONST PIANO_UFS_WRITE_IO *,PIANO_UFS_WRITE_WORK *,PIANO_UFS_WRITE_RESULT *);
// Same complete baseline/live GPT/capability/gap/current-block gate, with no
// WRITE or SYNC callback invocation. Authorized may remain false in this mode.
EFI_STATUS PianoUfsWriteTestPreflight(CONST PIANO_UFS_WRITE_REQUEST *,CONST PIANO_UFS_WRITE_IO *,PIANO_UFS_WRITE_WORK *,PIANO_UFS_WRITE_RESULT *);

// Wire construction only; no controller I/O or generic/arbitrary write target.
EFI_STATUS PianoUfsWriteTestBuildWrite10(VOID *,UINTN,VOID *,UINTN,UINT64,UINT64,UINT8,UINT8,EFI_LBA,UINTN);
EFI_STATUS PianoUfsWriteTestBuildSync10(VOID *,UINTN,VOID *,UINTN,UINT64,UINT8,UINT8,EFI_LBA,UINTN);

// Pure shared gates for the separately bounded experimental volume. These do
// not build/submit WRITE and do not authorize another LUN/window.
EFI_STATUS PianoUfsWriteTestCheckBaseline(CONST PIANO_UFS_WRITE_BASELINE *);
EFI_STATUS PianoUfsWriteTestCheckGuard(CONST PIANO_UFS_WRITE_GUARD *);
EFI_STATUS PianoUfsWriteTestCheckLiveGpt(CONST PIANO_UFS_WRITE_BASELINE *,CONST PIANO_UFS_WRITE_BLOB *,CONST PIANO_UFS_WRITE_BLOB *,CONST PIANO_UFS_WRITE_BLOB *,CONST PIANO_UFS_WRITE_BLOB *);
