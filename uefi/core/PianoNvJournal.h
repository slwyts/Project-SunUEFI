// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsProductVolume.h"
#define PIANO_NV_VARIABLE_BYTES (256U*1024U)
#define PIANO_NV_WORKING_BYTES (64U*1024U)
#define PIANO_NV_SPARE_BYTES (256U*1024U)
#define PIANO_NV_SNAPSHOT_BYTES (PIANO_NV_VARIABLE_BYTES+PIANO_NV_WORKING_BYTES+PIANO_NV_SPARE_BYTES)
#define PIANO_NV_SNAPSHOT_BLOCKS (PIANO_NV_SNAPSHOT_BYTES/4096U)
#define PIANO_NV_COMMIT_BLOCK 767U
// All buffers/context outlive the FVB and standard variable driver. No allocator
// or DMA logic here; disk access only through the reserved-volume private token.
typedef struct {
  UINT32 Signature;
  PIANO_UFS_PRODUCT_NV_IO Io;
  UINT8 *Mirror,*Scratch;
  BOOLEAN Opened,Busy,Runtime,Ready,Dirty,Quarantined,RecoveredTornSlot,ScanComplete;
  UINT32 ActiveSlot;
  UINT64 Sequence,Commits,Failures;
  EFI_STATUS LastStatus;
  UINT8 Header[4096],Footer[4096],Block[4096];
} PIANO_NV_JOURNAL;
// Bind never writes or formats. The IO comes from ProductVolumeNvIo only after
// exact GPT reservation+immutable dual volume header validation; no enable bool.
EFI_STATUS PianoNvJournalBind(PIANO_NV_JOURNAL *,CONST PIANO_UFS_PRODUCT_NV_IO *,UINT8 *Mirror,UINT8 *Scratch);
EFI_STATUS PianoNvJournalRecover(PIANO_NV_JOURNAL *);
// Candidate is a complete immutable standard NV/FTW snapshot. Inactive-footer
// invalidation -> header -> payload -> sync/whole readback -> footer commit ->
// sync/readback. RAM mirror changes only after exact committed acknowledgement.
EFI_STATUS PianoNvJournalCommit(PIANO_NV_JOURNAL *,CONST VOID *Candidate);
EFI_STATUS PianoNvJournalFlush(PIANO_NV_JOURNAL *);
VOID PianoNvJournalFenceRuntime(PIANO_NV_JOURNAL *);
UINT32 PianoNvJournalCrc(CONST VOID *,UINTN);
