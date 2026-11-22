/* SPDX-License-Identifier: MIT */
#include "hfi_perf_table.h"
#include "hfi_bandwidth.h"
#include <string.h>

/* AnyLaySys' A830 GMU Configure() uses the compact Qualcomm Gen8 performance
 * table. The selected device supplies both OPP lists and all rail corners.
 */
static int unpack_rail(struct adreno_arc_levels *rail, size_t limit)
{
    if (!rail->levels || !rail->count)
        return 0;
    while (rail->count && !rail->levels[rail->count - 1])
        --rail->count; /* Command DB pads auxiliary tables with zero words. */
    if (!rail->count || rail->count > limit)
        return 0;
    for (size_t i = 1; i < rail->count; ++i)
        if (rail->levels[i] < rail->levels[i - 1])
            return 0;
    return 1;
}

static unsigned corner(const struct adreno_arc_levels *rail, uint16_t level, int clamp)
{
    unsigned i = 0;
    while (i < rail->count && rail->levels[i] < level)
        ++i;
    return i == rail->count && clamp ? i - 1 : i;
}

static int arc_vote(const struct adreno_arc_levels *primary, const struct adreno_arc_levels *secondary,
                    uint16_t required, uint32_t *vote, uint16_t *selected)
{
    unsigned index = corner(primary, required, 0);
    if (index == primary->count)
        return 0;
    *selected = primary->levels[index];
    /* Match the voltage the primary rail will actually deliver, not the
     * requested corner. Secondary rails saturate at their highest corner.
     */
    *vote = (uint32_t)*selected << 16 | corner(secondary, *selected, 1) << 8 | index;
    return 1;
}

static int ascending(const struct adreno_opp *opps, size_t count, size_t limit)
{
    if (!opps || !count || count >= limit || !opps[0].khz)
        return 0;
    for (size_t i = 1; i < count; ++i)
        if (opps[i].khz <= opps[i - 1].khz)
            return 0;
    return 1;
}

int adreno_hfi_select_level(const struct adreno_perf_config *p, const struct adreno_bw_config *bw,
                            uint32_t gpu_khz, uint32_t ddr_kbps, struct adreno_hfi_level *level)
{
    if (!level || !p || !bw || !gpu_khz || !ddr_kbps || !bw->kbps || bw->count < 2 ||
        bw->count > ADRENO_HFI_DDR_LEVELS || bw->kbps[0] ||
        !ascending(p->gpu, p->gpu_count, ADRENO_HFI_GPU_LEVELS))
        return 0;
    uint32_t gpu = 0, ddr = 0;
    for (size_t i = 0; i < p->gpu_count; ++i)
        if (p->gpu[i].khz == gpu_khz) gpu = (uint32_t)i + 1;
    /* Only the DDR input already contains the firmware's off entry. */
    for (size_t i = 1; i < bw->count; ++i) {
        if (bw->kbps[i] <= bw->kbps[i - 1]) return 0;
        if (bw->kbps[i] == ddr_kbps) ddr = (uint32_t)i;
    }
    if (!gpu || !ddr) return 0;
    *level = (struct adreno_hfi_level){gpu, UINT32_C(0xffff0000) | ddr};
    return 1;
}

enum adreno_perf_status adreno_hfi_perf_table(uint32_t hfi_version,
        const struct adreno_perf_config *config, struct adreno_perf_table *table)
{
    if (!config || !table)
        return ADRENO_PERF_INVALID;
    if (hfi_version < UINT32_C(0x20060001))
        return ADRENO_PERF_VERSION;
    struct adreno_perf_config p = *config;
    int has_bx = p.gbx.count || p.gbx.levels;
    if (!ascending(p.gpu, p.gpu_count, ADRENO_HFI_GPU_LEVELS) ||
        !ascending(p.gmu, p.gmu_count, ADRENO_HFI_GMU_LEVELS) ||
        !unpack_rail(&p.gfx, 256) || !unpack_rail(&p.gmxc, 256) ||
        !unpack_rail(&p.cx, 256) || !unpack_rail(&p.mx, 64) ||
        (has_bx && !unpack_rail(&p.gbx, 64)))
        return ADRENO_PERF_INVALID;

    uint32_t out[ADRENO_HFI_PERF_WORDS] = {0, 1}; /* table version 0, GPU_PERF */
    size_t at = 2;
    out[at++] = (uint32_t)p.gpu_count + 1;
    out[at++] = 3; /* ARC vote, dependent-rail vote, frequency in kHz */
    out[at++] = 0; out[at++] = UINT32_MAX; out[at++] = 0;
    for (size_t i = 0; i < p.gpu_count; ++i) {
        uint16_t gx;
        if (!arc_vote(&p.gfx, &p.gmxc, p.gpu[i].corner, &out[at], &gx))
            return ADRENO_PERF_NO_CORNER;
        ++at;
        uint32_t dep = UINT32_C(0x0fffc0ff) | corner(&p.mx, p.gpu[i].corner, 1) << 8;
        if (has_bx)
            dep = (dep & ~UINT32_C(0x03f00000)) | (corner(&p.gbx, gx, 1) << 20);
        out[at++] = dep;
        out[at++] = p.gpu[i].khz;
    }
    out[at++] = (uint32_t)p.gmu_count + 1;
    out[at++] = 2; /* ARC vote, frequency in kHz */
    out[at++] = 0; out[at++] = 0;
    for (size_t i = 0; i < p.gmu_count; ++i) {
        uint16_t cx;
        if (!arc_vote(&p.cx, &p.mx, p.gmu[i].corner, &out[at], &cx))
            return ADRENO_PERF_NO_CORNER;
        ++at;
        out[at++] = p.gmu[i].khz;
    }
    memcpy(table->words, out, at * sizeof(*out));
    table->count = at;
    return ADRENO_PERF_OK;
}
