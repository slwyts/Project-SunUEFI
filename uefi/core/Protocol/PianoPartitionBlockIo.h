// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

#define PIANO_PARTITION_BLOCK_IO_GUID \
  {0xfb35fe59,0x94ab,0x4a48,{0xb4,0xe9,0x85,0x7b,0x0d,0x3b,0x4b,0x95}}

// Optional parent interface for a disk with independently writable partitions.
// Called before PartitionDxe publishes the child; other partitions keep their
// inherited Block I/O methods and media attributes.
typedef struct {
  VOID (EFIAPI *Configure)(CONST EFI_PARTITION_ENTRY *Entry, EFI_BLOCK_IO_PROTOCOL *Child);
} PIANO_PARTITION_BLOCK_IO_PROTOCOL;
