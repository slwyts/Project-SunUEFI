/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Active frequencies only, in ascending order. The firmware ABI reserves
 * index zero for the powered-off state. Levels are RPMh ARC corners, not uV.
 */
struct adreno_opp {
    uint32_t khz;
    uint16_t corner;
};
struct adreno_arc_levels {
    const uint16_t *levels; /* Decoded little-endian command-db auxiliary data. */
    size_t count;
};
struct adreno_perf_config {
    const struct adreno_opp *gpu, *gmu;
    size_t gpu_count, gmu_count;
    struct adreno_arc_levels gfx, gmxc, cx, mx;
    struct adreno_arc_levels gbx; /* Optional third memory rail. */
};
enum {
    ADRENO_HFI_TABLE = 15, ADRENO_HFI_GPU_LEVELS = 32, ADRENO_HFI_GMU_LEVELS = 4,
    ADRENO_HFI_PERF_WORDS = 6 + 3 * ADRENO_HFI_GPU_LEVELS + 2 * ADRENO_HFI_GMU_LEVELS
};
struct adreno_perf_table {
    size_t count;
    uint32_t words[ADRENO_HFI_PERF_WORDS]; /* Header is supplied by adreno_hfi_send. */
};
struct adreno_bw_config;
struct adreno_hfi_level {
    uint32_t gpu_index, bw_vote;
};
enum adreno_perf_status { ADRENO_PERF_OK, ADRENO_PERF_VERSION, ADRENO_PERF_INVALID, ADRENO_PERF_NO_CORNER };

/* Encode H2F_MSG_TABLE as used by Gen8 HFI 2.6.1 and later. No frequencies,
 * voltage levels, physical addresses or register writes are inferred here.
 */
enum adreno_perf_status adreno_hfi_perf_table(uint32_t hfi_version,
        const struct adreno_perf_config *config, struct adreno_perf_table *table);

/* Select active entries in the separately indexed performance and DDR
 * tables. The HFI vote sets peak bandwidth; average bandwidth stays under
 * the platform's interconnect vote. No rounding to another OPP or bus rate. */
int adreno_hfi_select_level(const struct adreno_perf_config *perf, const struct adreno_bw_config *bandwidth,
                            uint32_t gpu_khz, uint32_t ddr_kbps, struct adreno_hfi_level *level);
