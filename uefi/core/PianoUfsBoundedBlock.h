// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsWriteTest.h"
#include <Protocol/BlockIo.h>
#define PIANO_UFS_WINDOW_FIRST 375040ULL
#define PIANO_UFS_WINDOW_LAST 378623ULL
#define PIANO_UFS_WINDOW_BLOCKS 3584U
#define PIANO_UFS_WINDOW_BYTES 14680064U
// Acquire/Release hold one transport/TPL exclusion across the complete request.
// Release must not reset when Dirty/NeedsRecovery; those states outlive each IO.
typedef EFI_STATUS (*PIANO_WINDOW_ACQUIRE)(VOID *);
typedef EFI_STATUS (*PIANO_WINDOW_RELEASE)(VOID *,BOOLEAN Quarantined);
typedef struct {PIANO_UFS_WRITE_IO Io;PIANO_WINDOW_ACQUIRE Acquire;PIANO_WINDOW_RELEASE Release;} PIANO_UFS_WINDOW_IO;
typedef struct {
  BOOLEAN Opened,Closed,Busy,Dirty,NeedsRecovery,Quarantined,RestoreVerified,ConsumersDetached;
  UINT64 Requests,WriteRequests,WriteAttempts,VerifiedBlocks,Flushes,Failures,ReadBlocks;
  EFI_LBA LastLogical,LastPhysical;
  UINTN LastRequestBlocks,LastCompletedBlocks;
  BOOLEAN LastAttempted,LastReturned;
  EFI_STATUS DisconnectStatus,LastStatus,LastWriteStatus,LastSyncStatus,LastReadStatus,LastQuietStatus;
} PIANO_UFS_WINDOW_STATE;
typedef struct {
  UINT32 Signature;
  EFI_BLOCK_IO_MEDIA Media;EFI_BLOCK_IO_PROTOCOL Block;
  PIANO_UFS_WINDOW_STATE State;
  PIANO_UFS_WRITE_BASELINE Baseline;PIANO_UFS_WINDOW_IO Transport;
  PIANO_UFS_WRITE_GUARD LastGuard;
  UINT8 Primary[4096],Entries[12288],Backup[4096],BackupEntries[12288],Rx[4096],Tx[4096];
} PIANO_UFS_BOUNDED_BLOCK;
// Caller zero-initializes once. Enabled is a profile gate, never a user dialog.
EFI_STATUS PianoUfsBoundedBlockOpen(PIANO_UFS_BOUNDED_BLOCK *,BOOLEAN Enabled,CONST PIANO_UFS_WRITE_BASELINE *,CONST PIANO_UFS_WINDOW_IO *);
// After consumers are disconnected, close ordinary protocol access before root
// submits its separately-ledgered whole-gap restore using private recovery IO.
EFI_STATUS PianoUfsBoundedBlockCloseForRecovery(PIANO_UFS_BOUNDED_BLOCK *);
// Read-only final proof: fresh guard + complete dual GPT + every gap block zero.
// Clears Dirty/NeedsRecovery only on actual independent full-window RX success.
EFI_STATUS PianoUfsBoundedBlockVerifyRestored(PIANO_UFS_BOUNDED_BLOCK *,CONST PIANO_UFS_WINDOW_IO *Recovery);
VOID PianoUfsBoundedBlockReport(CONST PIANO_UFS_BOUNDED_BLOCK *);
