/* SPDX-License-Identifier: BSD-2-Clause */
#include "fabric.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (1u << (n))
#define VALID BIT(29)
#define COMMIT BIT(30)
#define XY(x, y) ((uint32_t)(x) << 14 | (uint32_t)(y))
enum { N = RPMH_FABRIC_BCM_COUNT, HEAD = 144, ROW = 24, AUX = HEAD + ROW * N, TOTAL = AUX + 8 * N };
static const char *const names[N] = {"ACV","CN0","CN1","MC0","QUP1","SH0","SH1","SN0","SN2"};
static const unsigned vcd[N] = {1,4,4,1,2,3,3,2,2};

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
    for (unsigned i = 0; i < N; ++i) {
        uint8_t *entry = data + HEAD + ROW * i, *aux = data + AUX + 8 * i;
        memcpy(entry, names[i], strlen(names[i]));
        put32(entry + 16, 0x50000 + i * 4);
        put16(entry + 20, 8);
        put16(entry + 22, 8 * i);
        put32(aux, 1000);
        put16(aux + 4, 8);
        aux[6] = (uint8_t)vcd[i];
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
    assert(rpmh_fabric_init(&f, &db) == all);
    assert(rpmh_fabric_bcm_mask(&f, all) == boot.mask);
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_QUP1_CORE)) == BIT(RPMH_BCM_QUP1));
    assert(rpmh_fabric_bcm_mask(&f, BIT(RPMH_CPU_QUP1)) ==
           (BIT(RPMH_BCM_SH1) | BIT(RPMH_BCM_CN0) | BIT(RPMH_BCM_CN1)));

    /* A warm AP already owns an independent ACV bit and a bandwidth floor. */
    boot.xy[RPMH_BCM_ACV] = XY(7, 4);
    boot.xy[RPMH_BCM_SN0] = XY(450, 5);
    const struct rpmh_fabric_request requests[] = {
        { RPMH_UFS_DDR, 160, 320, 0 }, { RPMH_UFS_DDR, 80, 120, 0 },
        { RPMH_QUP1_DDR, 400, 240, 0 }, { RPMH_CPU_UFS, 0, 40, 0 },
        { RPMH_CPU_QUP1, 60, 80, 0 }, { RPMH_QUP1_CORE, 19200, 0, 0 },
    };
    assert(rpmh_fabric_aggregate(&f, all, requests, 6, &boot, &votes));
    /* 640 average, maximum peak 320. BCM auxiliary width is 8;
     * MC0 has 4 channels, each 4 bytes, whereas SH0 has 4 x 16. */
    assert(votes.xy[RPMH_BCM_MC0] == XY(320, 640));
    assert(votes.xy[RPMH_BCM_SH0] == XY(80, 160));
    assert(votes.xy[RPMH_BCM_SN2] == XY(320, 160));
    assert(votes.xy[RPMH_BCM_SN0] == XY(450, 160));
    assert(votes.xy[RPMH_BCM_ACV] == XY(7, 4));
    assert(votes.xy[RPMH_BCM_CN0] == 1 && votes.xy[RPMH_BCM_SH1] == 1);
    assert(votes.xy[RPMH_BCM_CN1] == XY(120, 160));
    assert(votes.xy[RPMH_BCM_QUP1] == XY(38, 0)); /* Scale 1, not 1000. */
    struct rpmh_fabric_load load = {0};
    struct rpmh_fabric_votes linked;
    for (unsigned i = 0; i < 6; ++i) assert(rpmh_fabric_add_request(&load, &requests[i]));
    assert(rpmh_fabric_aggregate_load(&f, all, &load, &boot, &linked) &&
           linked.mask == votes.mask && !memcmp(linked.xy, votes.xy, sizeof(votes.xy)));
    load.avg_kBps[RPMH_UFS_DDR] = UINT64_MAX - 1;
    assert(rpmh_fabric_add_request(&load, requests) && load.avg_kBps[RPMH_UFS_DDR] == UINT64_MAX);
    assert(rpmh_fabric_aggregate_load(&f, all, &load, &boot, &linked) &&
           linked.xy[RPMH_BCM_MC0] == XY(16383, 640));
    struct rpmh_fabric_request perf = {RPMH_UFS_DDR, 1, 1, 1};
    struct rpmh_fabric_votes tiny;
    assert(rpmh_fabric_aggregate(&f, all, &perf, 1, &boot, &tiny));
    assert(tiny.xy[RPMH_BCM_ACV] == XY(7, 6));
    assert(tiny.xy[RPMH_BCM_SH0] == XY(1, 1)); /* Positive but less than one step. */
    perf.avg_kBps = perf.peak_kBps = UINT32_MAX;
    assert(rpmh_fabric_aggregate(&f, all, &perf, 1, &boot, &tiny) && tiny.xy[RPMH_BCM_MC0] == XY(16383, 16383));
    struct rpmh_fabric_votes only_ufs, off;
    assert(rpmh_fabric_aggregate(&f, all, requests, 2, &boot, &only_ufs));
    assert(only_ufs.xy[RPMH_BCM_MC0] == XY(120, 640) && only_ufs.xy[RPMH_BCM_QUP1] == 0);
    assert(rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &off));
    assert(off.xy[RPMH_BCM_MC0] == XY(1, 1) && off.xy[RPMH_BCM_SH0] == XY(1, 1));
    assert(off.xy[RPMH_BCM_CN0] == 1 && off.xy[RPMH_BCM_ACV] == XY(7, 4));
    assert(off.xy[RPMH_BCM_SN0] == XY(450, 5) && !off.xy[RPMH_BCM_SN2] && !off.xy[RPMH_BCM_QUP1]);

    assert(rpmh_fabric_commands(&f, &boot, &votes, 0, 3, &cmds));
    assert(cmds.count == 8 && cmds.groups == 4 && cmds.end[0] == 1 && cmds.end[1] == 4);
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
    assert(!rpmh_fabric_commands(&f, &boot, &votes, 0, 2, &cmds) && cmds.count == 8 && cmds.groups == 4);
    assert(rpmh_fabric_commands(&f, &votes, &votes, 0, 1, &cmds) && !cmds.count && !cmds.groups);
    unsigned replay = BIT(RPMH_BCM_QUP1) | BIT(RPMH_BCM_SN0) | BIT(RPMH_BCM_SN2);
    assert(!rpmh_fabric_commands(&f, &votes, &votes, replay, 2, &cmds) && !cmds.count);
    assert(rpmh_fabric_commands(&f, &votes, &votes, replay, 3, &cmds) && cmds.count == 3 && cmds.groups == 1);
    assert(cmds.id[0] == RPMH_BCM_QUP1 && cmds.id[1] == RPMH_BCM_SN0 && cmds.id[2] == RPMH_BCM_SN2 &&
           !(cmds.cmd[0].data & COMMIT) && !(cmds.cmd[1].data & COMMIT) && (cmds.cmd[2].data & COMMIT));
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
    assert(rpmh_fabric_commands(&f, &partial, &votes, 0, 3, &cmds) && cmds.count == 7 && cmds.groups == 3);
    boot.mask &= ~BIT(RPMH_BCM_SH0);
    assert(!rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &tiny));
    assert(!rpmh_fabric_commands(&f, &boot, &votes, 0, 3, &cmds));

    const unsigned without_core = all & ~BIT(RPMH_QUP1_CORE);
    put32(data + AUX + RPMH_BCM_QUP1 * 8, 0);
    assert(rpmh_fabric_init(&f, &db) == without_core && !rpmh_fabric_bcm_mask(&f, all));
    assert(!rpmh_fabric_aggregate(&f, all, NULL, 0, &boot, &tiny));
    database(data, &db);
    put32(data + HEAD + RPMH_BCM_QUP1 * ROW + 16, 0x50000 + RPMH_BCM_CN1 * 4);
    assert(rpmh_fabric_init(&f, &db) == (without_core & ~BIT(RPMH_CPU_QUP1)));
    puts("Piano BCM shared paths, AP floors, active policy and VCD commits passed.");
    return 0;
}
