/* SPDX-License-Identifier: BSD-2-Clause */
#include "fabric.h"

#define BIT(n) (1u << (n))
#define DDR_PATHS (BIT(RPMH_UFS_DDR) | BIT(RPMH_QUP1_DDR))
#define CFG_PATHS (BIT(RPMH_CPU_UFS) | BIT(RPMH_CPU_QUP1))
#define VOTE_MAX UINT32_C(0x3fff)
#define XY_MASK UINT32_C(0x0fffffff)
#define VALID BIT(29)
#define COMMIT BIT(30)

/* SM8750 HLOS voter. For the two DDR paths all bandwidth-scaled nodes of a
 * given BCM are the same physical node, downstream of the A1NOC junction.
 * Mask BCMs need only know whether any of their nodes has a request.
 * ACV, CN0 and QUP1 follow the Piano vendor tree's active-state policy. */
static const struct bcm_route {
    const char *name;
    unsigned paths, channels, buswidth, scale, keepalive, enable, perf;
} routes[RPMH_FABRIC_BCM_COUNT] = {
    [RPMH_BCM_ACV]  = { "ACV",  DDR_PATHS,              4,  4, 1000, 0, 0, 2 }, /* ebi */
    [RPMH_BCM_CN0]  = { "CN0",  CFG_PATHS,              1,  4, 1000, 1, 1, 0 },
    [RPMH_BCM_CN1]  = { "CN1",  BIT(RPMH_CPU_QUP1),     1,  4, 1000, 0, 0, 0 }, /* qhs_qup1 */
    [RPMH_BCM_MC0]  = { "MC0",  DDR_PATHS,              4,  4, 1000, 1, 0, 0 }, /* ebi */
    [RPMH_BCM_QUP1] = { "QUP1", BIT(RPMH_QUP1_CORE),    1,  4,    1, 0, 0, 0 }, /* qup1_core_slave */
    [RPMH_BCM_SH0]  = { "SH0",  DDR_PATHS,              4, 16, 1000, 1, 0, 0 }, /* qns_llcc */
    [RPMH_BCM_SH1]  = { "SH1",  DDR_PATHS | CFG_PATHS,  1, 16, 1000, 0, 1, 0 },
    [RPMH_BCM_SN0]  = { "SN0",  DDR_PATHS,              1, 16, 1000, 1, 0, 0 }, /* qns_gemnoc_sf */
    [RPMH_BCM_SN2]  = { "SN2",  DDR_PATHS,              1, 16, 1000, 0, 0, 0 }, /* qnm_aggre1_noc */
};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

unsigned rpmh_fabric_init(struct rpmh_fabric *f, const struct rpmh_cmd_db *db)
{
    if (!f) return 0;
    *f = (struct rpmh_fabric){0};
    if (!db) return 0;
    f->paths = RPMH_FABRIC_ALL_PATHS;
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i) {
        struct rpmh_db_entry entry;
        if (!rpmh_db_find(db, routes[i].name, RPMH_BCM, &entry) || entry.aux_size < 8) {
            f->paths &= ~routes[i].paths;
            continue;
        }
        struct rpmh_bcm *b = &f->bcm[i];
        b->address = entry.address;
        b->unit = le32(entry.aux);
        b->width = (uint16_t)(entry.aux[4] | (unsigned)entry.aux[5] << 8);
        b->vcd = entry.aux[6];
        /* Qualcomm normalizes zero factors to one; bit-vote BCMs such as
         * ACV publish no bandwidth width and still belong to the path. */
        if (!b->unit) b->unit = 1;
        if (!b->width) b->width = 1;
        for (unsigned j = 0; j < i; ++j)
            if (f->bcm[j].address == b->address)
                f->paths &= ~(routes[i].paths | routes[j].paths);
    }
    return f->paths;
}

unsigned rpmh_fabric_bcm_mask(const struct rpmh_fabric *f, unsigned paths)
{
    if (!f || (paths & ~f->paths)) return 0;
    unsigned mask = 0;
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i)
        if (routes[i].paths & paths) mask |= BIT(i);
    return mask;
}

