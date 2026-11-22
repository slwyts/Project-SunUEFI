/* SPDX-License-Identifier: BSD-2-Clause */
#include "provider.h"

#define BIT(n) (1u << (n))

static int member(const struct rpmh_provider *p, const struct rpmh_client *c)
{
    for (const struct rpmh_client *it = p->clients; it; it = it->next)
        if (it == c) return 1;
    return 0;
}

int rpmh_provider_start(struct rpmh_provider *p, const struct rpmh_cmd_db *db,
                        const struct rpmh_transport *io, unsigned slots)
{
    static const char *const names[] = { "xo.lvl", "cx.lvl", "mx.lvl", "clka8" };
    if (!p || !db || !io || !io->write || !slots || slots > RPMH_TCS_MAX_COMMANDS) return 0;
    struct rpmh_provider local = {0};
    local.transport = *io;
    local.slots = slots;
    int found = rpmh_fabric_init(&local.fabric, db) != 0;
    found |= rpmh_vrm_init(&local.vrm, db);
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) {
        struct rpmh_db_entry e;
        uint32_t off;
        if (!rpmh_db_find(db,names[i],i==RPMH_UFS_REF_CLOCK ? RPMH_VRM : RPMH_ARC,&e)) continue;
        if (i==RPMH_UFS_REF_CLOCK) {
            /* Sun's RPMH_LN_BB_CLK3 uses the VRM clock enable, never the
             * voltage, mode or headroom registers of a regulator. */
            if (e.address&15u) continue;
            e.address+=4;
        } else if (i!=RPMH_XO && !rpmh_arc_corner(&e,0,&off)) continue;
        for (unsigned j = 0; j < i; ++j)
            if (local.rail[j].address == e.address) return 0;
        local.rail[i] = e;
        found = 1;
    }
    if (found) *p = local;
    return found;
}

int rpmh_provider_adopt(struct rpmh_provider *p, const struct rpmh_boot_votes *v)
{
    if (!p || !v || (!v->rails && !v->bus.mask) || (v->rails >> RPMH_RAIL_COUNT) ||
        (v->bus.mask & ~rpmh_fabric_bcm_mask(&p->fabric, p->fabric.paths))) return 0;
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) {
        if (!(v->rails & BIT(i))) continue;
        const struct rpmh_db_entry *e = &p->rail[i];
        uint32_t c = v->corner[i];
        if (!e->address || (i==RPMH_UFS_REF_CLOCK ? c>1 : i==RPMH_XO ? c>3 : c>=e->aux_size/2)) return 0;
        if ((i==RPMH_CX || i==RPMH_MX) && c && !(e->aux[c*2] | e->aux[c*2+1])) return 0;
        if ((p->floor.rails & BIT(i)) && p->floor.corner[i] != c) return 0;
    }
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i) {
        if (!(v->bus.mask & BIT(i))) continue;
        if (v->bus.xy[i] & ~UINT32_C(0x0fffffff)) return 0;
        if ((p->floor.bus.mask & BIT(i)) && p->floor.bus.xy[i] != v->bus.xy[i]) return 0;
    }
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if ((v->rails & ~p->floor.rails) & BIT(i))
            p->applied.corner[i] = p->floor.corner[i] = v->corner[i];
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i)
        if ((v->bus.mask & ~p->floor.bus.mask) & BIT(i))
            p->applied.bus.xy[i] = p->floor.bus.xy[i] = v->bus.xy[i];
    p->applied.rails = p->floor.rails |= v->rails;
    p->applied.bus.mask = p->floor.bus.mask |= v->bus.mask;
    p->adopted = 1;
    return 1;
}

void rpmh_provider_power_lost(struct rpmh_provider *p)
{
    if (!p) return;
    for (const struct rpmh_client *c = p->clients; c; c = c->next) {
        for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
            if (c->corner[i]) p->restore_rails |= BIT(i);
        for (unsigned i = 0; i < RPMH_UFS_REFCLK; ++i)
            if (c->ufs[i] != RPMH_SUPPLY_OFF) p->vrm.restore |= BIT(i);
        for (unsigned i = RPMH_UFS_REFCLK; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
            const struct rpmh_supply_request *r = &c->aux[i - RPMH_UFS_REFCLK];
            if (r->enable || r->min_mv) p->vrm.restore |= BIT(i);
        }
    }
    p->vrm.floor = (struct rpmh_vrm_votes){0};
    p->deferred_rails=p->restore_rails & BIT(RPMH_UFS_REF_CLOCK);
    p->vrm.boot_holds = p->vrm.managed = 0;
    p->vrm.deferred = p->vrm.restore & ~(BIT(RPMH_UFS_PHY_QREF)-1);
    p->restore_bcms |= rpmh_fabric_bcm_mask(&p->fabric, p->touched_paths);
    p->floor = (struct rpmh_boot_votes){0};
    p->applied = (struct rpmh_boot_votes){0};
    p->adopted = p->boot_rails = p->boot_bcms = 0;
}

