// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_PRODUCT_CORE_GUID {0x35e0d1b5,0x93ce,0x4d6a,{0x9a,0x93,0x6a,0xda,0xa3,0xf2,0x6c,0x40}}
EFI_STATUS EFIAPI PianoProductCoreEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable);
