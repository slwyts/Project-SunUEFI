// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsBoundedBlock.h"
#define PIANO_PRODUCT_STORAGE_TYPE_GUID {0x3e4ea305,0x5b3d,0x49a4,{0xb3,0xb4,0x58,0x44,0xef,0x51,0x36,0x48}}
#define PIANO_PRODUCT_VOLUME_LAYOUT 1U
#define PIANO_PRODUCT_VOLUME_HEADER_BLOCKS 2U
#define PIANO_PRODUCT_VOLUME_FAT_FIRST 2U
#define PIANO_PRODUCT_VOLUME_FAT_BLOCKS 2046U
#define PIANO_PRODUCT_VOLUME_NV_A_FIRST 2048U
#define PIANO_PRODUCT_VOLUME_NV_B_FIRST 2816U
#define PIANO_PRODUCT_VOLUME_NV_BLOCKS 768U
#define PIANO_PRODUCT_VOLUME_GPT_INDEX 95U
// Fixed wire header, two byte-identical 4KiB copies at container blocks0/1:
// magic[16]="PIANO-VOLUME-v1\0"; version@16=1; bytes@20=128;
// whole-block CRC32@24 (zeroed for CRC); layout@28=1; UUID@32; diskGUID@48;
// typeGUID@64; LUN@80=4; blockbytes@84=4096; firstLBA@88=375040;
// blocks@96=3584; FATfirst@100=2; FATblocks@104=2046;
// NV_Afirst@108=2048; NV_Bfirst@112=2816; NVblocks@116=768;
// GPTindex@120=95; reserved@124..4095=zero. GUIDs use EFI bytes / UUID bytes_le.
typedef struct {
  VOID *Context;
  UINT8 VolumeUuid[16];
  UINT32 LayoutId;
  EFI_STATUS (*Read)(VOID *,UINT32 Slot,UINT32 SlotBlock,UINTN Bytes,VOID *Buffer);
  EFI_STATUS (*Write)(VOID *,UINT32 Slot,UINT32 SlotBlock,UINTN Bytes,CONST VOID *Buffer);
  EFI_STATUS (*Flush)(VOID *);
} PIANO_UFS_PRODUCT_NV_IO;
typedef struct {
  PIANO_UFS_WRITE_BLOB PrimaryHeader,PrimaryEntries,BackupHeader;
} PIANO_UFS_PRODUCT_ORIGINAL_GPT;
typedef enum {PianoProductIoProbe=0,PianoProductIoFat,PianoProductIoNv,PianoProductIoClose} PIANO_UFS_PRODUCT_IO_SCOPE;
typedef struct {
  PIANO_UFS_PRODUCT_IO_SCOPE Scope;
  BOOLEAN Opened,Provisioned,Closed,Busy,Quarantined,NeedsRecovery,PendingWrite;
  EFI_STATUS LastStatus,FirstFailure,LastGuardStatus,LastQuietStatus;
  UINT64 Reads,Writes,WriteAttempts,VerifiedWrites,Flushes,Failures;
  EFI_LBA LastPhysical;
} PIANO_UFS_PRODUCT_VOLUME_STATE;
typedef struct {
  UINT32 Signature;
  EFI_BLOCK_IO_MEDIA Media;EFI_BLOCK_IO_PROTOCOL Block;
  PIANO_UFS_PRODUCT_VOLUME_STATE State;
  PIANO_UFS_WINDOW_IO Transport;
  PIANO_UFS_PRODUCT_NV_IO NvIo;
  PIANO_UFS_WRITE_GUARD LastGuard;
  UINT8 OriginalPrimary[4096],OriginalEntries[12288],OriginalBackup[4096];
  UINT8 Primary[4096],Entries[12288],Backup[4096],BackupEntries[12288];
  UINT8 Header[4096],HeaderCopy[4096],Rx[4096],Tx[4096];
} PIANO_UFS_PRODUCT_VOLUME;
// Caller zero-initializes a driver-lifetime object. There is no permission bool,
// provision callback or formatter. Original GPT pins are checked by actual SHA.
// An original GPT/no reservation returns NOT_FOUND without writes or Sync.
EFI_STATUS PianoUfsProductVolumeOpen(PIANO_UFS_PRODUCT_VOLUME *,CONST PIANO_UFS_PRODUCT_ORIGINAL_GPT *,CONST PIANO_UFS_WINDOW_IO *);
CONST PIANO_UFS_PRODUCT_NV_IO *PianoUfsProductVolumeNvIo(PIANO_UFS_PRODUCT_VOLUME *);
// Re-read immutable headers, capabilities and reserved GPT inside an already
// held transport lease. Used by the actual adapter immediately before mutation.
EFI_STATUS PianoUfsProductVolumeRefreshReservation(PIANO_UFS_PRODUCT_VOLUME *);
// Ordinary close after consumers disconnect; verified writes are permanent,
// never restored to zero. Pending/uncertain writes forbid close and later boot.
EFI_STATUS PianoUfsProductVolumeClose(PIANO_UFS_PRODUCT_VOLUME *);
// Shared actual UFS owner adapter; no separate domain/DMA allocations.
// Default-off transport gate cannot be confused with provisioning evidence.
EFI_STATUS PianoUfsProductTransportIo(PIANO_UFS_PRODUCT_VOLUME *,PIANO_UFS_WINDOW_IO *);

// Publish only the verified FAT child alongside the unchanged RO original LUs.
EFI_STATUS PianoUfsProductTransportPublish(PIANO_UFS_PRODUCT_VOLUME *);
