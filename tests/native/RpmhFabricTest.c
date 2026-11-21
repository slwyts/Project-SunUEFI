/* SPDX-License-Identifier: BSD-2-Clause */
#include "fabric.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (1u << (n))
#define VALID BIT(29)
#define COMMIT BIT(30)
#define XY(x, y) ((uint32_t)(x) << 14 | (uint32_t)(y))
enum { N = RPMH_FABRIC_BCM_COUNT, HEAD = 144, ROW = 24, AUX = HEAD + ROW * N,
       ARC = AUX + 8 * N, ARC_AUX = ARC + 3 * ROW, TOTAL = ARC_AUX + 3 * 32 };

/* Piano OS3.0.309.0.WPYCNXM AOP: build a minimal directory around the
 * resource metadata, not a snapshot of any boot-time AP votes. */
static const struct {
    const char *name;
    uint32_t address, unit;
    uint16_t width;
    uint8_t vcd;
} bcm[N] = {
    {"ACV",  0x50068,       1,  0, 13},
    {"CN0",  0x50030,  140000,  4,  5},
    {"CN1",  0x50034,  140000,  4,  5},
    {"MC0",  0x50000, 4800000, 16,  0},
    {"QUP1", 0x50044,      20,  4,  8},
    {"SH0",  0x50004, 4800000, 64,  1},
    {"SH1",  0x50008, 4800000, 64,  1},
    {"SN0",  0x50010,  530000, 16,  3},
    {"SN2",  0x50018,  530000, 16,  3},
};
static const struct {
    const char *name;
    uint32_t address;
    uint16_t level[16];
} arc[] = {
    {"cx.lvl", 0x30000, {0, 16, 64, 128, 192, 256, 320, 384, 416, 448, 480}},
    {"mx.lvl", 0x30010, {0, 16, 64, 128, 192, 256, 320, 384, 416, 448}},
    {"xo.lvl", 0x300e0, {0, 32, 80, 128}},
};

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
static void database(uint8_t data[TOTAL], struct rpmh_cmd_db *db)
{
    memset(data, 0, TOTAL);
    put32(data, 1);
    put32(data + 4, 0x0c0330db);
    put16(data + 8, RPMH_BCM);
    put16(data + 12, ROW * N);
    put16(data + 14, N);
    put16(data + 16, 0x1000);
    for (unsigned i = 0; i < N; ++i) {
        uint8_t *entry = data + HEAD + ROW * i, *aux = data + AUX + 8 * i;
        memcpy(entry, bcm[i].name, strlen(bcm[i].name));
        put32(entry + 16, bcm[i].address);
        put16(entry + 20, 8);
        put16(entry + 22, 8 * i);
        put32(aux, bcm[i].unit);
        put16(aux + 4, bcm[i].width);
        aux[6] = bcm[i].vcd;
    }
    put16(data + 24, RPMH_ARC);
    put16(data + 26, ARC - HEAD);
    put16(data + 28, ARC_AUX - HEAD);
    put16(data + 30, 3);
    put16(data + 32, 0x1000);
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *entry = data + ARC + ROW * i;
        memcpy(entry, arc[i].name, strlen(arc[i].name));
        put32(entry + 16, arc[i].address);
        put16(entry + 20, 32);
        put16(entry + 22, 32 * i);
        for (unsigned j = 0; j < 16; ++j)
            put16(data + ARC_AUX + 32 * i + 2 * j, arc[i].level[j]);
    }
    assert(rpmh_db_open(db, data, TOTAL));
}