int rpmh_client_open(struct rpmh_provider *p, struct rpmh_client *c)
{
    if (!p || !c || member(p,c)) return 0;
    *c = (struct rpmh_client){ .next = p->clients };
    p->clients = c;
    return 1;
}

static int sync_level(struct rpmh_provider *p, unsigned i)
{
    uint32_t value = p->floor.corner[i];
    for (const struct rpmh_client *c = p->clients; c; c = c->next)
        if (value < c->corner[i]) value = c->corner[i];
    if (!(p->restore_rails & BIT(i)) && value == p->applied.corner[i]) return 1;
    struct rpmh_rsc_command cmd = {p->rail[i].address,value};
    /* A lost acknowledgement doesn't prove that the rail stayed put. */
    p->restore_rails |= BIT(i);
    if (!p->transport.write(p->transport.context, &cmd, 1, 1)) return 0;
    p->applied.corner[i] = value;
    p->restore_rails &= ~BIT(i);
    return 1;
}

static int sync_bus(struct rpmh_provider *p)
{
    struct rpmh_fabric_load load = {0};
    struct rpmh_fabric_votes target;
    struct rpmh_fabric_batch batch;
    for (const struct rpmh_client *c = p->clients; c; c = c->next)
        for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i) {
            struct rpmh_fabric_request request = { (enum rpmh_fabric_path)i, c->avg[i], c->peak[i], 0 };
            if (!rpmh_fabric_add_request(&load, &request)) return 0;
        }
    if (!rpmh_fabric_aggregate_load(&p->fabric,p->touched_paths,&load,&p->floor.bus,&target) ||
        !rpmh_fabric_commands(&p->fabric,&p->applied.bus,&target,p->restore_bcms,p->slots,&batch)) return 0;
    for (unsigned g = 0, first = 0; g < batch.groups; ++g) {
        unsigned end = batch.end[g], mask = 0;
        for (unsigned i = first; i < end; ++i) mask |= BIT(batch.id[i]);
        p->restore_bcms |= mask;
        /* Vote for all BCMs in one VCD before waiting on its final commit. */
        unsigned count = end - first;
        if (!p->transport.write(p->transport.context, &batch.cmd[first], count, BIT(count - 1))) return 0;
        for (unsigned i = first; i < end; ++i)
            p->applied.bus.xy[batch.id[i]] = target.xy[batch.id[i]];
        p->restore_bcms &= ~mask;
        first = end;
    }
    return 1;
}

static int platform_floor_ready(const struct rpmh_provider *p)
{
    return !(p->restore_rails & ~p->deferred_rails & ~p->floor.rails) &&
        !(p->restore_bcms & ~p->floor.bus.mask);
}

static int reconcile_platform(struct rpmh_provider *p)
{
    if (!platform_floor_ready(p)) return 0;
    /* After D3 the bus may need raising from its inherited floor. Restore
     * the supplies it needs before increasing the interconnect request. */
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if ((p->restore_rails & ~p->deferred_rails & BIT(i)) && !sync_level(p,i)) return 0;
    if (p->restore_bcms && !sync_bus(p)) return 0;
    return 1;
}

int rpmh_provider_can_reconcile(const struct rpmh_provider *p)
{
    if (!p) return 0;
    int platform_pending = (p->restore_rails & ~p->deferred_rails) || p->restore_bcms;
    return (!p->restore_rails && !p->restore_bcms && !p->vrm.restore) || (platform_pending && platform_floor_ready(p)) ||
        rpmh_vrm_pending(&p->vrm);
}

int rpmh_provider_reconcile(struct rpmh_provider *p)
{
    if (!p) return 0;
    int platform_ok = reconcile_platform(p);
    /* A missing boot value for a separate PMIC supply must not keep an
     * otherwise known interconnect/clock request from being restored. */
    if (!rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm))) return 0;
    return platform_ok && !p->vrm.restore && !p->restore_rails;
}

