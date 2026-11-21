/* SPDX-License-Identifier: BSD-2-Clause */
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct hardware {
    uint32_t host[0x3000 / 4], phy[0x2000 / 4];
    unsigned writes, resets, lanes, rails, refclocks, domain_down, log_count, log[64];
    int core_asserted, deny_reset_release, deny_lanes, dump, deny;
};

enum { RAIL_ON = 1, REFCLOCK_ON = 5, DOMAIN_OFF, REFCLOCK_OFF, RAIL_OFF };
static int event(struct hardware *h,unsigned id)
{
    if ((int)id==h->deny) return 0;
    assert(h->log_count<64);
    h->log[h->log_count++]=id;
    return 1;
}
static int supply(void *ctx, enum ufs_phy_supply id, int on)
{
    struct hardware *h=ctx;
    unsigned bit=1u<<id;
    assert(!!(h->rails&bit)!=on);
    if (on) {
        assert(!h->domain_down && !h->refclocks);
        assert(h->rails==bit-1);
        if (id==UFS_PHY_PLL) assert(h->phy[0x404/4]&1);
    } else {
        assert(!h->refclocks);
        if (id==UFS_PHY_ANALOG) assert(!(h->rails & (1u<<UFS_PHY_PLL)));
        if (id==UFS_PHY_QREF || id==UFS_PHY_REFGEN) assert(!(h->rails & (1u<<UFS_PHY_ANALOG)));
    }
    if (!event(h,(on?RAIL_ON:RAIL_OFF)+id)) return 0;
    h->rails ^= bit;
    return 1;
}
static int phy_clocks(void *ctx,int on)
{
    struct hardware *h=ctx;
    assert(h->rails==15 && !h->lanes && h->refclocks!=(unsigned)on);
    if (!event(h,on?REFCLOCK_ON:REFCLOCK_OFF)) return 0;
    h->refclocks=(unsigned)on;return 1;
}
static int domain_off(void *ctx)
{
    struct hardware *h=ctx;
    assert(!h->lanes && !(h->phy[0x404/4]&1) && !h->domain_down);
    assert((h->phy[0x13f0/4]&64) && (h->phy[0x1bf0/4]&64));
    if (!event(h,DOMAIN_OFF)) return 0;
    h->domain_down=1;return 1;
}

static uint32_t *reg(struct hardware *hw, enum ufs_platform_window window, unsigned offset)
{
    assert(!(offset & 3));
    assert(!hw->core_asserted);
    if (window == UFS_HOST) {
        assert(offset < sizeof(hw->host));
        return &hw->host[offset / 4];
    }
    assert(window == UFS_PHY && offset < sizeof(hw->phy));
    return &hw->phy[offset / 4];
}

static uint32_t rd(void *ctx, enum ufs_platform_window w, unsigned off) { return *reg(ctx, w, off); }

static void wr(void *ctx, enum ufs_platform_window w, unsigned off, uint32_t value)
{
    struct hardware *hw = ctx;
    if (w == UFS_PHY && hw->dump) printf("%x %x\n", off, value);
    *reg(hw, w, off) = value;
    ++hw->writes;
}

static int core_reset(void *ctx, int on)
{
    struct hardware *hw = ctx;
    assert(on != hw->core_asserted);
    if (!on && hw->deny_reset_release) return 0;
    hw->core_asserted = on;
    ++hw->resets;
    return 1;
}

static int lanes(void *ctx, int on)
{
    struct hardware *hw = ctx;
    if (on) {
        assert(!hw->lanes && !hw->core_asserted && hw->refclocks && hw->rails==15);
        assert((rd(hw, UFS_PHY, 0x5a8) & 1) && (rd(hw, UFS_HOST, 0xdc) & 3) == 1);
        if (hw->deny_lanes) return 0;
        hw->lanes = 1;
    } else {
        assert(hw->lanes == 1);
        hw->lanes = 0;
    }
    return 1;
}

static void init(struct hardware *h)
{
    memset(h, 0, sizeof(*h));
    h->host[0x08 / 4] = 0x400;
    h->host[0xe4 / 4] = 0x60010000;
    h->host[0xd8 / 4] = 0x60; /* Unrelated bits survive RMW. */
    h->host[0xdc / 4] = 0x80;
}

static enum ufs_platform_error begin(struct ufs_platform *p, struct hardware *h, int cold_reset)
{
    const struct ufs_platform_io io = { .context = h, .read = rd, .write = wr };
    const struct ufs_platform_power power = { .context = h, .set_lane_clocks = lanes,
        .set_core_reset = cold_reset ? core_reset : NULL, .phy={h,supply,phy_clocks,domain_off} };
    return ufs_platform_begin(p, &io, &power, 0);
}

