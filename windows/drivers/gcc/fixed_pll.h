/* SPDX-License-Identifier: MIT */
#pragma once
#include "clock_interface.h"

typedef struct {
    ULONG users;
    BOOLEAN initialized, boot, voted, restore;
} SM8750_GCC_PLL_STATE;
/* One instance and one APSS spin lock per assigned GCC, shared by every
 * consumer. GPLL0 main and its even divider hold the same physical PLL. */
typedef struct {
    volatile ULONG *base;
    ULONGLONG start, end, generation;
    PKSPIN_LOCK vote_lock;
    KMUTEX lock;
    SM8750_GCC_PLL_STATE pll[2];
    ULONG gpu_users[Sm8750GpuClockCount - 1];
    PVOID ufs;
    PVOID volatile retained_ufs;
    BOOLEAN online, removed, release_gpu_parent, release_gpu_branch;
} SM8750_GCC_PLLS;

_IRQL_requires_(PASSIVE_LEVEL) void Sm8750InitializeGcc(SM8750_GCC_PLLS *, PKSPIN_LOCK);
BOOLEAN Sm8750GccResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR, ULONGLONG *start, ULONGLONG *end);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Sm8750PrepareGccPlls(SM8750_GCC_PLLS *, PCM_PARTIAL_RESOURCE_DESCRIPTOR,
                                                              PVOID mapping);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Sm8750GccPower(SM8750_GCC_PLLS *, BOOLEAN online, BOOLEAN removed);
_IRQL_requires_(PASSIVE_LEVEL) PVOID Sm8750DetachGcc(SM8750_GCC_PLLS *);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS
    Sm8750SetGccPll(PVOID clocks, enum sm8750_gcc_pll, BOOLEAN *held, BOOLEAN on);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS
    Sm8750SetGpuGccClock(SM8750_GCC_PLLS *, enum sm8750_gcc_gpu_clock, BOOLEAN *held, BOOLEAN on);
void Sm8750AbandonGpuGccClocks(SM8750_GCC_PLLS *, BOOLEAN held[Sm8750GpuClockCount]);
NTSTATUS Sm8750GccGeneration(SM8750_GCC_PLLS *, ULONGLONG *generation);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Sm8750ReleaseGccPllBootReference(SM8750_GCC_PLLS *, enum sm8750_gcc_pll);

NTSTATUS Sm8750OpenUfsClocks(SM8750_GCC_PLLS *, PVOID adapter);
BOOLEAN Sm8750CloseUfsClocks(SM8750_GCC_PLLS *, PVOID adapter);
BOOLEAN Sm8750UfsPowerRetained(SM8750_GCC_PLLS *, PVOID adapter);
ULONG Sm8750ReadUfsClock(SM8750_GCC_PLLS *, PVOID adapter, ULONG offset);
void Sm8750WriteUfsClock(SM8750_GCC_PLLS *, PVOID adapter, ULONG offset, ULONG value);

/* End of the UFS PHY domain status, the highest register we access. */
#define SM8750_GCC_REQUIRED_BYTES 0x9e008
