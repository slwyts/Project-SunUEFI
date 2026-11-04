// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoMemoryLayout.h"
#include <Library/MemoryMapLib.h>
#define PIANO_COLD_MEMORY_HOB_GUID \
 {0x85e26af7,0x1b65,0x4805,{0xb8,0x21,0x4d,0x45,0x4d,0x50,0x49,0x41}}
typedef struct {
 UINT32 Version,Bytes,Crc32,Reserved;
 EFI_STATUS Status,PublishStatus;
 UINT64 BootEpoch,InputFingerprint;
 UINT32 NativeCount,Count;
 PIANO_MEMORY_LAYOUT Layout;
 EFI_MEMORY_REGION_DESCRIPTOR Rows[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
} PIANO_COLD_MEMORY_REPORT;
EFI_STATUS PianoColdMemoryPrepare(VOID);
EFI_STATUS PianoColdMemoryPublishHob(VOID);
// Generated product MemoryMapLib stores this pointer only in the current SEC
// library instance. No DXE remap or second MemoryPeim is involved.
EFI_STATUS PianoProductSelectMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR *,UINT8);
STATIC inline UINT32 PianoColdMemoryCrc(CONST PIANO_COLD_MEMORY_REPORT *R){
 CONST UINT8 *P=(CONST UINT8 *)R;UINT32 C=MAX_UINT32;
 for(UINTN I=0;I<sizeof(*R);++I){UINT8 V=I>=OFFSET_OF(PIANO_COLD_MEMORY_REPORT,Crc32)&&I<OFFSET_OF(PIANO_COLD_MEMORY_REPORT,Crc32)+4?0:P[I];C^=V;for(UINTN X=0;X<8;++X)C=(C>>1)^((C&1)?0xedb88320U:0);}return ~C;
}