static void calibrate(struct ufs_platform *p, struct hardware *h)
{
    assert(begin(p, h, 1) == UFS_PLATFORM_OK && h->core_asserted);
    assert(ufs_platform_poll(p, 199) == UFS_CORE_RESET && !h->writes);
    assert(ufs_platform_poll(p, 200) == UFS_CORE_SETTLE && !h->core_asserted);
    assert(ufs_platform_poll(p, 1199) == UFS_CORE_SETTLE && !h->writes);
    assert(ufs_platform_poll(p, 1200) == UFS_PHY_RESET && rd(h, UFS_PHY, 0x404) == 1);
    assert(rd(h, UFS_HOST, 0xdc) == 0x82);
    assert(ufs_platform_poll(p, 2199) == UFS_PHY_RESET && !rd(h, UFS_PHY, 0x110));
    assert(ufs_platform_poll(p, 2200) == UFS_PHY_SETTLE && rd(h, UFS_HOST, 0xdc) == 0x80);
    assert(rd(h, UFS_PHY, 0x408) == 1);
    assert(ufs_platform_poll(p, 3199) == UFS_PHY_SETTLE && !rd(h, UFS_PHY, 0x400));
    assert(ufs_platform_poll(p, 3200) == UFS_PHY_START && rd(h, UFS_PHY, 0x400) == 1);
    assert(!rd(h, UFS_PHY, 0x408) && !h->lanes);
}

static void cold(int dump)
{
    struct ufs_platform p = {0};
    struct hardware h;
    init(&h);
    h.dump = dump;
    calibrate(&p, &h);
    assert(ufs_platform_poll(&p, 3400) == UFS_PHY_START && !h.lanes);
    h.phy[0x5a8 / 4] = 1;
    assert(ufs_platform_poll(&p, 3600) == UFS_PLATFORM_READY && h.lanes);
    assert(rd(&h, UFS_HOST, 0xd8) == 0x40 && rd(&h, UFS_HOST, 0xdc) == 0x81);
    h.dump = 0;
    h.host[0x34 / 4] = 1;
    assert(!ufs_platform_stop(&p, 3700) && h.lanes);
    h.host[0x34 / 4] = 2; /* Crypto enable is not HCE. */
    h.host[0x78 / 4] = 1;
    assert(!ufs_platform_stop(&p, 3800) && h.lanes);
    h.host[0x78 / 4] = 0;
    assert(ufs_platform_stop(&p, 3900) && !h.lanes && !rd(&h, UFS_PHY, 0x404));
    assert(ufs_platform_stop(&p, 4000));
}

static void warm(void)
{
    struct ufs_platform p = {0};
    struct hardware h;
    init(&h);
    h.host[0x34 / 4] = 1;
    h.host[0x30 / 4] = 0x10f;
    h.host[0xd8 / 4] = 0x40;
    h.host[0xdc / 4] = 0x81;
    h.phy[0x400 / 4] = h.phy[0x404 / 4] = h.phy[0x5a8 / 4] = 1;
    assert(begin(&p, &h, 0) == UFS_PLATFORM_OK && p.phase == UFS_PLATFORM_READY && h.lanes);
    assert(!h.writes && !h.resets && !ufs_platform_stop(&p, 100));
    h.host[0x34 / 4] = 0;
    assert(ufs_platform_stop(&p, 200) && !h.lanes && !h.writes);
    assert(rd(&h, UFS_PHY, 0x404) == 1);
    assert(begin(&p,&h,0)==UFS_PLATFORM_OK && !h.writes);
    ufs_platform_claim_phy(&p);
    assert(ufs_platform_stop(&p,300) && !rd(&h,UFS_PHY,0x404) && h.domain_down && !h.rails && !h.refclocks);
}

static void conflicts(void)
{
    static const unsigned busy_regs[] = {0x34, 0x30, 0x58, 0x60, 0x78, 0x80, 0x300};
    for (unsigned i = 0; i < sizeof(busy_regs) / sizeof(*busy_regs); ++i) {
        struct ufs_platform p = {0};
        struct hardware h;
        init(&h);
        h.host[busy_regs[i] / 4] = 1;
        assert(begin(&p, &h, 1) == UFS_PLATFORM_IN_USE);
        assert(!h.writes && !h.resets && !h.lanes);
        assert(ufs_platform_stop(&p, 0));
    }
    struct ufs_platform p = {0};
    struct hardware h;
    init(&h);
    h.phy[0x5a8 / 4] = UINT32_MAX;
    assert(begin(&p, &h, 1) == UFS_PLATFORM_HARDWARE && !h.resets && !h.writes);
    assert(ufs_platform_stop(&p, 0));
    h.phy[0x5a8 / 4] = 0;
    assert(begin(&p, &h, 0) == UFS_PLATFORM_POWER && !h.writes);
}

