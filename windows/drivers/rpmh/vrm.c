/* SPDX-License-Identifier: BSD-2-Clause */
#include "provider.h"

/* Piano's PMIC5 LDO constraints and load thresholds. The resources are
 * discovered by name on every D0, not by their PMIC bus addresses. */
static const struct { const char *name; unsigned mv, high_ua, threshold_ua; } supplies[] = {
    [RPMH_UFS_VCC]  = {"ldob17", 2504, 1300000, 10000},
    [RPMH_UFS_VCCQ] = {"ldod1",  1200, 1200000, 30000},
};
enum { MV = 0, ENABLE = 4, MODE = 8, UFS_LPM_UA = 1000, LDO_LPM = 4, LDO_HPM = 7 };

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
        if (!v->address[i] || raw.mv > supplies[i].mv || raw.enable > 1 || raw.mode > LDO_HPM ||
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

static struct rpmh_vrm_vote required(const struct rpmh_provider *p, unsigned supply)
{
    struct rpmh_vrm_vote v = p->vrm.floor.vote[supply];
    unsigned ua = 0;
    for (const struct rpmh_client *c = p->clients; c; c = c->next) {
        if (c->ufs[supply] == RPMH_SUPPLY_OFF) continue;
        /* Above the threshold no further load can change the requested mode. */
        ua += c->ufs[supply] == RPMH_SUPPLY_HIGH ? supplies[supply].high_ua : UFS_LPM_UA;
        if (ua >= supplies[supply].threshold_ua) break;
    }
    if (ua) {
        unsigned mode = ua >= supplies[supply].threshold_ua ? LDO_HPM : LDO_LPM;
        v.enable = 1;
        v.mv = (uint16_t)supplies[supply].mv;
        if (mode > v.mode) v.mode = (uint8_t)mode;
    }
    return v;
}

static int apply(struct rpmh_provider *p, unsigned i)
{
    struct rpmh_vrm *v = &p->vrm;
    unsigned bit = 1u << i, force = v->restore & bit;
    if (!(v->floor.mask & bit)) return 0;
    struct rpmh_vrm_vote to = required(p, i), from = v->applied[i];
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
    /* The UFS core toggles VCC first, then VCCQ. The independent VCCQ
     * proxy is just another client and keeps its own enable/load request. */
    enum rpmh_ufs_supply first = RPMH_UFS_VCC, second = RPMH_UFS_VCCQ;
    if (p->vrm.rollback && !required(p, RPMH_UFS_VCC).enable) {
        /* Unwind a partly enabled device in reverse order, including a
         * VCCQ command that took effect after the caller timed out. */
        first = RPMH_UFS_VCCQ;
        second = RPMH_UFS_VCC;
    }
    if ((mask & (1u << first)) && !apply(p, first)) return 0;
    if ((mask & (1u << second)) && !apply(p, second)) return 0;
    if (!p->vrm.restore) p->vrm.rollback = 0;
    return 1;
}

void rpmh_vrm_restore_demands(struct rpmh_provider *p, unsigned mask)
{
    /* A preceding rail may have succeeded before the failed one. A later
     * rail which was never attempted needs no write at all. */
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i)
        if ((mask & (1u << i)) && !equal(p->vrm.applied[i], required(p, i))) p->vrm.restore |= 1u << i;
    p->vrm.rollback = 1;
}
