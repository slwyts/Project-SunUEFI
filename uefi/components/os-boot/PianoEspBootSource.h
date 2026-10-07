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
BOOLEAN PianoEspBootOwned(CONST PIANO_ESP_BOOT_SOURCE *,VOID *Owner);
EFI_STATUS PianoEspBootRelease(PIANO_ESP_BOOT_SOURCE *);
BOOLEAN PianoEspBootReleased(CONST PIANO_ESP_BOOT_SOURCE *);