static void recovery(void)
{
    struct ufs_platform p = {0};
    struct hardware h;
    init(&h);
    calibrate(&p, &h);
    assert(ufs_platform_poll(&p, 13199) == UFS_PHY_START && !h.lanes);
    assert(ufs_platform_poll(&p, 13200) == UFS_PLATFORM_FAILED && p.error == UFS_PLATFORM_TIMEOUT);
    assert(ufs_platform_stop(&p, 13201) && !rd(&h, UFS_PHY, 0x404));
    init(&h);
    assert(begin(&p, &h, 1) == UFS_PLATFORM_OK);
    h.deny_reset_release = 1;
    assert(ufs_platform_poll(&p, 200) == UFS_PLATFORM_FAILED && p.error == UFS_PLATFORM_POWER);
    assert(!ufs_platform_stop(&p, 210) && h.core_asserted && !h.writes);
    assert(ufs_platform_poll(&p, 211) == UFS_PLATFORM_STOPPING);
    h.deny_reset_release = 0;
    assert(!ufs_platform_stop(&p, 220) && !h.core_asserted);
    assert(!ufs_platform_stop(&p, 1219));
    assert(ufs_platform_stop(&p, 1220) && !h.writes);
    init(&h);
    assert(begin(&p, &h, 1) == UFS_PLATFORM_OK && h.core_asserted);
    assert(!ufs_platform_stop(&p, 199) && h.core_asserted);
    assert(!ufs_platform_stop(&p, 200) && !h.core_asserted);
    assert(ufs_platform_poll(&p, 1300) == UFS_PLATFORM_STOPPING);
    assert(ufs_platform_stop(&p, 1301) && !h.writes);
    init(&h);
    calibrate(&p, &h);
    h.phy[0x5a8 / 4] = 1;
    h.deny_lanes = 1;
    assert(ufs_platform_poll(&p, 3300) == UFS_PLATFORM_FAILED && p.error == UFS_PLATFORM_POWER);
    assert(!h.lanes && ufs_platform_stop(&p, 3500) && !rd(&h, UFS_PHY, 0x404));
}

static void supply_failures(void)
{
    for (unsigned stage=RAIL_ON;stage<RAIL_OFF+UFS_PHY_SUPPLY_COUNT;++stage) {
        struct ufs_platform p={0};struct hardware h;
        init(&h);h.deny=(int)stage;
        if (stage<RAIL_ON+UFS_PHY_PLL) {
            assert(begin(&p,&h,1)==UFS_PLATFORM_POWER);
        } else {
            assert(begin(&p,&h,1)==UFS_PLATFORM_OK && ufs_platform_poll(&p,200)==UFS_CORE_SETTLE);
            if (stage<=REFCLOCK_ON) assert(ufs_platform_poll(&p,1200)==UFS_PLATFORM_FAILED);
            else {
                assert(ufs_platform_poll(&p,1200)==UFS_PHY_RESET && ufs_platform_poll(&p,2200)==UFS_PHY_SETTLE &&
                       ufs_platform_poll(&p,3200)==UFS_PHY_START);
                h.phy[0x5a8/4]=1;
                assert(ufs_platform_poll(&p,3500)==UFS_PLATFORM_READY);
                assert(!ufs_platform_stop(&p,3600));
                if (stage==DOMAIN_OFF) assert(!h.domain_down && h.refclocks && h.rails==15);
                else assert(h.domain_down && h.rails);
            }
        }
        h.deny=0;
        assert(ufs_platform_stop(&p,4000) && !h.rails && !h.refclocks && !h.lanes);
    }
    struct ufs_platform p={0};struct hardware h;
    init(&h);h.phy[0x13f0/4]=0xa5c0;h.phy[0x1bf0/4]=0x5ac1;
    assert(begin(&p,&h,1)==UFS_PLATFORM_OK && ufs_platform_poll(&p,200)==UFS_CORE_SETTLE);
    assert(ufs_platform_poll(&p,1200)==UFS_PHY_RESET);
    assert(h.phy[0x13f0/4]==0xa580 && h.phy[0x1bf0/4]==0x5a81);
    assert(ufs_platform_stop(&p,3000) && h.phy[0x13f0/4]==0xa5c0 && h.phy[0x1bf0/4]==0x5ac1);
    const unsigned expected[]={RAIL_ON+UFS_PHY_QREF,RAIL_ON+UFS_PHY_REFGEN,RAIL_ON+UFS_PHY_ANALOG,
        RAIL_ON+UFS_PHY_PLL,REFCLOCK_ON,DOMAIN_OFF,REFCLOCK_OFF,RAIL_OFF+UFS_PHY_PLL,RAIL_OFF+UFS_PHY_ANALOG,
        RAIL_OFF+UFS_PHY_QREF,RAIL_OFF+UFS_PHY_REFGEN};
    assert(h.log_count==sizeof(expected)/sizeof(*expected) && !memcmp(h.log,expected,sizeof(expected)));
}

int main(int argc, char **argv)
{
    cold(argc == 2 && !strcmp(argv[1], "calibration"));
    warm();
    conflicts();
    recovery();
    supply_failures();
    return 0;
}
