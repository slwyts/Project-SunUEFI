/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Gen8 CM3 address space, in the GMU's view. These are not bus addresses. */
#define ADRENO_GMU_CACHE_BASE UINT32_C(0x4000)

struct adreno_gmu_range {
    uint32_t address, bytes;
};

struct adreno_gmu_layout {
    uint32_t cache_bytes;
    struct adreno_gmu_range hfi;
    size_t reserved_count;
};

enum adreno_gmu_layout_status {
    ADRENO_GMU_LAYOUT_OK, ADRENO_GMU_LAYOUT_NEEDS_SPACE, ADRENO_GMU_LAYOUT_INVALID
};

/* With no storage, reserved_count returns the maximum number of noncached
 * ranges needed. On success they are page-aligned, ordered and disjoint.
 * The cache includes the scatter-loader's destinations, not just file data.
 */
enum adreno_gmu_layout_status adreno_gmu_memory_layout(const void *image, size_t bytes,
        struct adreno_gmu_range *reserved, size_t capacity, struct adreno_gmu_layout *layout);
