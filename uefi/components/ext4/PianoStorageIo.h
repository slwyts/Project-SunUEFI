// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
// Internal UFS access for the persist filesystem and Fastboot.
typedef struct {
  UINT8 Lun;
  UINT32 BlockBytes,MediaId;
  UINT64 First,Blocks;
  EFI_GUID PartitionGuid;
  VOID *Identity;
} PIANO_STORAGE_PARTITION;
EFI_STATUS PianoStorageOpen(CONST CHAR16 *Name,PIANO_STORAGE_PARTITION *Partition);
EFI_STATUS PianoStorageOpenPersist(EFI_HANDLE Handle, PIANO_STORAGE_PARTITION *Partition);
EFI_STATUS PianoStorageWrite(CONST PIANO_STORAGE_PARTITION *,UINT64 Block,UINTN Bytes,CONST VOID *Buffer);
EFI_STATUS PianoStorageFlush(CONST PIANO_STORAGE_PARTITION *);
typedef struct {
  UINT64 Submitted,Completed,WaitUs;
} PIANO_STORAGE_COMMAND_TIMING;
typedef struct {
  PIANO_STORAGE_COMMAND_TIMING Read,Write,Sync;
} PIANO_STORAGE_IO_TIMING;
// Optional counters for a synchronous BlockIO call. Restore the previous
// pointer before returning; never retain a caller's buffer across APP slices.
PIANO_STORAGE_IO_TIMING *PianoStorageSetIoTiming(PIANO_STORAGE_IO_TIMING *Timing);
// A raw write must have exclusive use of an already validated partition.
EFI_STATUS PianoStorageCloseFilesystemForWrite(EFI_HANDLE Partition);
EFI_STATUS PianoStorageCheckFilesystemClosed(EFI_HANDLE Partition);
// At application TPL, close filesystem drivers before parent disk teardown.
EFI_STATUS PianoStorageCloseFilesystems(VOID);
