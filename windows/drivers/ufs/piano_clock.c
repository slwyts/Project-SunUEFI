/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_clock.h"

/* MiCode gcc-sun and upstream gcc-sm8750 agree on these controls inside
 * GCC. Compare their bus addresses with PnP; do not map them directly.
 */
const uint32_t piano_ufs_gcc_address[PIANO_UFS_CLOCK_REGS] = {
    0x00177000,             /* GCC_UFS_PHY_BCR */
    0x00177030, 0x00177068, /* RX lane 0 branch and parent */
    0x001770d8, 0x001770ec, /* RX lane 1 */
    0x0017702c, 0x00177058  /* TX lane 0 */
};

enum { ENABLE = 1, MUX_MASK = 3, PHY_SYMBOL = 0, REF_CLOCK = 2 };

static const enum piano_ufs_clock_reg gate[] = { PIANO_UFS_RX0_GATE, PIANO_UFS_RX1_GATE, PIANO_UFS_TX0_GATE };
static const enum piano_ufs_clock_reg mux[] = { PIANO_UFS_RX0_MUX, PIANO_UFS_RX1_MUX, PIANO_UFS_TX0_MUX };

static int update(struct piano_ufs_clocks *c, enum piano_ufs_clock_reg reg, uint32_t mask, uint32_t value)
{
    uint32_t before = c->io.read(c->io.context, reg);
    if (before == UINT32_MAX) return 0;
    if ((before & mask) != value) {
        c->io.write(c->io.context, reg, (before & ~mask) | value);
        /* These CBCRs use BRANCH_HALT_DELAY, not bit 31 as a reliable
         * PLL-running indication. First read back the posted gate write.
         */
        uint32_t after = c->io.read(c->io.context, reg);
        if (mask == ENABLE && reg != PIANO_UFS_BCR) c->io.wait_us(c->io.context, 10);
        return after != UINT32_MAX && (after & mask) == value;
    }
    return 1;
}

/* Revert only controls changed by this instance, in reverse enable order.
 * Keep any change whose readback fails for the next teardown attempt.
 */
static int restore_lanes(struct piano_ufs_clocks *c)
{
    for (unsigned n = 3; n; --n) {
        unsigned i = n-1, bit = 1u << i;
        if (c->gate_changed & bit) {
            if (!update(c, gate[i], ENABLE, 0)) return 0;
            c->gate_changed &= ~bit;
        }
        if (c->mux_changed & bit) {
            if (!update(c, mux[i], MUX_MASK, c->old_mux[i])) return 0;
            c->mux_changed &= ~bit;
        }
    }
    return 1;
}

static int set_lanes(void *ctx, int enable)
{
    struct piano_ufs_clocks *c = ctx;
    if (!c->bus_held || c->reset_owned) return 0;
    if (!enable) {
        c->lanes_held = 0;
        return restore_lanes(c);
    }
    if (c->lanes_held) return 1;
    if (!restore_lanes(c)) return 0;
    unsigned enabled = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t g = c->io.read(c->io.context, gate[i]), m = c->io.read(c->io.context, mux[i]);
        if (g == UINT32_MAX || m == UINT32_MAX) return 0;
        c->old_mux[i] = (uint8_t)(m & MUX_MASK);
        if (c->old_mux[i] != PHY_SYMBOL && c->old_mux[i] != REF_CLOCK) return 0;
        if (g & ENABLE) enabled |= 1u << i;
    }
    for (unsigned i = 0; i < 3; ++i) {
        unsigned bit = 1u << i;
        if (c->old_mux[i] != PHY_SYMBOL) {
            c->mux_changed |= bit;
            if (!update(c, mux[i], MUX_MASK, PHY_SYMBOL)) goto undo;
        }
        if (!(enabled & bit)) {
            c->gate_changed |= bit;
            if (!update(c, gate[i], ENABLE, ENABLE)) goto undo;
        }
    }
    c->lanes_held = 1;
    return 1;
undo:
    (void)restore_lanes(c);
    return 0;
}

static int set_bus(void *ctx, int enable)
{
    struct piano_ufs_clocks *c = ctx;
    if (enable) {
        if (c->bus_held) return 1;
        if (!c->bus_supply(c->supply_context, 1)) return 0;
        c->bus_held = 1;
    } else if (c->bus_held) {
        if (c->reset_uncertain) {
            /* The assert write may have arrived even when its readback
             * failed. Complete a full BCR pulse before releasing supplies.
             */
            c->io.wait_us(c->io.context, 200);
            if (!update(c, PIANO_UFS_BCR, ENABLE, 0)) return 0;
            c->io.wait_us(c->io.context, 1000);
            c->reset_owned = c->reset_uncertain = 0;
        }
        if (c->reset_owned || c->lanes_held || !restore_lanes(c) || !c->bus_supply(c->supply_context, 0))
            return 0;
        c->bus_held = 0;
    }
    return 1;
}

static int set_reset(void *ctx, int assert_reset)
{
    struct piano_ufs_clocks *c = ctx;
    if (!c->bus_held || c->lanes_held || c->mux_changed || c->gate_changed) return 0;
    if (assert_reset) {
        if (c->reset_owned) return 0;
        uint32_t before = c->io.read(c->io.context, PIANO_UFS_BCR);
        if (before == UINT32_MAX || (before & ENABLE)) return 0;
        c->reset_owned = 1;
        if (update(c, PIANO_UFS_BCR, ENABLE, ENABLE)) return 1;
        c->reset_uncertain = 1;
        return 0;
    }
    if (!c->reset_owned || !update(c, PIANO_UFS_BCR, ENABLE, 0)) return 0;
    c->reset_owned = c->reset_uncertain = 0;
    return 1;
}

int piano_ufs_clock_init(struct piano_ufs_clocks *c, const struct piano_ufs_clock_io *io,
                         void *supply_context, int (*supply)(void *, int), struct ufs_platform_power *power)
{
    if (!c || !io || !io->read || !io->write || !io->wait_us || !supply || !power || c->bus_held) return 0;
    *c = (struct piano_ufs_clocks){ .io = *io, .supply_context = supply_context, .bus_supply = supply };
    *power = (struct ufs_platform_power){ .context = c, .set_bus_power = set_bus, .set_lane_clocks = set_lanes,
                                         .set_core_reset = set_reset };
    return 1;
}
