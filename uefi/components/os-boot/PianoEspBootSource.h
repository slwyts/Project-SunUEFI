// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
// Return the unique current ESP filesystem handle after the same full partition,
// parent and media checks, without opening or reading any file.
EFI_STATUS PianoEspBootFind(EFI_HANDLE *Esp);
// Check the product UKI without loading it or changing media.
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
