// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoBootFileSource.h"
// Discover the unique current LUN0 ESP named sunuefi_esp, cross-check its parent,
// partition/path/media identity, then read EFI/Piano/stable/boot.img. No captured
// partition UUID, LBA, number or capacity is required. Writable media is allowed;
// the loader opens the file in read mode and creates only a read-only CPU snapshot.
typedef struct {
  PIANO_BOOT_FILE_SOURCE File;
  PIANO_CPU_INPUT_ENV Cpu;
  PIANO_BOOT_FILE_ENV Env;
  PIANO_LAUNCH_BLOB Blob;
  PIANO_BOOT_SOURCE Reader;
  VOID *Owner,*Loan;
  CONST VOID *View;
  BOOLEAN Loaded,Released,Retained;
} PIANO_ESP_BOOT_SOURCE;
EFI_STATUS PianoEspBootLoad(PIANO_ESP_BOOT_SOURCE *,CONST PIANO_CPU_INPUT_ENV *);
// Return the unique current ESP filesystem handle after the same full partition,
// parent and media checks, without opening or reading any file.
EFI_STATUS PianoEspBootFind(EFI_HANDLE *Esp);
// Check EFI/BOOT/BOOTAA64.EFI without loading it or changing media. The raw
// compatibility loader above continues to use EFI/Piano/stable/boot.img.
EFI_STATUS PianoEspBootPresent(VOID);
typedef struct {
 EFI_STATUS Locate,Open;
 UINT32 Filesystems,Matches,Candidates;
 struct {
   UINT32 Stage,BlockSize,Revision,Type;
   UINT64 LastBlock,FirstLba,LastLba;
   UINT8 Lun,Pun;
   CHAR16 Name[36];
 } Candidate[16];
} PIANO_ESP_DISCOVERY;
CONST PIANO_ESP_DISCOVERY *PianoEspBootDiscovery(VOID);
BOOLEAN PianoEspBootOwned(CONST PIANO_ESP_BOOT_SOURCE *,VOID *Owner);
EFI_STATUS PianoEspBootRelease(PIANO_ESP_BOOT_SOURCE *);
BOOLEAN PianoEspBootReleased(CONST PIANO_ESP_BOOT_SOURCE *);
