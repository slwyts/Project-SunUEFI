// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_RAM_BOOT_PROBE_GUID {0x97ed2b41,0xa457,0x4cd6,{0xb7,0x04,0x8d,0x96,0xb1,0x18,0xad,0x80}}
#define PIANO_RAM_BOOT_PROBE_NAME L"SunUEFI-RamBootProbe"
#define PIANO_RAM_BOOT_PROBE_SIGNATURE SIGNATURE_32('S','R','B','P')
// Fixed-size RAM-only evidence. No NON_VOLATILE or RUNTIME attribute is used.
typedef struct {
  UINT32 Signature,Revision,Bytes,Flags;
  UINT64 CurrentEl,ImageBase,ImageBytes,ImageHandle,ParentHandle;
  UINT64 LoadedStatus,ConsoleStatus;
  UINT32 SystemTableRevision,BootServicesRevision,Crc32,Reserved;
} PIANO_RAM_BOOT_PROBE_RECORD;
EFI_STATUS EFIAPI PianoRamBootProbeEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable);
