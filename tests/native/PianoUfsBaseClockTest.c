/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_base_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define OFF (1u << 31)
struct hw {
    uint32_t reg[PIANO_UFS_BASE_REGS];
    unsigned writes, waits, parents[4], ref, pending[4], pll_error, stuck_update, deny_ref, fail_read;
};
static uint32_t read_reg(void *ctx, enum piano_ufs_base_reg r)
{
    struct hw *h = ctx;
    if (h->fail_read == (unsigned)r + 1)
        return UINT32_MAX;
    if (r <= PIANO_AUX_CMD && !(r & 1) && h->pending[r / 2] && !h->stuck_update && !--h->pending[r / 2])
        h->reg[r] &= ~1u;
    return h->reg[r];
}
static void write_reg(void *ctx, enum piano_ufs_base_reg r, uint32_t v)
{
    struct hw *h = ctx;
    ++h->writes;
    if (r <= PIANO_AUX_CFG && (r & 1)) {
        assert((h->reg[r - 1] & 2) && !(h->reg[r - 1] & OFF));
        assert((h->reg[r] ^ v) == ((h->reg[r] ^ v) & ((1u << 20) | (3u << 12) | (7u << 8) | 31)));
        if (((v >> 8) & 7) == 5)
            assert(h->parents[r / 2] & (1u << PIANO_GPLL4));
        if (((v >> 8) & 7) == 6)
            assert(h->parents[r / 2] & (1u << PIANO_GPLL0_EVEN));
        h->reg[r] = v;
    } else if (r <= PIANO_AUX_CMD) {
        assert((h->reg[r] ^ v) == ((h->reg[r] ^ v) & 3));
        h->reg[r] = (v & ~OFF) | ((v & 2) ? 0 : OFF);
        if (v & 1)
            h->pending[r / 2] = 2;
    } else {
        assert((h->reg[r] ^ v) == 1 && (v & 0x8000));
        h->reg[r] = (v & ~OFF) | ((v & 1) ? 0 : OFF);
    }
}
static void delay(void *ctx, unsigned n)
{
    struct hw *h = ctx;
    assert(n <= 10);
    h->waits += n;
}
static int parent(void *ctx, unsigned i, enum piano_ufs_pll pll, int on)
{
    struct hw *h = ctx;
    unsigned b = 1u << pll;
    assert(i < 4);
    if (on) {
        assert(!(h->parents[i] & b));
        h->parents[i] |= b;
        if (h->pll_error)
            return 0;
    } else {
        assert(h->parents[i] & b);
        h->parents[i] &= ~b;
    }
    return 1;
}
static int dev_ref(void *ctx, int on)
{
    struct hw *h = ctx;
    if (on && h->deny_ref)
        return 0;
    h->ref = (unsigned)on;
    return 1;
}
static void init(struct piano_ufs_base_clocks *c, struct hw *h)
{
    *h = (struct hw){0};
    for (unsigned i = 0; i < 8; i += 2) {
        h->reg[i] = OFF | 0x4000;
        h->reg[i + 1] = 0x1000000u | 1;
    }
    for (unsigned i = PIANO_AXI_CBCR; i < PIANO_UFS_BASE_REGS; ++i)
        h->reg[i] = OFF | 0x8000;
    *c = (struct piano_ufs_base_clocks){.io = {h, read_reg, write_reg, delay, parent, dev_ref}};
}
static void cold(void)
{
    struct piano_ufs_base_clocks c;
    struct hw h;
    init(&c, &h);
    assert(!piano_ufs_phy_clocks(&c, 1) && !h.writes);
    assert(piano_ufs_bus_clocks(&c, 1) && h.ref);
    const uint32_t f = (1u << 20) | (5u << 8) | 3u | 0x1000000u;
    assert(h.reg[PIANO_AXI_CFG] == f && h.reg[PIANO_ICE_CFG] == f && h.reg[PIANO_UNIPRO_CFG] == f);
    for (unsigned i = PIANO_AXI_CBCR; i <= PIANO_ICE_CBCR; ++i)
        assert(h.reg[i] & 1);
    assert(!(h.reg[PIANO_QREF] & 1) && !(h.reg[PIANO_AUX_CBCR] & 1));
    assert(piano_ufs_phy_clocks(&c, 1) && (h.reg[PIANO_QREF] & 1) && (h.reg[PIANO_AUX_CBCR] & 1));
    unsigned writes = h.writes;
    assert(piano_ufs_bus_clocks(&c, 1) && piano_ufs_phy_clocks(&c, 1) && h.writes == writes);
    assert(!piano_ufs_bus_clocks(&c, 0) && h.ref && h.writes == writes);
    assert(piano_ufs_phy_clocks(&c, 0) && piano_ufs_bus_clocks(&c, 0) && !h.ref);
    assert(!c.gates && !c.inherited && !c.bus_pending && !c.phy_pending);
    for (unsigned i = 0; i < 4; ++i)
        assert(!h.parents[i] && h.reg[i * 2 + 1] == (0x1000000u | 1));
    for (unsigned i = PIANO_AXI_CBCR; i < PIANO_UFS_BASE_REGS; ++i)
        assert(h.reg[i] == (OFF | 0x8000));
}
static void warm(void)
{
    struct piano_ufs_base_clocks c;
    struct hw h;
    init(&c, &h);
    for (unsigned i = PIANO_AXI_CBCR; i < PIANO_UFS_BASE_REGS; ++i)
        h.reg[i] = 0x8003;
    h.reg[PIANO_AXI_CFG] = (1u << 20) | (6u << 8) | 5;
    h.reg[PIANO_UNIPRO_CFG] = (1u << 20) | (5u << 8) | 7;
    h.reg[PIANO_ICE_CFG] = (1u << 20) | (5u << 8) | 3;
    h.reg[PIANO_AUX_CFG] = 3;
    assert(piano_ufs_bus_clocks(&c, 1) && piano_ufs_phy_clocks(&c, 1) && !h.writes);
    assert(h.parents[0] == (1u << PIANO_GPLL0_EVEN) && h.parents[1] == (1u << PIANO_GPLL4) && h.ref);
    assert(piano_ufs_phy_clocks(&c, 0) && piano_ufs_bus_clocks(&c, 0) && !h.writes);
    assert(piano_ufs_bus_clocks(&c, 1) && piano_ufs_phy_clocks(&c, 1));
    piano_ufs_claim_clocks(&c);
    assert(piano_ufs_phy_clocks(&c, 0) && piano_ufs_bus_clocks(&c, 0) && h.writes == 7);
    for (unsigned i = PIANO_AXI_CBCR; i < PIANO_UFS_BASE_REGS; ++i)
        assert(h.reg[i] == (OFF | 0x8002));
    assert(h.reg[PIANO_AXI_CFG] == ((1u << 20) | (6u << 8) | 5));
}
static void failures(void)
{
    struct piano_ufs_base_clocks c;
    struct hw h;
    init(&c, &h);
    h.pll_error = 1;
    assert(!piano_ufs_bus_clocks(&c, 1) && !h.parents[0] && !h.ref);
    h.pll_error = 0;
    h.deny_ref = 1;
    assert(!piano_ufs_bus_clocks(&c, 1) && !c.bus_pending && !h.parents[2] && !c.gates);
    h.deny_ref = 0;
    h.stuck_update = 1;
    assert(!piano_ufs_bus_clocks(&c, 1) && c.bus_pending && c.root[0].changed && h.parents[0]);
    assert(!piano_ufs_bus_clocks(&c, 0) && h.parents[0] && !h.ref);
    h.stuck_update = 0;
    assert(piano_ufs_bus_clocks(&c, 0) && !c.bus_pending && !h.parents[0]);
    assert(piano_ufs_bus_clocks(&c, 1));
    h.stuck_update = 1;
    assert(!piano_ufs_phy_clocks(&c, 1) && c.phy_pending);
    assert(!piano_ufs_bus_clocks(&c, 0) && h.ref);
    h.stuck_update = 0;
    assert(piano_ufs_phy_clocks(&c, 0) && piano_ufs_bus_clocks(&c, 0));
    h.fail_read = PIANO_UNIPRO_CBCR + 1;
    assert(!piano_ufs_bus_clocks(&c, 1) && !h.ref && !c.bus_pending);
}
int main(void)
{
    cold();
    warm();
    failures();
    for (unsigned i = 0; i < PIANO_UFS_BASE_REGS; ++i)
        printf("%x\n", piano_ufs_base_address[i]);
}
