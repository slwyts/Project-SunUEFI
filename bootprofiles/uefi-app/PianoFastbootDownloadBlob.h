// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastboot.h"
#include "PianoFastbootLaunch.h"
#include "PianoCpuInput.h"

// Adapter for the existing bounded CPU pool. This does not enable boot, change
// the advertised limit or provide a high-RAM allocator. The caller must stop
// command dispatch and drain copied TX frames before ReadyToTake acknowledges.
typedef EFI_STATUS (*PIANO_DOWNLOAD_QUIET)(VOID *Context);
typedef struct {
  UINT32 Signature;
  PIANO_FASTBOOT *Source;
  VOID *QuietContext;
  PIANO_DOWNLOAD_QUIET ReadyToTake;
  EFI_FREE_POOL ReleasePool; // Real status-returning Boot Services free, not VOID library wrapper.
  UINT8 *BoundDownload;
  UINT8 *Owned;
  UINTN Bytes;
  UINTN LoanSequence;
  VOID *ActiveLoan;
  EFI_STATUS ReleaseStatus;
  BOOLEAN Taken,Consumed,ReleaseAttempted;
  PIANO_CPU_INPUT_ENV Cpu;PIANO_LINUX_MEMORY_PROOF Memory;
  BOOLEAN HasCpu,Busy,Retained;
} PIANO_FASTBOOT_DOWNLOAD_BLOB;

// State must be zero-initialized, have driver lifetime, and never be overwritten
// while bound or retained. Source must remain alive for the entire operation.
// Successful quiet acknowledgement freezes command dispatch/content until Take.
EFI_STATUS PianoFastbootDownloadBlobBind(PIANO_FASTBOOT_DOWNLOAD_BLOB *State,
  PIANO_FASTBOOT *Source,VOID *QuietContext,PIANO_DOWNLOAD_QUIET ReadyToTake,
  EFI_FREE_POOL ReleasePool,PIANO_LAUNCH_BLOB *Blob);
// Same quiet/take/ACK/DMA semantics, optional Root-owned large CPU policy.
// NULL policy caps admission at64MiB. Does not advertise/allocate high RAM.
EFI_STATUS PianoFastbootDownloadBlobBindWithCpu(PIANO_FASTBOOT_DOWNLOAD_BLOB *,
  PIANO_FASTBOOT *,VOID *,PIANO_DOWNLOAD_QUIET,EFI_FREE_POOL,
  CONST PIANO_CPU_INPUT_ENV *,PIANO_LAUNCH_BLOB *);
