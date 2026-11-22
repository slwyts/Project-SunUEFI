/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "miniport.h"
#include "piano_clock.h"
#include "piano_base_clock.h"
#include "piano_power.h"
#include "piano_rpmh.h"
#include "../gcc/clock_interface.h"

typedef struct {
    struct piano_ufs_clocks clocks;
    struct piano_ufs_bus_power power;
    struct piano_ufs_base_clocks base;
    SM8750_GCC_INTERFACE gcc;
    SM8750_UFS_GCC_WINDOWS windows;
    BOOLEAN pll[4][Sm8750GccPllCount];
    PIANO_UFS_RPMH rpmh;
    UFS_ADAPTER *adapter;
    PULONG reg[PIANO_UFS_CLOCK_REGS];
    PULONG bus_reg[PIANO_UFS_BASE_REGS];
    PULONG domain[PIANO_UFS_DOMAINS];
} PIANO_UFS_CLOCK_DEVICE;

/* ACPI assigns HCI, QMP and the UFS TCSR gate to this board instance.
 * GCC lends its own UFS windows. PnP must not assign overlapping GCC
 * resources to both the clock provider and the storage controller.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *controller, UFS_ADAPTER *adapter,
                          PPORT_CONFIGURATION_INFORMATION config, const PIANO_RPMH_INTERFACE *rpmh,
                          const SM8750_GCC_INTERFACE *gcc, const struct piano_ufs_parent *parent);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *controller, ULONGLONG now_us);
