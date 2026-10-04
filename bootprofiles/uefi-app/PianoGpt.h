// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_GPT_MAX_ARRAY_BYTES 65536U
typedef struct {
  UINT64 FirstUsable,LastUsable,EntryLba;
  UINT32 Entries,EntryBytes,ArrayBytes,HeaderCrc,ArrayCrc;
} PIANO_GPT_HEADER;
UINT32 PianoGptCrc32(CONST VOID *Data,UINTN Bytes);
EFI_STATUS PianoGptParseHeader(CONST VOID *Data,UINTN Bytes,UINT64 LastLba,UINT32 BlockBytes,PIANO_GPT_HEADER *Header);
EFI_STATUS PianoGptCheckEntries(CONST VOID *Data,UINTN Bytes,CONST PIANO_GPT_HEADER *Header,UINTN *ActiveEntries);
