// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SUNUEFI_PIANOCPUINPUT_H
#define SUNUEFI_PIANOCPUINPUT_H
#include <Uefi.h>
#define PIANO_CPU_INPUT_LOW_BYTES (64ULL*1024*1024)
#define PIANO_CPU_INPUT_MAX_BYTES (1024ULL*1024*1024)
#define PIANO_CPU_INPUT_CHUNK 65536U
// Same live proof used by Linux EFI handoff; no second ready flag or planner.
typedef struct {
 UINT32 Revision;EFI_STATUS Status;UINT64 BootEpoch,DramBytes,NormalBytes;
 UINT32 UnresolvedReservations;
 BOOLEAN FullDdr,FixedReservations,DynamicReservations,RuntimeRegions,CacheVerified,OwnershipVerified;
} PIANO_LINUX_MEMORY_PROOF;
typedef struct {
 VOID *Context;
 BOOLEAN (*BootServicesAlive)(VOID *);
 EFI_STATUS (*ServiceSlice)(VOID *,UINTN BudgetUs);
 // Root observes its current configured DT/EFI map/current owner registry.
 EFI_STATUS (*CheckMemory)(VOID *,PIANO_LINUX_MEMORY_PROOF *);
 EFI_STATUS (*ValidateMemory)(VOID *,CONST PIANO_LINUX_MEMORY_PROOF *);
 // Must verify an actual allocation or already-taken CPU source lease against
 // the platform owner registry and current WB CPU map. Producer identifies
 // the real FileSource/DownloadBlob; SourceOwner is NULL during file fill.
 EFI_STATUS (*ValidateBuffer)(VOID *,CONST PIANO_LINUX_MEMORY_PROOF *,
   CONST VOID *Producer,VOID *SourceOwner,CONST VOID *Base,UINT64 Bytes);
} PIANO_CPU_INPUT_ENV;
// <=64MiB keeps the legacy CPU path when Env is NULL. Above that, all real
// callbacks and a validated full-memory proof are mandatory; max is 1GiB.
EFI_STATUS PianoCpuInputAuthorize(CONST PIANO_CPU_INPUT_ENV *,UINT64 Bytes,PIANO_LINUX_MEMORY_PROOF *);
EFI_STATUS PianoCpuInputValidateBuffer(CONST PIANO_CPU_INPUT_ENV *,CONST PIANO_LINUX_MEMORY_PROOF *,
  CONST VOID *Producer,VOID *Owner,CONST VOID *Base,UINT64 Bytes);
EFI_STATUS PianoCpuInputSlice(CONST PIANO_CPU_INPUT_ENV *);
// Caller must first validate ownership/ranges. Each operation has 64KiB steps,
// exact-success service slices and CPU lifetime checks, including zero release.
EFI_STATUS PianoCpuInputCopy(CONST PIANO_CPU_INPUT_ENV *,VOID *To,CONST VOID *From,UINTN Bytes);
EFI_STATUS PianoCpuInputZero(CONST PIANO_CPU_INPUT_ENV *,VOID *Base,UINTN Bytes);
EFI_STATUS PianoCpuInputHash(CONST PIANO_CPU_INPUT_ENV *,CONST VOID *Base,UINTN Bytes,UINT8 Sha256[32]);

#endif // SUNUEFI_PIANOCPUINPUT_H
