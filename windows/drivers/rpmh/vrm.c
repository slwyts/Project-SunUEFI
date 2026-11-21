/* SPDX-License-Identifier: BSD-2-Clause */
#include "provider.h"

/* Piano's PMIC5 regulator constraints and load thresholds. The resources are
 * discovered by name on every D0, not by their PMIC bus addresses. */
static const struct {
    const char *name;
    unsigned min_mv, max_mv, init_mv, high_ua, threshold_ua, low_mode, boot_mv;
} supplies[] = {
    [RPMH_UFS_VCC]         = {"ldob17", 2504, 2504, 2504, 1300000, 10000, 4, 2504},
    [RPMH_UFS_VCCQ]        = {"ldod1",  1200, 1200, 1200, 1200000, 30000, 4, 1200},
    [RPMH_UFS_REFCLK]      = {"ldoi2",  1200, 1200, 1200,       0, 30000, 3, 1200},
    [RPMH_UFS_VCCQ_PARENT] = {"smpi7",  1224, 1340, 1256,       0,200000, 3, 1340},
    /* DSI and USB can require precisely 880 or 1200 mV on the shared
     * analog rails. Their maximum regulator voltage is not a boot vote. */
    [RPMH_UFS_PHY_QREF]    = {"ldoi3",   880,  912,  880,       0, 30000, 4,  880},
    [RPMH_UFS_PHY_REFGEN]  = {"ldof1",   880,  920,  880,       0, 30000, 4,  880},
    [RPMH_UFS_PHY_ANALOG]  = {"ldoj1",   880,  920,  912,       0, 30000, 4,  912},
    [RPMH_UFS_PHY_PLL]     = {"ldog3",  1200, 1256, 1200,       0, 30000, 4, 1200},
};
enum { MV = 0, ENABLE = 4, MODE = 8, UFS_LPM_UA = 1000, HIGH_MODE = 7 };

int rpmh_vrm_init(struct rpmh_vrm *v, const struct rpmh_cmd_db *db)
{
    *v = (struct rpmh_vrm){0};
    unsigned found = 0;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        struct rpmh_db_entry e;
        if (!rpmh_db_find(db, supplies[i].name, RPMH_VRM, &e) || (e.address & 0xfu)) continue;
        for (unsigned j = 0; j < i; ++j) {
            if (v->address[j] != e.address) continue;
            *v = (struct rpmh_vrm){0};
            return 0;
        }
        v->address[i] = e.address;
        ++found;
    }
    return found != 0;
}

static int equal(struct rpmh_vrm_vote a, struct rpmh_vrm_vote b)
{
    return a.mv == b.mv && a.enable == b.enable && a.mode == b.mode;
}

int rpmh_vrm_adopt(struct rpmh_vrm *v, const struct rpmh_vrm_votes *votes)
{
    if (!votes || !votes->mask || (votes->mask >> RPMH_UFS_SUPPLY_COUNT)) return 0;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        unsigned bit = 1u << i;
        if (!(votes->mask & bit)) continue;
        struct rpmh_vrm_vote raw = votes->vote[i];
        if (!v->address[i] || raw.mv > supplies[i].max_mv || raw.enable > 1 || raw.mode > HIGH_MODE ||
            ((v->floor.mask & bit) && !equal(v->floor.vote[i], raw))) return 0;
    }
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        unsigned bit = 1u << i;
        if ((votes->mask & ~v->floor.mask) & bit)
            v->applied[i] = v->floor.vote[i] = votes->vote[i];
    }
    v->floor.mask |= votes->mask;
    return 1;
}

