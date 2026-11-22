/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_rpmh.h"

/* MiCode gcc-sun: the 403 MHz AXI/UniPro/ICE clock configuration requires
 * RPMH_REGULATOR_LEVEL_NOM on both rails. ufs-qcom requests its maximum
 * fabric bandwidth for discovery, then uses the negotiated link parameters. */
enum { NOM = 256, DDR_INIT = 7643136, CFG_INIT = 819200, HBA_SUPPLIES = (1u << RPMH_UFS_PHY_QREF) - 1 };
enum step { Xo, Mx, Cx, Ddr, Cpu, RefClk, Parent, DeviceRails, Count };

static int change(PIANO_UFS_RPMH *r, enum step step, int on)
{
    PIANO_RPMH_INTERFACE *a = &r->api;
    PVOID ctx = a->Header.Context;
    struct rpmh_client *c = r->client;
    struct rpmh_supply_request req = {0};
    NTSTATUS s;
    switch (step) {
    case Xo: s = a->SetLevel(ctx,c,RPMH_XO,on ? 1 : 0); break;
    case Mx: s = a->SetLevel(ctx,c,RPMH_MX,on ? NOM : 0); break;
    case Cx: s = a->SetLevel(ctx,c,RPMH_CX,on ? NOM : 0); break;
    case Ddr: s = a->SetBandwidth(ctx,c,RPMH_UFS_DDR,0,on ? r->ddr_kBps : 0); break;
    case Cpu: s = a->SetBandwidth(ctx,c,RPMH_CPU_UFS,0,on ? r->cfg_kBps : 0); break;
    case RefClk:
    case Parent:
        req.enable = (uint8_t)on;
        req.ua = on ? (step == RefClk ? 100u : 210000u) : 0;
        s = a->SetAuxSupply(ctx,c,step == RefClk ? RPMH_UFS_REFCLK : RPMH_UFS_VCCQ_PARENT,&req);
        break;
    case DeviceRails: {
        enum rpmh_supply_load load = on ? RPMH_SUPPLY_HIGH : RPMH_SUPPLY_OFF;
        s = a->SetUfsSupplies(ctx,c,load,load);
        break;
    }
    default: return 0;
    }
    return NT_SUCCESS(s);
}

int UfsPianoSetRailsAndPaths(PIANO_UFS_RPMH *r, int on)
{
    if (!r || !r->client) return 0;
    if (!on && (r->phy_held || r->device_clock)) return 0;
    if (!on && r->owns_media && (r->held & (1u << DeviceRails)) &&
        !r->parent.media_may_power_off(r->parent.context)) return 0;
    if (on) {
        if (!r->held) {
            r->ddr_kBps = DDR_INIT;
            r->cfg_kBps = CFG_INIT;
        }
        unsigned rails = (1u << RPMH_XO) | (1u << RPMH_CX) | (1u << RPMH_MX);
        unsigned paths = (1u << RPMH_UFS_DDR) | (1u << RPMH_CPU_UFS);
        if (!NT_SUCCESS(r->api.HoldBootResources(r->api.Header.Context,rails,paths,HBA_SUPPLIES))) return 0;
        for (unsigned i = 0; i < Count; ++i) {
            if (r->held & (1u << i)) continue;
            if (!change(r,(enum step)i,1)) break;
            r->held |= 1u << i;
        }
        if (r->held == (1u << Count)-1) return 1;
    }
    /* A failed release must not remove a parent from underneath the rail
     * still in use. Retry from the first bit not yet returned. */
    for (unsigned i = Count; i; --i) {
        unsigned bit = 1u << (i-1);
        if (!(r->held & bit)) continue;
        if (!change(r,(enum step)(i-1),0)) return 0;
        r->held &= ~bit;
    }
    return !on;
}

int UfsPianoDeviceReference(PIANO_UFS_RPMH *r, int on)
{
    if (!r || !r->client || (on && r->held != (1u << Count)-1)) return 0;
    if (!on && !r->device_clock) return 1;
    PVOID ctx=r->api.Header.Context;
    if (on && !NT_SUCCESS(r->api.HoldBootResources(ctx,1u<<RPMH_UFS_REF_CLOCK,0,0))) return 0;
    if (!NT_SUCCESS(r->api.SetLevel(ctx,r->client,RPMH_UFS_REF_CLOCK,on ? 1u : 0u))) return 0;
    r->device_clock=(BOOLEAN)on;
    return 1;
}

