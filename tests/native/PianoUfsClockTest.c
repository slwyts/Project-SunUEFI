/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct hw {
    uint32_t reg[PIANO_UFS_CLOCK_REGS];
    unsigned writes, waited;
    int powered, deny_bus, unreadable, fail_after_write;
};

static uint32_t rd(void *arg, enum piano_ufs_clock_reg reg)
{
    struct hw *h = arg;
    assert(h->powered && reg < PIANO_UFS_CLOCK_REGS);
    return (int)reg == h->unreadable ? UINT32_MAX : h->reg[reg];
}

static void wr(void *arg, enum piano_ufs_clock_reg reg, uint32_t value)
{
    struct hw *h = arg;
    assert(h->powered && reg < PIANO_UFS_CLOCK_REGS);
    h->reg[reg] = value;
    ++h->writes;
    if (h->fail_after_write == (int)reg) h->unreadable = reg;
}

static void wait_us(void *arg, unsigned us)
{
    struct hw *h = arg;
    assert(h->powered);
    h->waited += us;
}

static int supplier(void *arg, int on)
{
    struct hw *h = arg;
    assert(on != h->powered);
    if (h->deny_bus) return 0;
    h->powered = on;
    return 1;
}

static void board(struct hw *h, struct piano_ufs_clocks *c, struct ufs_platform_power *p)
{
    *h = (struct hw){ .unreadable = -1, .fail_after_write = -1 };
    struct piano_ufs_clock_io io = { .context=h, .read=rd, .write=wr, .wait_us=wait_us };
    assert(!piano_ufs_clock_init(c, &io, h, NULL, p));
    assert(piano_ufs_clock_init(c, &io, h, supplier, p));
    assert(!p->set_core_reset(c, 1) && !p->set_lane_clocks(c, 1));
    h->deny_bus = 1;
    assert(!p->set_bus_power(c, 1) && !h->writes);
    h->deny_bus = 0;
    assert(p->set_bus_power(c, 1));
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i) h->reg[i] = 0xa4000100;
}

int main(void)
{
    struct hw h;
    struct piano_ufs_clocks c = {0};
    struct ufs_platform_power p;
    board(&h,&c,&p);
    uint32_t original[PIANO_UFS_CLOCK_REGS];
    h.reg[PIANO_UFS_RX0_GATE] |= 1;            /* An inherited branch. */
    h.reg[PIANO_UFS_RX1_MUX] |= 2;             /* Both remaining muxes use XO. */
    h.reg[PIANO_UFS_TX0_MUX] |= 2;
    memcpy(original,h.reg,sizeof(original));
    assert(p.set_lane_clocks(&c,1) && h.writes==4 && h.waited==20);
    assert(!p.set_bus_power(&c,0) && !p.set_core_reset(&c,1) && h.powered);
    assert(!((h.reg[PIANO_UFS_RX1_MUX] | h.reg[PIANO_UFS_TX0_MUX]) & 3));
    assert(p.set_lane_clocks(&c,1) && h.writes==4);
    assert(p.set_lane_clocks(&c,0) && h.waited==40 && h.writes==8);
    assert(!memcmp(h.reg,original,sizeof(original)));
    h.reg[PIANO_UFS_BCR] |= 1;
    assert(!p.set_core_reset(&c,1) && !p.set_core_reset(&c,0) && h.writes==8);
    h.reg[PIANO_UFS_BCR] &= ~1u;
    h.fail_after_write=PIANO_UFS_BCR;
    assert(!p.set_core_reset(&c,1) && c.reset_uncertain);
    assert(!p.set_bus_power(&c,0) && h.powered);
    h.fail_after_write=h.unreadable=-1;
    assert(p.set_bus_power(&c,0) && !h.powered && !c.reset_owned);
    assert(h.waited >= 1240 && !memcmp(h.reg,original,sizeof(original)));

    board(&h,&c,&p);
    h.reg[PIANO_UFS_TX0_MUX] |= 3;              /* Undefined parent: no writes. */
    assert(!p.set_lane_clocks(&c,1) && !h.writes);
    h.reg[PIANO_UFS_TX0_MUX] &= ~3u;
    h.fail_after_write=PIANO_UFS_RX1_GATE;       /* Uncertain posted write. */
    memcpy(original,h.reg,sizeof(original));
    assert(!p.set_lane_clocks(&c,1) && c.gate_changed && h.powered);
    assert(!p.set_bus_power(&c,0) && h.powered);
    h.fail_after_write=h.unreadable=-1;
    assert(p.set_bus_power(&c,0) && !h.powered && !memcmp(h.reg,original,sizeof(original)));
    board(&h,&c,&p);
    assert(p.set_core_reset(&c,1) && (h.reg[PIANO_UFS_BCR]&1) && !p.set_bus_power(&c,0));
    assert(p.set_core_reset(&c,0) && !(h.reg[PIANO_UFS_BCR]&1));
    h.reg[PIANO_UFS_RX0_GATE] |= 1;
    h.reg[PIANO_UFS_RX1_GATE] |= 1;
    h.reg[PIANO_UFS_TX0_GATE] |= 1;
    unsigned before = h.writes;
    assert(p.set_lane_clocks(&c,1) && p.set_lane_clocks(&c,0) && h.writes==before);
    h.deny_bus=1;
    assert(!p.set_bus_power(&c,0) && c.bus_held);
    h.deny_bus=0;
    assert(p.set_bus_power(&c,0) && !c.bus_held);
    for (unsigned i=0; i<PIANO_UFS_CLOCK_REGS; ++i) printf("%x\n",piano_ufs_gcc_address[i]);
    return 0;
}
