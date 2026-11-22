/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_power.h"
#include <assert.h>
#include <stdio.h>

enum { HBA=PIANO_UFS_HBA_DOMAIN, PHY=PIANO_UFS_PHY_DOMAIN, UP=1<<16, DOWN=1<<15, RETAIN=1<<11 };
struct hw {
    uint32_t ctrl[2], status[2];
    unsigned writes[2], elapsed, reads;
    int rails, clocks, peer, freeze_up, freeze_down, bad_domain, fail_readback;
    int rail_error, clock_error, phy_quiesced;
};

static uint32_t read_reg(void *ctx, enum piano_ufs_domain d, unsigned offset)
{
    struct hw *h=ctx;
    assert(h->rails && d<PIANO_UFS_DOMAINS && (offset==0 || offset==4));
    ++h->reads;
    if ((int)d==h->bad_domain) return UINT32_MAX;
    return offset ? h->status[d] : h->ctrl[d];
}

static void write_control(void *ctx, enum piano_ufs_domain d, uint32_t value)
{
    struct hw *h=ctx;
    assert(h->rails && d<PIANO_UFS_DOMAINS);
    if (h->clocks) {
        unsigned changed=value ^ h->ctrl[d];
        assert((changed==RETAIN && (value&RETAIN)) ||
               (d==PIANO_UFS_PHY_DOMAIN && h->phy_quiesced && changed==1 && (value&1)));
    }
    /* Never request collapse of a completed, running domain without having
     * enabled retention first. Failed cold starts can still unwind their vote.
     */
    if ((value & 1) && (h->status[d] & UP)) assert(value & RETAIN);
    assert(!((h->ctrl[d]^value) & ~(0xfff000u|RETAIN|1u)));
    if (!(value & 1) && h->freeze_up!=(int)d) h->status[d]=UP;
    if ((value & 1) && h->freeze_down!=(int)d) h->status[d]=d==PIANO_UFS_HBA_DOMAIN && h->peer ? UP : DOWN;
    h->ctrl[d]=value;
    ++h->writes[d];
    if (h->fail_readback==(int)d) h->bad_domain=(int)d;
}

static void wait_us(void *ctx, unsigned delay)
{
    struct hw *h=ctx;
    assert(h->rails);
    h->elapsed+=delay;
}
static int rails(void *ctx, int on)
{
    struct hw *h=ctx;
    assert(!h->clocks && on!=h->rails);
    if (h->rail_error) return 0;
    h->rails=on;
    return 1;
}
static int clocks(void *ctx, int on)
{
    struct hw *h=ctx;
    if (!on && !h->clocks) return 1;
    assert(h->rails && on!=h->clocks);
    assert(h->status[HBA]==UP && !(h->ctrl[HBA]&1));
    if (!on && h->phy_quiesced) assert(h->status[PHY]==DOWN && (h->ctrl[PHY]&1));
    else assert(h->status[PHY]==UP && !(h->ctrl[PHY]&1));
    if (h->clock_error) return 0;
    h->clocks=on;
    return 1;
}

static void setup(struct hw *h, struct piano_ufs_bus_power *p, int warm)
{
    *h=(struct hw){.freeze_up=-1,.freeze_down=-1,.bad_domain=-1,.fail_readback=-1};
    *p=(struct piano_ufs_bus_power){
        .inputs={h,rails,clocks}, .io={h,read_reg,write_control,wait_us},
    };
    for (unsigned d=0; d<PIANO_UFS_DOMAINS; ++d) {
        h->ctrl[d]=0x40000000u | (warm ? RETAIN : 1);
        h->status[d]=warm ? UP : DOWN;
    }
}