static uint64_t add_sat(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static uint64_t mul_sat(uint64_t a, unsigned b)
{
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* Linux bcm_div truncates, but never drops a nonzero request to zero. */
static uint64_t bcm_div(uint64_t value, unsigned by)
{
    return value && value < by ? 1 : value / by;
}

static uint32_t bandwidth(uint64_t kbps, const struct rpmh_bcm *b, unsigned bus, unsigned scale)
{
    uint64_t rate = bcm_div(mul_sat(kbps, b->width), bus);
    rate = bcm_div(mul_sat(rate, scale), b->unit);
    return rate > VOTE_MAX ? VOTE_MAX : (uint32_t)rate;
}

int rpmh_fabric_add_request(struct rpmh_fabric_load *load, const struct rpmh_fabric_request *r)
{
    if (!load || !r || (unsigned)r->path >= RPMH_FABRIC_PATH_COUNT) return 0;
    unsigned p = (unsigned)r->path;
    load->avg_kBps[p] = add_sat(load->avg_kBps[p], r->avg_kBps);
    if (load->peak_kBps[p] < r->peak_kBps) load->peak_kBps[p] = r->peak_kBps;
    if (r->perf_mode && (r->avg_kBps || r->peak_kBps)) load->performance |= BIT(p);
    return 1;
}

int rpmh_fabric_aggregate_load(const struct rpmh_fabric *f, unsigned paths,
                               const struct rpmh_fabric_load *load,
                               const struct rpmh_fabric_votes *floor, struct rpmh_fabric_votes *out)
{
    struct rpmh_fabric_votes result = {0};
    unsigned active = 0;
    if (!f || !floor || !out || !load || (paths & ~f->paths)) return 0;
    for (unsigned p = 0; p < RPMH_FABRIC_PATH_COUNT; ++p)
        if (load->avg_kBps[p] || load->peak_kBps[p]) active |= BIT(p);
    if ((active & ~paths) || (load->performance & ~active)) return 0;
    result.mask = rpmh_fabric_bcm_mask(f, paths);
    if (result.mask & ~floor->mask) return 0;
    for (unsigned i = 0; i < RPMH_FABRIC_BCM_COUNT; ++i) {
        if (!(result.mask & BIT(i))) continue;
        const struct bcm_route *r = &routes[i];
        const struct rpmh_bcm *b = &f->bcm[i];
        uint64_t sum = 0;
        uint32_t max = 0, x = 0, y = 0, base = floor->xy[i];
        if (base & ~XY_MASK) return 0;
        for (unsigned p = 0; p < RPMH_FABRIC_PATH_COUNT; ++p) {
            if (!(r->paths & BIT(p))) continue;
            sum = add_sat(sum, load->avg_kBps[p]);
            if (max < load->peak_kBps[p]) max = load->peak_kBps[p];
        }
        if (r->enable || r->perf) {
            if (sum || max || r->keepalive) y |= r->enable;
            if (load->performance & r->paths) y |= r->perf;
            result.xy[i] = base | y;
        } else {
            x = bandwidth(sum, b, r->buswidth * r->channels, r->scale);
            y = bandwidth(max, b, r->buswidth, r->scale);
            if (!x && !y && r->keepalive) x = y = 1;
            if (x < (base >> 14)) x = base >> 14;
            if (y < (base & VOTE_MAX)) y = base & VOTE_MAX;
            result.xy[i] = (x << 14) | y;
        }
    }
    *out = result;
    return 1;
}

int rpmh_fabric_aggregate(const struct rpmh_fabric *f, unsigned paths,
                          const struct rpmh_fabric_request *requests, size_t count,
                          const struct rpmh_fabric_votes *floor, struct rpmh_fabric_votes *out)
{
    struct rpmh_fabric_load load = {0};
    if (count && !requests) return 0;
    for (size_t i = 0; i < count; ++i)
        if (!rpmh_fabric_add_request(&load, &requests[i]) || !(paths & BIT(requests[i].path))) return 0;
    return rpmh_fabric_aggregate_load(f, paths, &load, floor, out);
}

int rpmh_fabric_commands(const struct rpmh_fabric *f, const struct rpmh_fabric_votes *applied,
                         const struct rpmh_fabric_votes *desired, unsigned force_mask, unsigned slots,
                         struct rpmh_fabric_batch *out)
{
    struct rpmh_fabric_batch batch = {0};
    if (!f || !applied || !desired || !out || !slots || (force_mask & ~desired->mask) ||
        (desired->mask & ~rpmh_fabric_bcm_mask(f, f->paths)) ||
        (desired->mask & ~applied->mask)) return 0;
    for (unsigned id = 0; id < RPMH_FABRIC_BCM_COUNT; ++id) {
        if (!(desired->mask & BIT(id))) continue;
        if ((applied->xy[id] | desired->xy[id]) & ~XY_MASK) return 0;
        if (desired->xy[id] == applied->xy[id] && !(force_mask & BIT(id))) continue;
        unsigned j = batch.count++;
        while (j && f->bcm[id].vcd < f->bcm[batch.id[j - 1]].vcd) {
            batch.id[j] = batch.id[j - 1];
            --j;
        }
        batch.id[j] = id;
    }
    unsigned start = 0;
    for (unsigned j = 0; j < batch.count; ++j) {
        unsigned id = batch.id[j];
        uint32_t xy = desired->xy[id];
        batch.cmd[j] = (struct rpmh_rsc_command){f->bcm[id].address, xy | (xy ? VALID : 0)};
        if (j + 1 == batch.count || f->bcm[id].vcd != f->bcm[batch.id[j + 1]].vcd) {
            if (j + 1 - start > slots) return 0;
            batch.cmd[j].data |= COMMIT;
            start = batch.end[batch.groups++] = j + 1;
        }
    }
    *out = batch;
    return 1;
}
