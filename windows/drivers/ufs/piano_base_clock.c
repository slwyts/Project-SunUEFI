/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_base_clock.h"

const uint32_t piano_ufs_base_address[PIANO_UFS_BASE_REGS] = {0x177034, 0x177038, 0x1770a4, 0x1770a8, 0x17708c,
                                                              0x177090, 0x1770c0, 0x1770c4, 0x177018, 0x1770f0,
                                                              0x177028, 0x17706c, 0x17707c, 0x1770bc, 0xf205008};
enum {
    UPDATE = 1,
    ROOT_EN = 2,
    DIV = 31,
    SELECT = 7u << 8,
    MODE = 3u << 12,
    HW_CTRL = 1u << 20,
    CLK_HWC = 2,
    CFG_MASK = DIV | SELECT | MODE | HW_CTRL,
    UNI = 1,
    ICE = 2,
    AUX = 3
};
#define OFF (UINT32_C(1) << 31)
#define BIT(i) (1u << (i))
static const enum piano_ufs_base_reg gate[] = {PIANO_AXI_CBCR, PIANO_AGGRE_CBCR, PIANO_AHB_CBCR, PIANO_UNIPRO_CBCR,
                                               PIANO_ICE_CBCR, PIANO_AUX_CBCR,   PIANO_QREF};

static uint32_t rd(struct piano_ufs_base_clocks *c, enum piano_ufs_base_reg r)
{
    return c->io.read(c->io.context, r);
}

static int bits(struct piano_ufs_base_clocks *c, enum piano_ufs_base_reg r, uint32_t mask, uint32_t value)
{
    uint32_t v = rd(c, r);
    if (v == UINT32_MAX)
        return 0;
    if ((v & mask) == value)
        return 1;
    c->io.write(c->io.context, r, (v & ~mask) | value);
    v = rd(c, r);
    return v != UINT32_MAX && (v & mask) == value;
}

static int wait(struct piano_ufs_base_clocks *c, enum piano_ufs_base_reg r, uint32_t mask, uint32_t value)
{
    for (unsigned i = 0; i <= 500; ++i) {
        uint32_t v = rd(c, r);
        if (v == UINT32_MAX)
            return 0;
        if ((v & mask) == value)
            return 1;
        if (i < 500)
            c->io.wait_us(c->io.context, 1);
    }
    return 0;
}

static int gate_on(struct piano_ufs_base_clocks *c, unsigned i)
{
    uint32_t v = rd(c, gate[i]);
    if (v == UINT32_MAX)
        return 0;
    if (!(v & 1)) {
        c->gates |= BIT(i);
        if (!bits(c, gate[i], 1, 1))
            return 0;
    } else
        c->inherited |= BIT(i);
    if (gate[i] == PIANO_QREF) {
        c->io.wait_us(c->io.context, 10);
        return 1;
    }
    for (unsigned n = 0; n <= 200; ++n) {
        v = rd(c, gate[i]);
        if (v == UINT32_MAX || !(v & 1))
            return 0;
        if ((v & CLK_HWC) || !(v & OFF) || ((v >> 28) & 7) == 2)
            return 1;
        if (n < 200)
            c->io.wait_us(c->io.context, 1);
    }
    return 0;
}

static int gate_off(struct piano_ufs_base_clocks *c, unsigned i)
{
    if (c->gates & BIT(i)) {
        if (!bits(c, gate[i], 1, 0))
            return 0;
        c->io.wait_us(c->io.context, 10); /* Votable: another master can keep it running. */
        c->gates &= ~BIT(i);
    }
    c->inherited &= ~BIT(i);
    return 1;
}

static int prepare_parent(struct piano_ufs_base_clocks *c, unsigned i, uint32_t cfg)
{
    unsigned sel = (cfg & SELECT) >> 8;
    if (!sel)
        return 1; /* The board's independent XO vote is already held. */
    enum piano_ufs_pll pll;
    if (sel == 1)
        pll = PIANO_GPLL0;
    else if (sel == 6)
        pll = PIANO_GPLL0_EVEN;
    else if (sel == 5)
        pll = PIANO_GPLL4;
    else
        return 0;
    unsigned bit = BIT(pll);
    if (c->root[i].parents & bit)
        return 1;
    c->root[i].parents |= bit;
    return c->io.parent(c->io.context, i, pll, 1);
}

static int configure(struct piano_ufs_base_clocks *c, unsigned i, uint32_t cfg)
{
    struct piano_ufs_clock_root *s = &c->root[i];
    enum piano_ufs_base_reg cmd = (enum piano_ufs_base_reg)(i * 2), config = (enum piano_ufs_base_reg)(i * 2 + 1);
    if (!wait(c, cmd, UPDATE, 0))
        return 0;
    uint32_t v = rd(c, cmd);
    if (v == UINT32_MAX)
        return 0;
    if (!(v & ROOT_EN)) {
        s->force = 1;
        if (!bits(c, cmd, ROOT_EN, ROOT_EN))
            return 0;
    }
    if (!wait(c, cmd, OFF, 0) || !bits(c, config, CFG_MASK, cfg))
        return 0;
    v = rd(c, cmd);
    if (v == UINT32_MAX || (v & UPDATE))
        return 0;
    c->io.write(c->io.context, cmd, v | UPDATE);
    if (!wait(c, cmd, UPDATE, 0))
        return 0;
    v = rd(c, config);
    if (v == UINT32_MAX || (v & CFG_MASK) != cfg)
        return 0;
    if (s->force) {
        if (!bits(c, cmd, ROOT_EN, 0))
            return 0;
        s->force = 0;
    }
    return 1;
}

