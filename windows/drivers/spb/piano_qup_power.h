/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "piano_qup_clock.h"

extern const unsigned piano_qup_i2c_pins[2];

struct piano_qup_board_io {
    void *context;
    /* Hold the crystal and wrapper paths, and reserve both fixed I2C pads
     * from their single pin controller before accessing their CTL fields.
     * A failed acquire/release leaves the previous reference unchanged.
     */
    int (*fabric)(void *, int acquire);
    int (*reserve_pads)(void *, int acquire);
    uint32_t (*read_pad)(void *, unsigned gpio);
    void (*write_pad)(void *, unsigned gpio, uint32_t value);
};

struct piano_qup_power {
    struct piano_qup_clocks clocks;
    unsigned fabric_held, pads_held, changed, ready;
    uint32_t old[2], mask[2];
};

/* One state per SE6, zeroed by its PnP owner. D0 entry precedes QCI2C's
 * first PrepareHardware register access, D3 follows its final transfer.
 * A failed stop retains the resources it still needs for a retry.
 */
int piano_qup_set_power(struct piano_qup_power *p, const struct piano_qup_io *clock,
                        const struct piano_qup_board_io *board, int enable);
