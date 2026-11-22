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
    ULONGLONG start, end;
    PKSPIN_LOCK vote_lock;
    KMUTEX lock;
    INTERFACE lifetime;
    SM8750_GCC_PLL_STATE pll[2];
    SM8750_UFS_GCC_WINDOWS *ufs;
} SM8750_GCC_PLLS;

_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS
    Sm8750PrepareGccPlls(SM8750_GCC_PLLS *, PCM_PARTIAL_RESOURCE_DESCRIPTOR gcc, PVOID mapping,
                         PKSPIN_LOCK vote_lock, const INTERFACE *device);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS
    Sm8750SetGccPll(PVOID clocks, enum sm8750_gcc_pll, BOOLEAN *held, BOOLEAN on);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Sm8750ReleaseGccPllBootReference(SM8750_GCC_PLLS *, enum sm8750_gcc_pll);
_IRQL_requires_(PASSIVE_LEVEL) void Sm8750GccPllPowerLost(SM8750_GCC_PLLS *);
/* Fill IRP_MN_QUERY_INTERFACE for a child of the GCC PnP provider. */
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Sm8750GetGccInterface(SM8750_GCC_PLLS *, SM8750_GCC_INTERFACE *);