int main(void)
{
    uint8_t data[TOTAL];
    struct rpmh_cmd_db db;
    struct rpmh_fabric f;
    const unsigned all = RPMH_FABRIC_ALL_PATHS;
    struct rpmh_fabric_votes boot = {.mask = BIT(N) - 1}, votes;
    struct rpmh_fabric_batch cmds;
    database(data, &db);
    const uint32_t requested[] = {480,447,80}, corners[] = {10,9,2};
    for (unsigned i = 0; i < 3; ++i) {
        struct rpmh_db_entry e;
        uint32_t corner;
        assert(rpmh_db_find(&db, arc[i].name, RPMH_ARC, &e) && e.address == arc[i].address);
        assert(rpmh_arc_corner(&e, requested[i], &corner) && corner == corners[i]);
    }
    assert(rpmh_fabric_init(&f, &db) == all);
    assert(f.bcm[RPMH_BCM_ACV].width == 1 && f.bcm[RPMH_BCM_ACV].address == 0x50068);
    const unsigned ddr = BIT(RPMH_BCM_ACV) | BIT(RPMH_BCM_MC0) | BIT(RPMH_BCM_SH0) |
                         BIT(RPMH_BCM_SH1) | BIT(RPMH_BCM_SN0) | BIT(RPMH_BCM_SN2);
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_UFS_DDR)) == ddr);
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_QUP1_DDR)) == ddr);
    assert(rpmh_fabric_bcm_mask(&f, all) == boot.mask);
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_QUP1_CORE)) == BIT(RPMH_BCM_QUP1));
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_CPU_QUP1)) ==
           (BIT(RPMH_BCM_SH1) | BIT(RPMH_BCM_CN0) | BIT(RPMH_BCM_CN1)));

    /* A warm AP already owns an independent ACV bit and a bandwidth floor. */
    boot.xy[RPMH_BCM_ACV] = XY(7, 4);
    boot.xy[RPMH_BCM_SN0] = XY(14000, 5);
    const struct rpmh_fabric_request requests[] = {
        { RPMH_UFS_DDR, 1600000, 3200000, 0 }, { RPMH_UFS_DDR, 800000, 1200000, 0 },
        { RPMH_QUP1_DDR, 4000000, 2400000, 0 }, { RPMH_CPU_UFS, 0, 40000, 0 },
        { RPMH_CPU_QUP1, 60000, 80000, 0 }, { RPMH_QUP1_CORE, 19200, 0, 0 },
    };
    assert(rpmh_fabric_aggregate(&f, all, requests, 6, &boot, &votes));
    /* Both DDR clients share EBI and LLCC. Their average adds to 6400000,
     * while the largest single peak is 3200000 kB/s. */
    assert(votes.xy[RPMH_BCM_MC0] == XY(1333, 2666));
    assert(votes.xy[RPMH_BCM_SH0] == XY(1333, 2666));
    assert(votes.xy[RPMH_BCM_SN2] == XY(12075, 6037));
    assert(votes.xy[RPMH_BCM_SN0] == XY(14000, 6037));
    assert(votes.xy[RPMH_BCM_ACV] == XY(7, 4));
    assert(votes.xy[RPMH_BCM_CN0] == 1 && votes.xy[RPMH_BCM_SH1] == 1);
    assert(votes.xy[RPMH_BCM_CN1] == XY(428, 571));
    assert(votes.xy[RPMH_BCM_QUP1] == XY(960, 0)); /* Scale 1, not 1000. */
    struct rpmh_fabric_load load = {0};
    struct rpmh_fabric_votes linked;
    for (unsigned i = 0; i < 6; ++i) assert(rpmh_fabric_add_request(&load, &requests[i]));
    assert(rpmh_fabric_aggregate_load(&f, all, &load, &boot, &linked) &&
           linked.mask == votes.mask && !memcmp(linked.xy, votes.xy, sizeof(votes.xy)));
    load.avg_kBps[RPMH_UFS_DDR] = UINT64_MAX - 1;
    assert(rpmh_fabric_add_request(&load, requests) && load.avg_kBps[RPMH_UFS_DDR] == UINT64_MAX);
    assert(rpmh_fabric_aggregate_load(&f, all, &load, &boot, &linked) &&
           linked.xy[RPMH_BCM_MC0] == XY(16383, 2666));
    struct rpmh_fabric_request perf = {RPMH_UFS_DDR, 1, 1, 1};
    struct rpmh_fabric_votes tiny;
    assert(rpmh_fabric_aggregate(&f, all, &perf, 1, &boot, &tiny));
    assert(tiny.xy[RPMH_BCM_ACV] == XY(7, 6));
    assert(tiny.xy[RPMH_BCM_SH0] == XY(1, 1)); /* Positive but less than one step. */
    perf.avg_kBps = perf.peak_kBps = UINT32_MAX;
    assert(rpmh_fabric_aggregate(&f, all, &perf, 1, &boot, &tiny) && tiny.xy[RPMH_BCM_MC0] == XY(16383, 16383));
    struct rpmh_fabric_votes only_ufs, off;
    assert(rpmh_fabric_aggregate(&f, all, requests, 2, &boot, &only_ufs));
    assert(only_ufs.xy[RPMH_BCM_MC0] == XY(500, 2666) && only_ufs.xy[RPMH_BCM_QUP1] == 0);
    assert(rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &off));
    assert(off.xy[RPMH_BCM_MC0] == XY(1, 1) && off.xy[RPMH_BCM_SH0] == XY(1, 1));
    assert(off.xy[RPMH_BCM_CN0] == 1 && off.xy[RPMH_BCM_ACV] == XY(7, 4));
    assert(off.xy[RPMH_BCM_SN0] == XY(14000, 5) && !off.xy[RPMH_BCM_SN2] && !off.xy[RPMH_BCM_QUP1]);

    const unsigned order[] = { RPMH_BCM_MC0, RPMH_BCM_SH0, RPMH_BCM_SH1, RPMH_BCM_SN0,
                               RPMH_BCM_SN2, RPMH_BCM_CN0, RPMH_BCM_CN1, RPMH_BCM_QUP1,
                               RPMH_BCM_ACV };
    const unsigned ends[] = {1,3,5,7,8,9}; /* VCDs 0, 1, 3, 5, 8 and 13. */
    assert(rpmh_fabric_commands(&f, &boot, &votes, BIT(RPMH_BCM_ACV), 2, &cmds));
    assert(cmds.count == 9 && cmds.groups == 6 && !memcmp(cmds.id, order, sizeof(order)));
    assert(!memcmp(cmds.end, ends, sizeof(ends)));
    unsigned begin = 0;
    for (unsigned g = 0; g < cmds.groups; ++g) {
        unsigned domain = f.bcm[cmds.id[begin]].vcd;
        assert(!begin || domain > f.bcm[cmds.id[begin - 1]].vcd);
        for (unsigned j = begin; j < cmds.end[g]; ++j) {
            unsigned id = cmds.id[j];
            uint32_t expected = votes.xy[id] | VALID | (j + 1 == cmds.end[g] ? COMMIT : 0);
            assert(cmds.cmd[j].address == f.bcm[id].address && cmds.cmd[j].data == expected);
            assert(f.bcm[id].vcd == domain);
        }
        begin = cmds.end[g];
    }
    assert(begin == cmds.count);
    assert(!rpmh_fabric_commands(&f, &boot, &votes, 0, 1, &cmds) && cmds.count == 9 && cmds.groups == 6);
    assert(rpmh_fabric_commands(&f, &votes, &votes, 0, 1, &cmds) && !cmds.count && !cmds.groups);
    unsigned replay = BIT(RPMH_BCM_QUP1) | BIT(RPMH_BCM_SN0) | BIT(RPMH_BCM_SN2);
    assert(!rpmh_fabric_commands(&f, &votes, &votes, replay, 1, &cmds) && !cmds.count);
    assert(rpmh_fabric_commands(&f, &votes, &votes, replay, 2, &cmds) && cmds.count == 3 && cmds.groups == 2);
    assert(cmds.id[0] == RPMH_BCM_SN0 && cmds.id[1] == RPMH_BCM_SN2 && cmds.id[2] == RPMH_BCM_QUP1 &&
           !(cmds.cmd[0].data & COMMIT) && (cmds.cmd[1].data & COMMIT) && (cmds.cmd[2].data & COMMIT));
    assert(!rpmh_fabric_commands(&f, &votes, &votes, BIT(N), 3, &cmds));
    assert(rpmh_fabric_commands(&f, &votes, &off, 0, 3, &cmds));
    for (unsigned j = 0; j < cmds.count; ++j)
        if (cmds.id[j] == RPMH_BCM_QUP1) assert(!(cmds.cmd[j].data & VALID));
    /* A timed-out group cannot be reported as applied. Replanning after
     * just the first ACK must still include all later domains. */
    assert(rpmh_fabric_commands(&f, &boot, &votes, 0, 3, &cmds));
    struct rpmh_fabric_votes partial = boot;
    for (unsigned j = 0; j < cmds.end[0]; ++j)
        partial.xy[cmds.id[j]] = votes.xy[cmds.id[j]];
    assert(rpmh_fabric_commands(&f, &partial, &votes, 0, 3, &cmds) && cmds.count == 7 && cmds.groups == 4);
    boot.mask &= ~BIT(RPMH_BCM_SH0);
    assert(!rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &tiny));
    assert(!rpmh_fabric_commands(&f, &boot, &votes, 0, 3, &cmds));

    put32(data + AUX + RPMH_BCM_QUP1 * 8, 0);
    put16(data + AUX + RPMH_BCM_QUP1 * 8 + 4, 0);
    assert(rpmh_fabric_init(&f, &db) == all);
    assert(f.bcm[RPMH_BCM_QUP1].unit == 1 && f.bcm[RPMH_BCM_QUP1].width == 1);
    const unsigned without_core = all & ~BIT(RPMH_QUP1_CORE);
    put16(data + HEAD + RPMH_BCM_QUP1 * ROW + 20, 0);
    assert(rpmh_fabric_init(&f, &db) == without_core && !rpmh_fabric_bcm_mask(&f, all));
    assert(!rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &tiny));
    database(data, &db);
    put32(data + HEAD + RPMH_BCM_QUP1 * ROW + 16, bcm[RPMH_BCM_CN1].address);
    assert(rpmh_fabric_init(&f, &db) == (without_core & ~BIT(RPMH_CPU_QUP1)));
    puts("Piano BCM shared paths, AP floors, active policy and VCD commits passed.");
    return 0;
}