static int required(const struct rpmh_provider *p, unsigned supply, struct rpmh_vrm_vote *out)
{
    struct rpmh_vrm_vote v = p->vrm.floor.vote[supply];
    unsigned ua = 0, min_mv = 0, max_mv = supplies[supply].max_mv, active = 0;
    for (const struct rpmh_client *c = p->clients; c; c = c->next) {
        uint32_t load;
        if (supply < RPMH_UFS_REFCLK) {
            if (c->ufs[supply] == RPMH_SUPPLY_OFF) continue;
            load = c->ufs[supply] == RPMH_SUPPLY_HIGH ? supplies[supply].high_ua : UFS_LPM_UA;
        } else {
            const struct rpmh_supply_request *r = &c->aux[supply - RPMH_UFS_REFCLK];
            if (r->min_mv > min_mv) min_mv = r->min_mv;
            if (r->max_mv && r->max_mv < max_mv) max_mv = r->max_mv;
            if (!r->enable) continue;
            load = r->ua;
        }
        active = 1;
        if (load > supplies[supply].threshold_ua - ua) ua = supplies[supply].threshold_ua;
        else ua += load;
    }
    if (min_mv > max_mv) return 0;
    /* Before all users register, do not exceed the board's common boot
     * voltage. After a forced D3 the recorded clients constrain that vote. */
    if ((p->vrm.boot_holds & (1u << supply)) && supply >= RPMH_UFS_PHY_QREF &&
        p->vrm.floor.vote[supply].mv < min_mv) return 0;
    if (v.mv > max_mv) {
        /* A boot hold is our new request, not an immutable firmware
         * floor. Choose the highest voltage still permitted by all of
         * this regulator's consumers, including clients surviving D3. */
        if (!(p->vrm.boot_holds & (1u << supply))) return 0;
        v.mv = (uint16_t)max_mv;
    }
    if (min_mv > v.mv) v.mv = (uint16_t)min_mv;
    if (active) {
        unsigned mode = ua >= supplies[supply].threshold_ua ? HIGH_MODE : supplies[supply].low_mode;
        v.enable = 1;
        if (v.mv < supplies[supply].min_mv) v.mv = (uint16_t)supplies[supply].init_mv;
        if (v.mv > max_mv) return 0;
        if (mode > v.mode) v.mode = (uint8_t)mode;
    } else if (p->vrm.managed & (1u << supply)) {
        /* Removing an enable vote is not a request for an invalid 0 mV
         * output. Keep the programmed board defaults (or a voltage-only
         * consumer's range) while this AP no longer enables the supply. */
        if (!v.mv) v.mv = (uint16_t)supplies[supply].init_mv;
        if (!v.mode) v.mode = (uint8_t)supplies[supply].low_mode;
    }
    *out = v;
    return 1;
}

static int apply(struct rpmh_provider *p, unsigned i)
{
    struct rpmh_vrm *v = &p->vrm;
    unsigned bit = 1u << i, force = v->restore & bit;
    if (!(v->floor.mask & bit)) return 0;
    struct rpmh_vrm_vote to, from = v->applied[i];
    if (!required(p, i, &to)) return 0;
    uint32_t base = v->address[i];
    struct rpmh_rsc_command cmd[3];
    unsigned n = 0;
    /* Match the PMIC driver: switch off before programming a lower
     * voltage or mode, and finish configuring them before switching on. */
    if (!to.enable && (force || to.enable != from.enable))
        cmd[n++] = (struct rpmh_rsc_command){base + ENABLE, 0};
    if (force || to.mv != from.mv) cmd[n++] = (struct rpmh_rsc_command){base + MV, to.mv};
    if (force || to.mode != from.mode) cmd[n++] = (struct rpmh_rsc_command){base + MODE, to.mode};
    if (to.enable && (force || to.enable != from.enable))
        cmd[n++] = (struct rpmh_rsc_command){base + ENABLE, 1};
    if (!n) return 1;
    v->restore |= bit;
    for (unsigned first = 0; first < n;) {
        unsigned count = n - first;
        if (count > p->slots) count = p->slots;
        if (!p->transport.write(p->transport.context, cmd + first, count, (1u << count) - 1u)) return 0;
        first += count;
    }
    v->applied[i] = to;
    v->restore &= ~bit;
    return 1;
}

int rpmh_vrm_sync(struct rpmh_provider *p, unsigned mask)
{
    enum rpmh_ufs_supply order[] = { RPMH_UFS_REFCLK, RPMH_UFS_VCCQ_PARENT, RPMH_UFS_VCC, RPMH_UFS_VCCQ,
        RPMH_UFS_PHY_QREF, RPMH_UFS_PHY_REFGEN, RPMH_UFS_PHY_ANALOG, RPMH_UFS_PHY_PLL };
    struct rpmh_vrm_vote to[RPMH_UFS_SUPPLY_COUNT];
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i)
        if ((mask & (1u << i)) && !required(p,i,&to[i])) return 0;
    /* Start parents before dependent rails. On normal media power-down
     * the UFS core releases VCC, then VCCQ. Roll back a failed acquisition
     * in reverse order, including commands with a delayed completion. */
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        unsigned s = order[i];
        if ((mask & (1u << s)) && to[s].enable && !apply(p,s)) return 0;
    }
    if (!p->vrm.rollback) { order[2] = RPMH_UFS_VCCQ; order[3] = RPMH_UFS_VCC; }
    for (unsigned i = RPMH_UFS_SUPPLY_COUNT; i; --i) {
        unsigned s = order[i-1];
        if ((mask & (1u << s)) && !to[s].enable && !apply(p,s)) return 0;
    }
    if (!p->vrm.restore) p->vrm.rollback = 0;
    return 1;
}

