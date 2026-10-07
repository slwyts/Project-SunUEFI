// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/BlockIo.h>
typedef EFI_STATUS (*PIANO_BLOCK_READ)(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer);
typedef struct {
  UINT32 Signature;
  EFI_BLOCK_IO_MEDIA Media;
  EFI_BLOCK_IO_PROTOCOL Block;
  VOID *Context;
  PIANO_BLOCK_READ Read;
  UINT8 Lun;
} PIANO_READ_ONLY_BLOCK;
EFI_STATUS PianoReadOnlyBlockInit(PIANO_READ_ONLY_BLOCK *Device,UINT8 Lun,UINT32 BlockBytes,
                                 EFI_LBA LastLba,VOID *Context,PIANO_BLOCK_READ Read);
