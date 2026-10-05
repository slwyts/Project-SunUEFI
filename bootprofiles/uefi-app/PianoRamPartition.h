// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_RAM_PARTITION_MAX 32U
typedef struct {UINT64 Base,AvailableLength;} PIANO_RAM_BANK;
// Native SM8750 Env output; NOT old header's named 48-byte entry.
typedef struct {UINT64 Base,Size;UINT32 RawType,Padding;} PIANO_RAM_PRELOADED;
typedef struct {
  EFI_STATUS Status,Locate,Identity,Abi,BanksStatus,PreloadedStatus,Release;
  BOOLEAN Present,IdentityVerified,AbiVerified,FetchAttempted,PotentialFallback,Retained;
  BOOLEAN DataValid,OwnershipVerified; // final coherent data != target ownership
  UINT64 Revision;
  UINTN ImageBase,ImageSize,InterfaceAddress;
  UINT32 BankCount;UINT64 PreloadedCount;
  PIANO_RAM_BANK Banks[PIANO_RAM_PARTITION_MAX];
  PIANO_RAM_PRELOADED Preloaded[PIANO_RAM_PARTITION_MAX];
  VOID *RetainedSource,*RetainedHandles;
} PIANO_RAM_PARTITION_REPORT;
// APP-only, no provider registration, memory mapping or target allocation.
// Default FALSE verifies presence/source/vtable only, with no native Get calls.
EFI_STATUS PianoRamPartitionInventory(BOOLEAN ExplicitFetch,PIANO_RAM_PARTITION_REPORT *Report);