void rpmh_vrm_restore_demands(struct rpmh_provider *p, unsigned mask)
{
    /* A preceding rail may have succeeded before the failed one. A later
     * rail which was never attempted needs no write at all. */
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        struct rpmh_vrm_vote to;
        if ((mask & (1u << i)) && required(p,i,&to) && !equal(p->vrm.applied[i], to)) p->vrm.restore |= 1u << i;
    }
    p->vrm.rollback = 1;
}

int rpmh_vrm_set_aux(struct rpmh_provider *p, struct rpmh_client *c, enum rpmh_ufs_supply supply,
                     const struct rpmh_supply_request *r)
{
    unsigned i = (unsigned)supply;
    if (!r || i < RPMH_UFS_REFCLK || i >= RPMH_UFS_SUPPLY_COUNT || r->enable > 1 ||
        (!r->enable && r->ua) || (!!r->min_mv != !!r->max_mv) ||
        (r->min_mv && (r->min_mv < supplies[i].min_mv || r->max_mv > supplies[i].max_mv ||
                        r->min_mv > r->max_mv))) return 0;
    unsigned bit = 1u << i;
    if (!(p->vrm.floor.mask & bit) || (p->vrm.deferred & bit) ||
        !rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm))) return 0;
    struct rpmh_supply_request *entry = &c->aux[i - RPMH_UFS_REFCLK], old = *entry;
    *entry = *r;
    if (rpmh_vrm_sync(p, bit)) return 1;
    *entry = old;
    rpmh_vrm_restore_demands(p, bit);
    (void)rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm));
    return 0;
}

int rpmh_vrm_hold_boot(struct rpmh_provider *p, unsigned mask)
{
    if (!p || !mask || (mask >> RPMH_UFS_SUPPLY_COUNT)) return 0;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i)
        if ((mask & (1u << i)) && !p->vrm.address[i]) return 0;
    unsigned add = mask & ~p->vrm.floor.mask;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        if (!(add & (1u << i))) continue;
        /* A temporary OS request, not an estimate of the old AP vote.
         * Surviving clients may have legitimately negotiated a different
         * voltage before D3; the regulator must honor their bounds too. */
        uint16_t mv = (uint16_t)supplies[i].boot_mv;
        if (i >= RPMH_UFS_PHY_QREF)
            for (const struct rpmh_client *c = p->clients; c; c = c->next)
                if (c->aux[i-RPMH_UFS_REFCLK].min_mv > mv) mv = c->aux[i-RPMH_UFS_REFCLK].min_mv;
        p->vrm.floor.vote[i] = (struct rpmh_vrm_vote){mv, 1, HIGH_MODE};
    }
    p->vrm.floor.mask |= add;
    p->vrm.boot_holds |= add;
    p->vrm.restore |= add;  /* Never mark the new request applied without a hardware reply. */
    p->vrm.deferred &= ~mask;
    return rpmh_vrm_sync(p, mask & p->vrm.restore);
}

int rpmh_vrm_finish_boot(struct rpmh_provider *p, unsigned mask)
{
    if (!p || !mask || (mask & (~p->vrm.floor.mask | p->vrm.deferred))) return 0;
    unsigned drop = mask & p->vrm.boot_holds;
    if (!drop) return 1;
    if (!rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm))) return 0;
    struct rpmh_vrm_votes old = p->vrm.floor;
    unsigned old_managed = p->vrm.managed;
    p->vrm.managed |= drop;
    p->vrm.boot_holds &= ~drop;
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i)
        if (drop & (1u << i)) p->vrm.floor.vote[i] = (struct rpmh_vrm_vote){0};
    if (!rpmh_vrm_sync(p, drop)) {
        p->vrm.floor = old;
        p->vrm.managed = old_managed;
        p->vrm.boot_holds |= drop;
        rpmh_vrm_restore_demands(p, drop);
        (void)rpmh_vrm_sync(p, rpmh_vrm_pending(&p->vrm));
        return 0;
    }
    return 1;
}
