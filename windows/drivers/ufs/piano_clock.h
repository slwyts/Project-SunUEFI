/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "platform.h"

/* Each register is its own assigned four-byte PnP resource. The shared GCC
 * GDSC, HW control, vote and root-clock frequency registers are not mapped.
 */
enum piano_ufs_clock_reg {
    PIANO_UFS_BCR,
    PIANO_UFS_RX0_GATE, PIANO_UFS_RX0_MUX,
    PIANO_UFS_RX1_GATE, PIANO_UFS_RX1_MUX,
    PIANO_UFS_TX0_GATE, PIANO_UFS_TX0_MUX,
    PIANO_UFS_CLOCK_REGS
};
extern const uint32_t piano_ufs_gcc_address[PIANO_UFS_CLOCK_REGS];
struct piano_ufs_clock_io {
    void *context;
    uint32_t (*read)(void *, enum piano_ufs_clock_reg);
    void (*write)(void *, enum piano_ufs_clock_reg, uint32_t);
    void (*wait_us)(void *, unsigned);
};

struct piano_ufs_clocks {
    struct piano_ufs_clock_io io;
    void *supply_context;
    int (*bus_supply)(void *, int);
    unsigned bus_held, reset_owned, reset_uncertain, lanes_held, gate_changed, mux_changed;
    uint8_t old_mux[3];
};

/* A real, independently reference-counted provider is required for both
 * power domains, supplies, base clocks and interconnects. The PnP caller
 * serializes this scope with device D0/D3. An independent clock driver must
 * not also own the assigned control bits. Zero-initialize on first use.
 */
int piano_ufs_clock_init(struct piano_ufs_clocks *clock, const struct piano_ufs_clock_io *io,
                         void *supply_context, int (*supply)(void *, int), struct ufs_platform_power *power);
