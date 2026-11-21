/* SPDX-License-Identifier: BSD-2-Clause */
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct hardware {
    uint32_t host[0x3000 / 4], phy[0x2000 / 4];
    unsigned writes, resets, lanes;
    int core_asserted, deny_reset_release, deny_lanes, dump;
};

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
        assert(!hw->lanes && !hw->core_asserted);
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
                                              .set_core_reset = cold_reset ? core_reset : NULL };
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

int main(int argc, char **argv)
{
    cold(argc == 2 && !strcmp(argv[1], "calibration"));
    warm();
    conflicts();
    recovery();
    return 0;
}
