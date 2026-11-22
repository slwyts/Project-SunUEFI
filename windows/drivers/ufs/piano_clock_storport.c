/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_clock_storport.h"

/* These are bus addresses in Piano's GCC register descriptions, not
 * Windows physical addresses. The provider applies the relative offset
 * to its translated mapping. TCSR belongs to the storage device instead. */
enum { GCC_BUS_BASE = 0x100000 };

static ULONG read_gcc(PIANO_UFS_CLOCK_DEVICE *c, ULONG address)
{
    return c->gcc.ReadUfs(c->gcc.Header.Context, c, address - GCC_BUS_BASE);
}

static void write_gcc(PIANO_UFS_CLOCK_DEVICE *c, ULONG address, ULONG value)
{
    c->gcc.WriteUfs(c->gcc.Header.Context, c, address - GCC_BUS_BASE, value);
}

static uint32_t read_clock(void *ctx, enum piano_ufs_clock_reg reg)
{
    return read_gcc(ctx, piano_ufs_gcc_address[reg]);
}

static void write_clock(void *ctx, enum piano_ufs_clock_reg reg, uint32_t value)
{
    write_gcc(ctx, piano_ufs_gcc_address[reg], value);
}

static uint32_t read_domain(void *ctx, enum piano_ufs_domain d, unsigned offset)
{
    return read_gcc(ctx, piano_ufs_domain_address[d] + offset);
}

static void write_domain(void *ctx, enum piano_ufs_domain d, uint32_t value)
{
    write_gcc(ctx, piano_ufs_domain_address[d], value);
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

static uint32_t read_base(void *ctx, enum piano_ufs_base_reg r)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    return r == PIANO_QREF ? StorPortReadRegisterUlong(c->adapter, c->qref) :
                             read_gcc(c, piano_ufs_base_address[r]);
}
static void write_base(void *ctx, enum piano_ufs_base_reg r,uint32_t value)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    if (r == PIANO_QREF)
        StorPortWriteRegisterUlong(c->adapter, c->qref, value);
    else
        write_gcc(c, piano_ufs_base_address[r], value);
}
static int set_parent(void *ctx,unsigned root,enum piano_ufs_pll id,int on)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    static const enum sm8750_gcc_pll clocks[]={Sm8750Gpll0,Sm8750Gpll4,Sm8750Gpll0Even};
    enum sm8750_gcc_pll pll=clocks[id];
    return c->gcc.Set(c->gcc.Header.Context,pll,&c->pll[root][pll],(BOOLEAN)on)==STATUS_SUCCESS;
}
static int device_reference(void *ctx,int on)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    return UfsPianoDeviceReference(&c->rpmh,on);
}
static int set_bus_clocks(void *ctx,int on)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    return piano_ufs_bus_clocks(&c->base,on);
}
static int set_rails_and_paths(void *ctx,int on)
{
    PIANO_UFS_CLOCK_DEVICE *c=ctx;
    return UfsPianoSetRailsAndPaths(&c->rpmh,on);
}

static void unmap_controls(PIANO_UFS_CLOCK_DEVICE *c)
{
    if (c->adapter && c->adapter->link_power_context == c) {
        c->adapter->link_power_ready = NULL;
        c->adapter->link_power_begin = NULL;
        c->adapter->can_retain_s0 = NULL;
        c->adapter->link_power_context = NULL;
    }
    if (c->qref) StorPortFreeDeviceBase(c->adapter, c->qref);
    if (c->gcc_open) c->gcc.CloseUfs(c->gcc.Header.Context, c);
    if (c->gcc.Header.InterfaceDereference) c->gcc.Header.InterfaceDereference(c->gcc.Header.Context);
    RtlZeroMemory(c, sizeof(*c));
}

static BOOLEAN begin_link_power(PVOID context)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    return UfsPianoDiscoveryBandwidth(&c->rpmh);
}

static BOOLEAN can_retain_s0(PVOID context)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    return c->adapter->platform.bus_vote && c->adapter->platform.phy.lane_vote && c->power.ready &&
           UfsPianoHasS0Power(&c->rpmh) && c->gcc.UfsPowerRetained(c->gcc.Header.Context, c);
}

static void link_power_ready(PVOID context, const uint8_t *p)
{
    PIANO_UFS_CLOCK_DEVICE *c = context;
    /* Now that the handed-over link has actually served the device, it can
     * be powered down on media shutdown just like a cold-initialized link. */
    ufs_platform_claim_phy(&c->adapter->platform.phy);
    piano_ufs_claim_clocks(&c->base);
    /* Only release the boot votes for exclusive UFS supplies. The display,
     * cameras, USB, audio and radio synchronize their shared rails separately. */
    (void)c->rpmh.api.FinishBootResources(c->rpmh.api.Header.Context,1u<<RPMH_UFS_REF_CLOCK,0,
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
    return piano_ufs_phy_clocks(&c->base,enable);
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
                          const PIANO_RPMH_INTERFACE *rpmh, const SM8750_GCC_INTERFACE *gcc)
{
    if (!c || !a || c->adapter || !gcc || gcc->Header.Size<sizeof(*gcc) ||
        gcc->Header.Version!=SM8750_GCC_INTERFACE_VERSION || !gcc->Header.InterfaceReference ||
        !gcc->Header.InterfaceDereference || !gcc->Set || !gcc->OpenUfs || !gcc->CloseUfs ||
        !gcc->UfsPowerRetained || !gcc->ReadUfs || !gcc->WriteUfs ||
        !cfg || cfg->Length < RTL_SIZEOF_THROUGH_FIELD(PORT_CONFIGURATION_INFORMATION, DumpMode) || cfg->DumpMode ||
        !cfg->AccessRanges || cfg->NumberOfAccessRanges < 3)
        return SP_RETURN_BAD_CONFIG;
    struct piano_ufs_clock_io io = { .context = c, .read = read_clock, .write = write_clock, .wait_us = wait_us };
    struct ufs_platform_power power;
    c->power.io = (struct piano_ufs_domain_io){ c, read_domain, write_domain, wait_us };
    if (!piano_ufs_clock_init(&c->clocks, &io, &c->power, piano_ufs_set_bus_power, &power))
        return SP_RETURN_BAD_CONFIG;
    c->adapter = a;
    gcc->Header.InterfaceReference(gcc->Header.Context);
    c->gcc=*gcc;
    if (gcc->OpenUfs(gcc->Header.Context, c) != STATUS_SUCCESS) goto failed;
    c->gcc_open = TRUE;
    c->qref = map_resource(a, cfg, piano_ufs_base_address[PIANO_QREF], sizeof(ULONG));
    if (!c->qref) goto failed;
    if (!UfsPianoOpenRpmh(&c->rpmh,rpmh,&a->media_off)) goto failed;
    c->base.io=(struct piano_ufs_base_io){c,read_base,write_base,wait_us,set_parent,device_reference};
    c->power.inputs=(struct piano_ufs_bus_inputs){c,set_rails_and_paths,set_bus_clocks};
    power.phy = (struct ufs_phy_power){c,set_phy_supply,set_phy_clocks,collapse_phy};
    ULONG result = UfsMiniportFindPiano(a, cfg, &power);
    if (result == SP_RETURN_FOUND) {
        piano_ufs_claim_domains(&c->power);
        c->rpmh.owns_media = TRUE;
        a->platform.media_off = &a->media_off;
        a->link_power_context = c;
        a->link_power_ready = link_power_ready;
        a->link_power_begin = begin_link_power;
        a->can_retain_s0 = can_retain_s0;
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
