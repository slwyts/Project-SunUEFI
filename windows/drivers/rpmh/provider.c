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
    static const char *const names[] = { "xo.lvl", "cx.lvl", "mx.lvl" };
    if (!p || !db || !io || !io->write || !slots || slots > RPMH_TCS_MAX_COMMANDS) return 0;
    struct rpmh_provider local = {0};
    local.transport = *io;
    local.slots = slots;
    int found = rpmh_fabric_init(&local.fabric, db) != 0;
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) {
        struct rpmh_db_entry e;
        uint32_t off;
        if (!rpmh_db_find(db, names[i], RPMH_ARC, &e)) continue;
        if (i != RPMH_XO && !rpmh_arc_corner(&e, 0, &off)) continue;
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
        if (!e->address || (i == RPMH_XO ? c > 3 : c >= e->aux_size / 2)) return 0;
        if (i != RPMH_XO && c && !(e->aux[c*2] | e->aux[c*2+1])) return 0;
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
    for (const struct rpmh_client *c = p->clients; c; c = c->next)
        for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
            if (c->corner[i]) p->restore_rails |= BIT(i);
    p->restore_bcms |= rpmh_fabric_bcm_mask(&p->fabric, p->touched_paths);
    p->floor = (struct rpmh_boot_votes){0};
    p->applied = (struct rpmh_boot_votes){0};
    p->adopted = 0;
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
    if (!p->transport.write(p->transport.context,&cmd,1)) return 0;
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
        if (!p->transport.write(p->transport.context,&batch.cmd[first],end - first)) return 0;
        for (unsigned i = first; i < end; ++i)
            p->applied.bus.xy[batch.id[i]] = target.xy[batch.id[i]];
        p->restore_bcms &= ~mask;
        first = end;
    }
    return 1;
}

int rpmh_provider_can_reconcile(const struct rpmh_provider *p)
{
    return p && !(p->restore_rails & ~p->floor.rails) && !(p->restore_bcms & ~p->floor.bus.mask);
}

int rpmh_provider_reconcile(struct rpmh_provider *p)
{
    if (!rpmh_provider_can_reconcile(p)) return 0;
    /* After D3 the bus may need raising from its inherited floor. Restore
     * the supplies it needs before increasing the interconnect request. */
    for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i)
        if ((p->restore_rails & BIT(i)) && !sync_level(p,i)) return 0;
    if (p->restore_bcms && !sync_bus(p)) return 0;
    return 1;
}

int rpmh_set_level(struct rpmh_provider *p, struct rpmh_client *c, enum rpmh_rail rail, uint32_t level)
{
    unsigned i = (unsigned)rail;
    if (!p || !member(p,c) || i >= RPMH_RAIL_COUNT || !(p->floor.rails & BIT(i))) return 0;
    uint32_t target = level ? 3 : 0, old = c->corner[i];
    if (i != RPMH_XO && !rpmh_arc_corner(&p->rail[i],level,&target)) return 0;
    if (!rpmh_provider_reconcile(p)) return 0;
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
    if ((mask & ~p->floor.bus.mask) || !rpmh_provider_reconcile(p)) return 0;
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

int rpmh_client_close(struct rpmh_provider *p, struct rpmh_client *c)
{
    if (!p || !member(p,c) || !rpmh_provider_reconcile(p)) return 0;
    for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i)
        if ((c->avg[i] || c->peak[i]) && !rpmh_set_bandwidth(p,c,(enum rpmh_fabric_path)i,0,0)) return 0;
    static const enum rpmh_rail off[] = {RPMH_CX,RPMH_MX,RPMH_XO};
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
        for (unsigned i = 0; i < RPMH_RAIL_COUNT; ++i) if (c->corner[i]) return 0;
        for (unsigned i = 0; i < RPMH_FABRIC_PATH_COUNT; ++i) if (c->avg[i] || c->peak[i]) return 0;
    }
    return 1;
}