static int get_root(struct piano_ufs_base_clocks *c, unsigned i, unsigned consumers)
{
    struct piano_ufs_clock_root *s = &c->root[i];
    if (s->acquired)
        return 1;
    enum piano_ufs_base_reg cmd = (enum piano_ufs_base_reg)(2 * i);
    if (!wait(c, cmd, UPDATE, 0))
        return 0;
    uint32_t cfg = rd(c, (enum piano_ufs_base_reg)(2 * i + 1)), cv = rd(c, cmd);
    if (cfg == UINT32_MAX || cv == UINT32_MAX)
        return 0;
    int borrowed = !!(cv & ROOT_EN);
    for (unsigned g = 0; g < 6; ++g) {
        if (!(consumers & BIT(g)))
            continue;
        uint32_t v = rd(c, gate[g]);
        if (v == UINT32_MAX)
            return 0;
        borrowed |= !!(v & (1 | CLK_HWC));
    }
    s->saved = cfg & CFG_MASK;
    if (!prepare_parent(c, i, cfg))
        return 0;
    /* An already running HBA may have negotiated a different valid rate.
     * Do not reparent/reset a live firmware link. Cold discovery uses the
     * XBL/DT maximum: GPLL4/2. AUX has no requested rate in Piano's DT. */
    uint32_t target = borrowed   ? s->saved
                      : i == AUX ? ((cfg & DIV) == 3 ? 3u : 1u) | HW_CTRL
                                 : (5u << 8) | 3u | HW_CTRL;
    if (!prepare_parent(c, i, target))
        return 0;
    if (target != s->saved) {
        s->changed = 1;
        if (!configure(c, i, target))
            return 0;
    }
    s->acquired = 1;
    return 1;
}

static int put_root(struct piano_ufs_base_clocks *c, unsigned i)
{
    struct piano_ufs_clock_root *s = &c->root[i];
    if (s->changed) {
        if (!configure(c, i, s->saved))
            return 0;
        s->changed = 0;
    }
    for (unsigned j = PIANO_UFS_PLLS; j; --j) {
        unsigned bit = BIT(j - 1);
        if (!(s->parents & bit))
            continue;
        if (!c->io.parent(c->io.context, i, (enum piano_ufs_pll)(j - 1), 0))
            return 0;
        s->parents &= ~bit;
    }
    *s = (struct piano_ufs_clock_root){0};
    return 1;
}

static int valid(const struct piano_ufs_base_clocks *c)
{
    return c && c->io.read && c->io.write && c->io.wait_us && c->io.parent && c->io.device_reference;
}

int piano_ufs_phy_clocks(struct piano_ufs_base_clocks *c, int on)
{
    if (!valid(c) || (on && !c->bus))
        return 0;
    if (on && c->phy)
        return 1;
    if (on && c->phy_pending && !piano_ufs_phy_clocks(c, 0))
        return 0;
    if (on)
        c->phy_pending = 1;
    if (on && gate_on(c, 6) && get_root(c, AUX, BIT(5)) && gate_on(c, 5)) {
        c->phy = 1;
        return 1;
    }
    c->phy = 0;
    if (!gate_off(c, 5) || !put_root(c, AUX) || !gate_off(c, 6))
        return 0;
    c->phy_pending = 0;
    return !on;
}

int piano_ufs_bus_clocks(struct piano_ufs_base_clocks *c, int on)
{
    if (!valid(c))
        return 0;
    if (on && c->bus)
        return 1;
    if (on && c->bus_pending && !piano_ufs_bus_clocks(c, 0))
        return 0;
    if (on)
        c->bus_pending = 1;
    if (on && get_root(c, 0, BIT(0) | BIT(1)) && gate_on(c, 0) && gate_on(c, 1) && gate_on(c, 2) &&
        get_root(c, UNI, BIT(3)) && gate_on(c, 3) && get_root(c, ICE, BIT(4)) && gate_on(c, 4)) {
        c->reference = 1;
        if (c->io.device_reference(c->io.context, 1)) {
            c->bus = 1;
            return 1;
        }
    }
    if (c->phy_pending)
        return 0;
    c->bus = 0;
    if (c->reference) {
        if (!c->io.device_reference(c->io.context, 0))
            return 0;
        c->reference = 0;
    }
    if (!gate_off(c, 4) || !put_root(c, ICE) || !gate_off(c, 3) || !put_root(c, UNI) || !gate_off(c, 2) ||
        !gate_off(c, 1) || !gate_off(c, 0) || !put_root(c, 0))
        return 0;
    c->bus_pending = 0;
    return !on;
}

void piano_ufs_claim_clocks(struct piano_ufs_base_clocks *c)
{
    if (c && c->bus && c->phy) {
        c->gates |= c->inherited;
        c->inherited = 0;
    }
}
