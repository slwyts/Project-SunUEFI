// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_UFS_SHUTDOWN_GUID {0x3D43C751,0xC239,0x42D5,{0x95,0x9C,0x75,0x21,0xF0,0x18,0x02,0x60}}
typedef struct {UINT64 Revision;EFI_STATUS (*Halt)(VOID);} PIANO_UFS_SHUTDOWN;
