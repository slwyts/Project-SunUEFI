/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

/* Owned once by GCC, not once per GPU, USB or storage device. The same
 * lock protects APSS vote words used by the rest of the clock controller.
 * On a warm handoff the platform retires its initial reference after all
 * users of the fixed PLL have registered their own references. */
typedef struct {
    volatile ULONG *base;
    ULONGLONG start, end;
    PKSPIN_LOCK vote_lock;
    KMUTEX lock;
    ULONG users;
    BOOLEAN initialized, boot, voted;
} SM8750_GCC_PLL0;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Sm8750PrepareGccPll0(SM8750_GCC_PLL0 *pll, PCM_PARTIAL_RESOURCE_DESCRIPTOR gcc,
                             void *mapping, PKSPIN_LOCK vote_lock);
/* Call under the board's XO/CX power reference. A failed hardware wait
 * can leave the supplied reference held; release it before unmapping GCC. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Sm8750SetGccPll0(SM8750_GCC_PLL0 *pll, BOOLEAN *held, BOOLEAN on);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Sm8750ReleaseGccPll0BootReference(SM8750_GCC_PLL0 *pll);
