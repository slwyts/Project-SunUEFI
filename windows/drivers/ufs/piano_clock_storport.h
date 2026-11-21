/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "miniport.h"
#include "piano_clock.h"
#include "piano_power.h"
#include "piano_rpmh.h"

typedef struct {
    struct piano_ufs_clocks clocks;
    struct piano_ufs_bus_power power;
    PIANO_UFS_RPMH rpmh;
    UFS_ADAPTER *adapter;
    PULONG reg[PIANO_UFS_CLOCK_REGS];
    PULONG domain[PIANO_UFS_DOMAINS];
} PIANO_UFS_CLOCK_DEVICE;

/* Called only for a board-matched ACPI/PnP instance. After HCI and QMP,
 * the owning bus assigns seven scalar clock/reset resources and two pairs
 * of GDSCR/CFG_GDSCR registers. These AP votes have one software owner.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *controller, UFS_ADAPTER *adapter,
                          PPORT_CONFIGURATION_INFORMATION config, const PIANO_RPMH_INTERFACE *rpmh,
                          const struct piano_ufs_parent *parent);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *controller, ULONGLONG now_us);
