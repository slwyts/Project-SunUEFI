// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/ScsiPassThruExt.h>

// Block layer for a verified UFS ExtScsiPassThru controller. PHY, clocks and
// DMA setup belong to that controller and must be validated before attaching.
typedef struct {
  UINT32 Signature;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL *Transport;
  UINT64 ScsiLun;
  EFI_BLOCK_IO_MEDIA Media;
  EFI_BLOCK_IO_PROTOCOL Block;
  BOOLEAN WritesEnabled;
  EFI_LBA WriteFirst,WriteLast;
  EFI_STATUS LastStatus;
} PIANO_UFS_BLOCK;
EFI_STATUS PianoUfsBlockInit(PIANO_UFS_BLOCK *Device,
                           EFI_EXT_SCSI_PASS_THRU_PROTOCOL *Transport,UINT8 Lun);
EFI_STATUS PianoUfsEnableWrites(PIANO_UFS_BLOCK *Device,EFI_LBA First,EFI_LBA Last);
VOID PianoUfsDisableWrites(PIANO_UFS_BLOCK *Device);