int main(void)
{
    struct hw h;
    struct piano_ufs_bus_power p;
    setup(&h,&p,0);
    h.rail_error=1;
    assert(!piano_ufs_set_bus_power(&p,1) && !h.reads && !h.rails);
    h.rail_error=0;
    h.peer=1;
    h.status[HBA]=UP; /* Another hardware master's vote; we still need an AP vote. */
    assert(piano_ufs_set_bus_power(&p,1) && h.clocks && p.drop_mask==3);
    assert((h.ctrl[HBA]&0xfff800u)==(0x22f000u|RETAIN) && (h.ctrl[PHY]&0xfff800u)==(0x222000u|RETAIN));
    unsigned written=h.writes[HBA]+h.writes[PHY];
    assert(piano_ufs_set_bus_power(&p,1) && written==h.writes[HBA]+h.writes[PHY]);
    h.clock_error=1;
    assert(!piano_ufs_set_bus_power(&p,0) && p.clocks_held && p.drop_mask==3 && h.rails);
    h.clock_error=0;
    assert(piano_ufs_set_bus_power(&p,0) && !h.rails && !h.clocks && !p.drop_mask);
    assert((h.ctrl[HBA]&1) && (h.ctrl[PHY]&1) && h.status[HBA]==UP && h.status[PHY]==DOWN && h.elapsed>=500);

    setup(&h,&p,1); /* No reprogramming or collapse of inherited live domains on failed PnP. */
    h.ctrl[HBA]&=~(uint32_t)RETAIN;
    h.ctrl[PHY]&=~(uint32_t)RETAIN;
    h.clock_error=1;
    assert(!piano_ufs_set_bus_power(&p,1) && !h.rails && !h.writes[HBA] && !h.writes[PHY]);
    h.clock_error=0;
    assert(piano_ufs_set_bus_power(&p,1) && !h.writes[HBA] && !h.writes[PHY]);
    assert(piano_ufs_set_bus_power(&p,0) && !h.writes[HBA] && !h.writes[PHY]);
    assert(piano_ufs_set_bus_power(&p,1));
    piano_ufs_claim_domains(&p);
    h.bad_domain=PHY;
    assert(!piano_ufs_set_bus_power(&p,0) && h.clocks && h.rails &&
           !(h.ctrl[HBA]&1) && !(h.ctrl[PHY]&1));
    h.bad_domain=-1;
    assert(piano_ufs_set_bus_power(&p,0) && h.writes[PHY]==2 && h.writes[HBA]==2);

    setup(&h,&p,0);
    h.ctrl[PHY]|=2; /* An active hardware sequencer is not this driver's software vote. */
    assert(!piano_ufs_set_bus_power(&p,1) && !h.rails && !h.writes[PHY] && !p.drop_mask);
    setup(&h,&p,0);
    h.freeze_up=PHY;
    assert(!piano_ufs_set_bus_power(&p,1) && h.elapsed>=2000 && !h.rails && !p.drop_mask);
    setup(&h,&p,0);
    h.fail_readback=PHY;
    assert(!piano_ufs_set_bus_power(&p,1) && h.rails && !h.clocks && p.drop_mask==3);
    h.bad_domain=h.fail_readback=-1;
    assert(piano_ufs_set_bus_power(&p,0) && !p.drop_mask && !h.rails);
    setup(&h,&p,0);
    h.clock_error=1;
    assert(!piano_ufs_set_bus_power(&p,1) && !h.rails && !h.clocks && !p.drop_mask);
    h.clock_error=0;
    assert(piano_ufs_set_bus_power(&p,1));
    h.freeze_down=PHY;
    assert(!piano_ufs_set_bus_power(&p,0) && h.rails && !h.clocks && p.drop_mask==3 && !(h.ctrl[HBA]&1));
    h.freeze_down=-1;
    h.status[PHY]=DOWN;
    h.rail_error=1;
    assert(!piano_ufs_set_bus_power(&p,0) && !p.drop_mask && h.rails);
    h.rail_error=0;
    assert(piano_ufs_set_bus_power(&p,0) && !h.rails);
    setup(&h,&p,1);
    assert(piano_ufs_set_bus_power(&p,1));
    piano_ufs_claim_domains(&p);
    h.freeze_down=PHY;h.phy_quiesced=1;
    assert(!piano_ufs_collapse_phy(&p) && h.clocks && p.drop_mask==3 && !(h.ctrl[HBA]&1));
    assert(!piano_ufs_collapse_phy(&p) && h.clocks && p.drop_mask==3);
    h.freeze_down=-1;h.status[PHY]=DOWN;
    assert(piano_ufs_collapse_phy(&p) && p.drop_mask==1 && h.clocks && h.rails);
    assert(piano_ufs_set_bus_power(&p,0) && !h.clocks && !h.rails);
    for (unsigned d=0; d<PIANO_UFS_DOMAINS; ++d) printf("%x\n",piano_ufs_domain_address[d]);
    return 0;
}
