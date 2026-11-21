/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "miniport.h"
#include "piano_clock.h"

typedef struct {
    struct piano_ufs_clocks clocks;
    UFS_ADAPTER *adapter;
    PULONG reg[PIANO_UFS_CLOCK_REGS];
} PIANO_UFS_CLOCK_DEVICE;

/* Called only for a board-matched ACPI/PnP instance. After HCI and QMP,
 * the owning bus must assign the seven scalar clock/reset memory resources.
 * supply holds/releases its real shared votes, not an EFI clock handle.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *controller, UFS_ADAPTER *adapter,
                          PPORT_CONFIGURATION_INFORMATION config, void *ctx, int (*supply)(void *, int));
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *controller, ULONGLONG now_us);