int rpmh_provider_boot(struct rpmh_provider *p, unsigned rails, unsigned paths, unsigned supplies, int hold)
{
    if (!p || (!rails && !paths && !supplies) || rails >> RPMH_RAIL_COUNT || (paths & ~p->fabric.paths) ||
        supplies >> RPMH_UFS_SUPPLY_COUNT) return 0;
    if (hold) {
        /* After an unexpected D3, other clients still own their accepted
         * requests. On return to this RSC, restore their missing initial
         * votes as well. The next caller need not know every sibling's
         * paths or wait for siblings to probe a second time. */
        rails |= p->restore_rails & ~p->deferred_rails;
        if (p->restore_bcms) paths |= p->touched_paths;
        /* The PHY owner restores its analog supplies at their respective
         * hardware stages. In particular do not enable its PLL before PCS. */
        supplies |= p->vrm.restore & (BIT(RPMH_UFS_PHY_QREF)-1);
    }
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if ((rails & BIT(i)) && !p->rail[i].address) return 0;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i)
        if ((supplies & BIT(i)) && !p->vrm.address[i]) return 0;
    unsigned buses = rpmh_fabric_bcm_mask(&p->fabric,paths);
    if (hold) {
        unsigned add_rails = rails & ~p->floor.rails, add_buses = buses & ~p->floor.bus.mask;
        struct rpmh_fabric_votes base = {.mask = buses}, high;
        struct rpmh_fabric_load load = {0};
        for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i) {
            if (!(paths & BIT(i))) continue;
            /* Keep the numeric and performance-enable resources on this
             * path available until all users have supplied their own votes. */
            load.avg_kBps[i] = load.peak_kBps[i] = INT32_MAX;
            load.performance |= BIT(i);
        }
        if (!rpmh_fabric_aggregate_load(&p->fabric,paths,&load,&base,&high)) return 0;
        for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) {
            if (!(add_rails & BIT(i))) continue;
            /* rpmhpd clamps a domain to its highest firmware corner before
             * sync_state. Ignore the padding after the last real level. */
            unsigned corner = i == RPMH_XO ? 3 : i == RPMH_UFS_REF_CLOCK ? 1 : 0;
            if (i == RPMH_CX || i == RPMH_MX) {
                const struct rpmh_db_entry *e = &p->rail[i];
                for (unsigned k = 1; k < e->aux_size/2; ++k)
                    if (e->aux[2*k] || e->aux[2*k+1]) corner = k;
            }
            p->floor.corner[i] = corner;
        }
        for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i)
            if (add_buses & BIT(i)) p->floor.bus.xy[i] = high.xy[i];
        p->floor.rails |= add_rails; p->applied.rails |= add_rails;
        p->floor.bus.mask |= add_buses; p->applied.bus.mask |= add_buses;
        p->boot_rails |= add_rails; p->restore_rails |= add_rails;
        p->deferred_rails &= ~rails;
        p->boot_bcms |= add_buses; p->restore_bcms |= add_buses;
        p->touched_paths |= paths;
        return reconcile_platform(p) && (!supplies || rpmh_vrm_hold_boot(p,supplies));
    }
    if ((rails & (~p->floor.rails | p->deferred_rails)) || (buses & ~p->floor.bus.mask) ||
        (supplies & ~p->vrm.floor.mask) || !reconcile_platform(p)) return 0;
    unsigned drop_rails = rails & p->boot_rails, drop_buses = buses & p->boot_bcms;
    struct rpmh_boot_votes old = p->floor;
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if (drop_rails & BIT(i)) p->floor.corner[i] = 0;
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i)
        if (drop_buses & BIT(i)) p->floor.bus.xy[i] = 0;
    /* Lower the bus vote before its voltage supplies. On an uncertain
     * completion reassert the previous, already sufficient boot requests. */
    int ok = !drop_buses || sync_bus(p);
    for (unsigned i = RPMH_RAIL_COUNT; ok && i; --i)
        if (drop_rails & BIT(i-1)) ok = sync_level(p,i-1);
    if (!ok) {
        p->floor = old;
        p->restore_rails |= drop_rails;
        p->restore_bcms |= drop_buses;
        (void)reconcile_platform(p);
        return 0;
    }
    p->boot_rails &= ~drop_rails;
    p->boot_bcms &= ~drop_buses;
    return !supplies || rpmh_vrm_finish_boot(p,supplies);
}

int rpmh_set_level(struct rpmh_provider *p, struct rpmh_client *c, enum rpmh_rail rail, uint32_t level)
{
    unsigned i = (unsigned)rail;
    if (!p || !member(p,c) || i >= RPMH_RAIL_COUNT || !(p->floor.rails & BIT(i)) ||
        (p->deferred_rails & BIT(i))) return 0;
    uint32_t target = level ? (i==RPMH_UFS_REF_CLOCK ? 1 : 3) : 0, old = c->corner[i];
    if ((i==RPMH_CX || i==RPMH_MX) && !rpmh_arc_corner(&p->rail[i],level,&target)) return 0;
    if (!reconcile_platform(p)) return 0;
    c->corner[i] = target;
    if (sync_level(p,i)) return 1;
    c->corner[i] = old;
    return 0;
}

