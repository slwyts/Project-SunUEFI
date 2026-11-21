/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_clock_storport.h"

static uint32_t read_clock(void *ctx, enum piano_ufs_clock_reg reg)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    return StorPortReadRegisterUlong(c->adapter, c->reg[reg]);
}

static void write_clock(void *ctx, enum piano_ufs_clock_reg reg, uint32_t value)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    StorPortWriteRegisterUlong(c->adapter, c->reg[reg], value);
}

static uint32_t read_domain(void *ctx, enum piano_ufs_domain d, unsigned offset)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    return StorPortReadRegisterUlong(c->adapter, c->domain[d] + offset / sizeof(ULONG));
}

static void write_domain(void *ctx, enum piano_ufs_domain d, uint32_t value)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    StorPortWriteRegisterUlong(c->adapter, c->domain[d], value);
}

static void wait_us(void *ctx, unsigned usec)
{
    UNREFERENCED_PARAMETER(ctx);
    while (usec) {
        ULONG step = usec > 50 ? 50 : usec;
        StorPortStallExecution(step);
        usec -= step;
    }
}

static void unmap_controls(PIANO_UFS_CLOCK_DEVICE *c)
{
    if (c->adapter && c->adapter->link_power_context == c) {
        c->adapter->link_power_ready = NULL;
        c->adapter->link_power_context = NULL;
    }
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i)
        if (c->domain[i]) StorPortFreeDeviceBase(c->adapter, c->domain[i]);
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i)
        if (c->reg[i]) StorPortFreeDeviceBase(c->adapter, c->reg[i]);
    RtlZeroMemory(c, sizeof(*c));
}

static void link_power_ready(PVOID context, const uint8_t *p)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    /* Now that the handed-over link has actually served the device, it can
     * be powered down on media shutdown just like a cold-initialized link. */
    ufs_platform_claim_phy(&c->adapter->platform.phy);
    /* Only release the boot votes for exclusive UFS supplies. The display,
     * cameras, USB, audio and radio synchronize their shared rails separately. */
    (void)c->rpmh.api.FinishBootResources(c->rpmh.api.Header.Context,0,0,
        (1u << RPMH_UFS_VCC) | (1u << RPMH_UFS_VCCQ) | (1u << RPMH_UFS_PHY_ANALOG));
    if (!p) return;
    unsigned tx = p[UfsPaPowerMode] & 15, rx = p[UfsPaPowerMode] >> 4;
    if ((tx != 1 && tx != 2 && tx != 4 && tx != 5) ||
        (rx != 1 && rx != 2 && rx != 4 && rx != 5)) return;
    enum piano_ufs_link_mode mode = PianoUfsPwm;
    if (tx == 1 || tx == 4 || rx == 1 || rx == 4) {
        if (p[UfsPaHsSeries] != 1 && p[UfsPaHsSeries] != 2) return;
        mode = p[UfsPaHsSeries] == 2 ? PianoUfsHsB : PianoUfsHsA;
    }
    (void)UfsPianoSetLinkBandwidth(&c->rpmh,mode,p[UfsPaTxGear],p[UfsPaRxGear],p[UfsPaTxLanes],p[UfsPaRxLanes]);
}

static int set_phy_supply(void *context, enum ufs_phy_supply supply, int enable)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    return UfsPianoSetPhySupply(&c->rpmh,supply,enable);
}

static int set_phy_clocks(void *context, int enable)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    return c->rpmh.parent.phy_clocks(c->rpmh.parent.context,enable);
}

static int collapse_phy(void *context)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    return piano_ufs_collapse_phy(&c->power);
}

static PULONG map_resource(UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION cfg, uint32_t address, ULONG bytes)
{
    const ACCESS_RANGE *found = NULL;
    for (ULONG j = 2; j < cfg->NumberOfAccessRanges; ++j) {
        const ACCESS_RANGE *r = &(*cfg->AccessRanges)[j];
        if (r->RangeInMemory && r->RangeLength == bytes && r->RangeStart.QuadPart == address) {
            if (found) return NULL;
            found = r;
        }
    }
    return found ? StorPortGetDeviceBase(a, cfg->AdapterInterfaceType, cfg->SystemIoBusNumber,
                                         found->RangeStart, bytes, FALSE) : NULL;
}

ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *c, UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION cfg,
                          const PIANO_RPMH_INTERFACE *rpmh, const struct piano_ufs_parent *parent)
{
    if (!c || !a || c->adapter ||
        !cfg || !cfg->AccessRanges || cfg->NumberOfAccessRanges < 2 + PIANO_UFS_CLOCK_REGS + PIANO_UFS_DOMAINS)
        return SP_RETURN_BAD_CONFIG;
    struct piano_ufs_clock_io io = { .context = c, .read = read_clock, .write = write_clock, .wait_us = wait_us };
    struct ufs_platform_power power;
    c->power.io = (struct piano_ufs_domain_io){ c, read_domain, write_domain, wait_us };
    if (!piano_ufs_clock_init(&c->clocks, &io, &c->power, piano_ufs_set_bus_power, &power))
        return SP_RETURN_BAD_CONFIG;
    c->adapter = a;
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i) {
        c->reg[i] = map_resource(a, cfg, piano_ufs_gcc_address[i], sizeof(ULONG));
        if (!c->reg[i]) goto failed;
    }
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i) {
        c->domain[i] = map_resource(a, cfg, piano_ufs_domain_address[i], 2 * sizeof(ULONG));
        if (!c->domain[i]) goto failed;
    }
    if (!UfsPianoOpenRpmh(&c->rpmh,rpmh,parent,&c->power.inputs)) goto failed;
    power.phy = (struct ufs_phy_power){c,set_phy_supply,set_phy_clocks,collapse_phy};
    ULONG result = UfsMiniportFindPiano(a, cfg, &power);
    if (result == SP_RETURN_FOUND) {
        piano_ufs_claim_domains(&c->power);
        c->rpmh.owns_media = TRUE;
        a->platform.release_context = parent->context;
        a->platform.media_may_power_off = parent->media_may_power_off;
        a->link_power_context = c;
        a->link_power_ready = link_power_ready;
    } else if (!a->platform.extension && piano_ufs_set_bus_power(&c->power, 0) && UfsPianoCloseRpmh(&c->rpmh))
        unmap_controls(c);
    return result;
failed:
    unmap_controls(c);
    return SP_RETURN_BAD_CONFIG;
}

BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *c, ULONGLONG now_us)
{
    if (!c || !c->adapter) return FALSE;
    if (!UfsPlatformRelease(&c->adapter->platform, now_us) || c->clocks.bus_held ||
        !piano_ufs_set_bus_power(&c->power, 0) || !UfsPianoCloseRpmh(&c->rpmh)) return FALSE;
    unmap_controls(c);
    return TRUE;
}