int UfsPianoSetPhySupply(PIANO_UFS_RPMH *r, enum ufs_phy_supply supply, int on)
{
    /* Intersect the UFS PHY's voltage limits with the Piano regulator
     * constraints; do not program a USB or display voltage on their behalf. */
    static const struct {
        enum rpmh_ufs_supply id;
        struct rpmh_supply_request active;
    } phy[] = {
        [UFS_PHY_QREF]   = {RPMH_UFS_PHY_QREF,   {64500,  880,  912, 1}},
        [UFS_PHY_REFGEN] = {RPMH_UFS_PHY_REFGEN, {206000, 880,  920, 1}},
        [UFS_PHY_ANALOG] = {RPMH_UFS_PHY_ANALOG, {213000, 912,  920, 1}},
        [UFS_PHY_PLL]    = {RPMH_UFS_PHY_PLL,    {18300, 1200, 1256, 1}},
    };
    if (!r || !r->client || (unsigned)supply >= UFS_PHY_SUPPLY_COUNT || r->held != (1u << Count)-1)
        return 0;
    enum rpmh_ufs_supply id = phy[supply].id;
    PVOID context = r->api.Header.Context;
    if (on && !NT_SUCCESS(r->api.HoldBootResources(context,0,0,1u << id))) return 0;
    const struct rpmh_supply_request off = {0};
    if (!NT_SUCCESS(r->api.SetAuxSupply(context,r->client,id,on ? &phy[supply].active : &off))) return 0;
    if (on) r->phy_held |= 1u << supply;
    else r->phy_held &= ~(1u << supply);
    return 1;
}

BOOLEAN UfsPianoOpenRpmh(PIANO_UFS_RPMH *r, const PIANO_RPMH_INTERFACE *a,
                         const struct piano_ufs_parent *parent)
{
    if (!r || r->client || !parent || !parent->media_may_power_off ||
        !a || a->Header.Size < sizeof(*a) || a->Header.Version != PIANO_RPMH_INTERFACE_VERSION ||
        !a->Header.InterfaceReference || !a->Header.InterfaceDereference ||
        !a->Open || !a->Close || !a->SetLevel || !a->SetBandwidth || !a->SetUfsSupplies ||
        !a->SetAuxSupply || !a->HoldBootResources || !a->FinishBootResources) return FALSE;
    a->Header.InterfaceReference(a->Header.Context);
    struct rpmh_client *client = NULL;
    if (!NT_SUCCESS(a->Open(a->Header.Context,&client)) || !client) {
        a->Header.InterfaceDereference(a->Header.Context);
        return FALSE;
    }
    *r = (PIANO_UFS_RPMH){.api = *a, .client = client, .parent = *parent,
                          .ddr_kBps = DDR_INIT, .cfg_kBps = CFG_INIT};
    return TRUE;
}

BOOLEAN UfsPianoCloseRpmh(PIANO_UFS_RPMH *r)
{
    if (!r || !r->client) return TRUE;
    if (!UfsPianoSetRailsAndPaths(r,0) || !NT_SUCCESS(r->api.Close(r->api.Header.Context,r->client))) return FALSE;
    r->api.Header.InterfaceDereference(r->api.Header.Context);
    *r = (PIANO_UFS_RPMH){0};
    return TRUE;
}

BOOLEAN UfsPianoSetLinkBandwidth(PIANO_UFS_RPMH *r, enum piano_ufs_link_mode mode,
                                 unsigned tx_gear, unsigned rx_gear, unsigned tx_lanes, unsigned rx_lanes)
{
    static const ULONG hs_ddr[2][5][2] = {
        {{127796,255591},{255591,511181},{1492582,1492582},{2915200,2915200},{5836800,5836800}},
        {{149422,298189},{298189,596378},{1492582,1492582},{2915200,2915200},{5836800,5836800}},
    };
    static const ULONG hs_cfg[5][2] = {
        {1000,1000},{1000,1000},{102400,204800},{204800,409600},{409600,819200},
    };
    if (!r || !r->client || r->held != (1u << Count)-1 || (unsigned)mode > PianoUfsHsB ||
        !tx_gear || tx_gear > 5 || !rx_gear || rx_gear > 5 ||
        !tx_lanes || tx_lanes > 2 || !rx_lanes || rx_lanes > 2) return FALSE;
    unsigned g = max(tx_gear,rx_gear)-1, l = max(tx_lanes,rx_lanes)-1;
    ULONG ddr = mode == PianoUfsPwm ? (922u << (g+l)) : hs_ddr[mode-PianoUfsHsA][g][l];
    ULONG cfg = mode == PianoUfsPwm ? 1000 : hs_cfg[g][l];
    PVOID ctx = r->api.Header.Context;
    ULONG old_ddr = r->ddr_kBps;
    if (!NT_SUCCESS(r->api.SetBandwidth(ctx,r->client,RPMH_UFS_DDR,0,ddr))) return FALSE;
    r->ddr_kBps = ddr;
    if (!NT_SUCCESS(r->api.SetBandwidth(ctx,r->client,RPMH_CPU_UFS,0,cfg))) {
        /* Keep discovery bandwidth when the new pair was not accepted.
         * An already granted increase is sufficient for both modes. */
        if (ddr < old_ddr && NT_SUCCESS(r->api.SetBandwidth(ctx,r->client,RPMH_UFS_DDR,0,old_ddr)))
            r->ddr_kBps = old_ddr;
        return FALSE;
    }
    r->cfg_kBps = cfg;
    return TRUE;
}
