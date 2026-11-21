/* SPDX-License-Identifier: MIT */
#include "hfi_bandwidth.h"

enum { commit_vote = 1u << 30, valid_vote = 1u << 29, vote_max = 0x3fff };
_Static_assert(sizeof(struct adreno_bw_table) == 159 * sizeof(uint32_t), "Gen8 HFI bandwidth ABI");

int adreno_hfi_bandwidth_table(const struct adreno_bw_config *c, struct adreno_bw_table *t)
{
    static const enum rpmh_bcm_id ids[] = { RPMH_BCM_SH0, RPMH_BCM_MC0, RPMH_BCM_ACV, RPMH_BCM_CN0 };
    if (!c || !t || !c->fabric || !c->kbps || c->count < 2 || c->count > 16 || c->kbps[0])
        return 0;
    for (size_t i = 1; i < c->count; ++i)
        if (c->kbps[i] <= c->kbps[i - 1]) return 0;
    const struct rpmh_bcm *b = c->fabric->bcm;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t addr = b[ids[i]].address;
        if ((addr & 3) || ((addr >> 16) & 7) != RPMH_BCM) return 0;
        if (i < 2 && (!b[ids[i]].width || !b[ids[i]].unit)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (b[ids[j]].address == addr) return 0;
    }
    *t = (struct adreno_bw_table){ .levels = (uint32_t)c->count, .cnoc_count = 1, .ddr_count = 3, .cnoc_wait = 1 };
    t->cnoc_addr[0] = b[RPMH_BCM_CN0].address;
    t->cnoc_data[0][0] = commit_vote;
    t->cnoc_data[1][0] = commit_vote | valid_vote | 1;
    for (unsigned i = 0; i < 3; ++i) {
        const struct rpmh_bcm *res = &b[ids[i]];
        uint32_t commit = i == 2 || res->vcd != b[ids[i + 1]].vcd ? commit_vote : 0;
        t->ddr_addr[i] = res->address;
        t->ddr_data[0][i] = commit;
        if (commit) t->ddr_wait |= 1u << i;
        for (size_t j = 1; j < c->count; ++j) {
            uint32_t xy;
            if (ids[i] == RPMH_BCM_ACV) {
                xy = c->perfmode_kbps && c->kbps[j] >= c->perfmode_kbps ? 1u << 2 : 0;
            } else {
                uint64_t peak = (uint64_t)c->kbps[j] * res->width / (ids[i] == RPMH_BCM_SH0 ? 16u : 4u);
                peak = peak * 1000 / res->unit;
                uint32_t v = peak > vote_max ? vote_max : (peak ? (uint32_t)peak : 1);
                xy = (v << 14) | v;
            }
            t->ddr_data[j][i] = commit | valid_vote | xy;
        }
    }
    return 1;
}
