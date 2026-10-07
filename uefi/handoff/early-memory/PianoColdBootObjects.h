// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoEarlyMemory.h"
#include <Library/MemoryMapLib.h>
#define PIANO_COLD_OBJECT_VERSION 2U
#define PIANO_COLD_HANDOFF_ADDRESS 0xa7fff000ULL
#define PIANO_COLD_HANDOFF_MAGIC 0x534e554546494448ULL
#define PIANO_COLD_EXTENSION_MAGIC 0x314a424f544f4f42ULL
#define PIANO_COLD_HANDOFF_BYTES 144U
#define PIANO_COLD_OBJECT_MAX 24U
#define PIANO_COLD_DTB_MAX 0x200000U
#define PIANO_COLD_READ_MAX 256U
#define PIANO_COLD_TOTAL_MAX (2U*PIANO_COLD_DTB_MAX+0x10000U)
#define PIANO_COLD_MAX_USECS 2000000U
#define PIANO_COLD_OBJECT_HOB_GUID {0x3025a79b,0x7ed3,0x4f6b,{0x90,0x50,0x43,0x4f,0x4c,0x44,0x30,0x31}}
// Preserve the legacy magic/DTB/EL prefix, and ARM64 image header offset56.
// Counter is same-boot metadata, CRC is structural integrity, neither ownership.
typedef struct {
 UINT64 Magic,Dtb,EntryEl,ExtensionMagic;
 UINT32 Version,Bytes;
 UINT64 Counter,Frequency,ShimBase,ShimBytes,FdSource,FdBase,FdBytes;
 UINT64 EntrySp,EntryVbar,EntrySctlr,Flags;
 UINT32 Crc32,Reserved;UINT64 EntryPc;
} PIANO_COLD_BOOT_HANDOFF;
typedef struct {UINT64 Pc,Sp,El,Sctlr,Vbar,Daif,SpSel,Ttbr0,Ttbr1,Counter,Frequency;} PIANO_COLD_CPU;
typedef struct {PIANO_SEC_READ_STATE Read;UINT64 Words;} PIANO_COLD_BATCH_STATE;
UINTN EFIAPI PianoColdSecRead256(UINT64 Address,UINT32 *PrivateScratch,PIANO_COLD_BATCH_STATE *,UINTN Bytes);
typedef enum {
 PianoColdObjectNone=0,PianoColdObjectFirmware,PianoColdObjectStack,
 PianoColdObjectVectorReservation,PianoColdObjectActiveVector,
 PianoColdObjectMmuReservation,PianoColdObjectSecHeap,PianoColdObjectScheduler,
 PianoColdObjectFv,PianoColdObjectHandoff,PianoColdObjectOriginalShim,
 PianoColdObjectOriginalFd,PianoColdObjectFactoryDtb,PianoColdObjectCombinedInitrd,
 PianoColdObjectHobHeap
} PIANO_COLD_OBJECT_ROLE;
typedef struct {UINT64 Base,Bytes;PIANO_COLD_OBJECT_ROLE Role;UINT32 Reserved;} PIANO_COLD_BOOT_OBJECT;
typedef enum {
 PianoColdReasonNone=0,PianoColdReasonCpu,PianoColdReasonLayout,PianoColdReasonRead,
 PianoColdReasonLegacyHandoff,PianoColdReasonHandoff,PianoColdReasonEpoch,
 PianoColdReasonDtb,PianoColdReasonInitrd,PianoColdReasonCoherence,PianoColdReasonBudget
} PIANO_COLD_OBJECT_REASON;
typedef struct {
 UINT32 Version,Bytes,ReportCrc32,Reserved;
 EFI_STATUS Status,HandoffStatus,DtbStatus,PublishStatus;
 PIANO_COLD_OBJECT_REASON Reason;UINT32 Count,Loads,RecoveredFaults;
 UINT32 GuardBatches,CacheFills,CacheHits,ReservedCache;
 BOOLEAN Attempted,Finished,Published,Coherent,MemoryOwnershipGranted,HighDdrPublished,AuthorityReady;
 UINT8 ReservedFlags;
 UINT64 Epoch;PIANO_COLD_CPU Cpu,After;
 UINT64 ElapsedTicks,ElapsedUsecs,LastReadStart,LastCompletedAddress;
 UINT32 LastReadBytes,ReservedRead;
 PIANO_COLD_BOOT_HANDOFF Handoff;
 UINT32 DtbBytes,DtbHeaderCrc32;UINT64 InitrdStart,InitrdEnd;
 UINT8 DtbHeader[40]; // exact repeated header, not a whole-DTB checksum claim
 PIANO_SEC_READ_STATE LastRead,LastFault;
 PIANO_COLD_BOOT_OBJECT Objects[PIANO_COLD_OBJECT_MAX];
} PIANO_COLD_BOOT_OBJECT_REPORT;
// Singleton SEC producer, before the first PHIT/MemoryPeim. Calls the same real
// bounded short batch fixup, with an independent fixed BootObjects whitelist.
EFI_STATUS PianoColdBootObjectsObserve(VOID);
// After the one real HobConstructor: frozen typed object input, never resources.
EFI_STATUS PianoColdBootObjectsPublishHob(VOID);
CONST PIANO_COLD_BOOT_OBJECT_REPORT *PianoColdBootObjectsReport(VOID);
// Consumer validates exact layout/CRC before importing these occupied spans.
// The output contains no allocator, cache, high-DDR or full-owner authority.
EFI_STATUS PianoColdBootObjectsValidate(CONST PIANO_COLD_BOOT_OBJECT_REPORT *);
STATIC inline UINT32 PianoColdHandoffCrc(CONST PIANO_COLD_BOOT_HANDOFF *R){
 CONST UINT8 *P=(CONST UINT8*)R;UINT32 C=MAX_UINT32;for(UINTN I=0;I<sizeof(*R);++I){UINT8 V=I>=128&&I<132?0:P[I];C^=V;for(UINTN B=0;B<8;++B)C=(C>>1)^((C&1)?0xedb88320U:0);}return ~C;
}
STATIC inline UINT32 PianoColdObjectsCrc(CONST PIANO_COLD_BOOT_OBJECT_REPORT *R){
 CONST UINT8 *P=(CONST UINT8*)R;UINT32 C=MAX_UINT32;for(UINTN I=0;I<sizeof(*R);++I){UINT8 V=I>=OFFSET_OF(PIANO_COLD_BOOT_OBJECT_REPORT,ReportCrc32)&&I<OFFSET_OF(PIANO_COLD_BOOT_OBJECT_REPORT,ReportCrc32)+4?0:P[I];C^=V;for(UINTN B=0;B<8;++B)C=(C>>1)^((C&1)?0xedb88320U:0);}return ~C;
}