int rpmh_set_bandwidth(struct rpmh_provider *p, struct rpmh_client *c, enum rpmh_fabric_path path,
                       uint32_t avg, uint32_t peak)
{
    unsigned i = (unsigned)path;
    if (!p || !member(p,c) || i >= RPMH_FABRIC_PATH_COUNT || !(p->fabric.paths & BIT(i))) return 0;
    unsigned mask = rpmh_fabric_bcm_mask(&p->fabric,BIT(i));
    if ((mask & ~p->floor.bus.mask) || !reconcile_platform(p)) return 0;
    p->touched_paths |= BIT(i);
    uint32_t old_avg = c->avg[i], old_peak = c->peak[i];
    c->avg[i] = avg; c->peak[i] = peak;
    if (sync_bus(p)) return 1;
    c->avg[i] = old_avg; c->peak[i] = old_peak;
    /* Earlier VCDs in this change may have succeeded. Keep the unconfirmed
     * group for forced replay; a normal comparison restores the others. */
    if (p->restore_bcms) (void)sync_bus(p);
    return 0;
}

int rpmh_set_ufs_supplies(struct rpmh_provider *p, struct rpmh_client *c,
                          enum rpmh_supply_load vcc, enum rpmh_supply_load vccq)
{
    if (!p || !member(p, c) || (unsigned)vcc > RPMH_SUPPLY_HIGH || (unsigned)vccq > RPMH_SUPPLY_HIGH) return 0;
    enum rpmh_supply_load old_vcc = c->ufs[RPMH_UFS_VCC], old_vccq = c->ufs[RPMH_UFS_VCCQ];
    unsigned mask = (old_vcc != vcc ? BIT(RPMH_UFS_VCC) : 0u) |
                    (old_vccq != vccq ? BIT(RPMH_UFS_VCCQ) : 0u);
    if ((mask & ~p->vrm.floor.mask) || !rpmh_vrm_sync(p,rpmh_vrm_pending(&p->vrm))) return 0;
    c->ufs[RPMH_UFS_VCC] = vcc;
    c->ufs[RPMH_UFS_VCCQ] = vccq;
    if (rpmh_vrm_sync(p, mask)) return 1;
    c->ufs[RPMH_UFS_VCC] = old_vcc;
    c->ufs[RPMH_UFS_VCCQ] = old_vccq;
    /* Restore any confirmed earlier rail too, not only the TCS whose
     * reply was lost. Leave failed restores pending for the D0 worker. */
    rpmh_vrm_restore_demands(p, mask);
    (void)rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm));
    return 0;
}

int rpmh_set_aux_supply(struct rpmh_provider *p, struct rpmh_client *c, enum rpmh_ufs_supply supply,
                        const struct rpmh_supply_request *r)
{
    return p && member(p,c) && rpmh_vrm_set_aux(p,c,supply,r);
}

int rpmh_client_close(struct rpmh_provider *p, struct rpmh_client *c)
{
    if (!p || !member(p,c)) return 0;
    if ((c->ufs[RPMH_UFS_VCC] || c->ufs[RPMH_UFS_VCCQ]) &&
        !rpmh_set_ufs_supplies(p, c, RPMH_SUPPLY_OFF, RPMH_SUPPLY_OFF)) return 0;
    const struct rpmh_supply_request off_request = {0};
    for (unsigned i = RPMH_UFS_SUPPLY_COUNT; i > RPMH_UFS_REFCLK; --i) {
        const struct rpmh_supply_request *r = &c->aux[i - 1 - RPMH_UFS_REFCLK];
        if ((r->enable || r->min_mv) && !rpmh_set_aux_supply(p,c,(enum rpmh_ufs_supply)(i-1),&off_request)) return 0;
    }
    for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i)
        if ((c->avg[i] || c->peak[i]) && !rpmh_set_bandwidth(p,c,(enum rpmh_fabric_path)i,0,0)) return 0;
    static const enum rpmh_rail off[] = {RPMH_UFS_REF_CLOCK,RPMH_CX,RPMH_MX,RPMH_XO};
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if (c->corner[off[i]] && !rpmh_set_level(p,c,off[i],0)) return 0;
    struct rpmh_client **entry = &p->clients;
    while (*entry != c) entry = &(*entry)->next;
    *entry = c->next;
    c->next = NULL;
    return 1;
}

int rpmh_provider_quiet(struct rpmh_provider *p)
{
    if (!rpmh_provider_reconcile(p)) return 0;
    for (const struct rpmh_client *c = p->clients; c; c = c->next) {
        for (unsigned i = 0; i < RPMH_UFS_REFCLK; ++i) if (c->ufs[i] != RPMH_SUPPLY_OFF) return 0;
        for (unsigned i = RPMH_UFS_REFCLK; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
            const struct rpmh_supply_request *r = &c->aux[i - RPMH_UFS_REFCLK];
            if (r->enable || r->min_mv) return 0;
        }
        for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) if (c->corner[i]) return 0;
        for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i) if (c->avg[i] || c->peak[i]) return 0;
    }
    return 1;
}
