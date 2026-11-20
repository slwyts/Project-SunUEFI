/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Addresses in these regions are GMU virtual addresses, not CPU physical addresses. */
struct adreno_gmu_region {
    uint32_t address;
    size_t size;
};

struct adreno_gmu_segment {
    size_t region;
    size_t offset;
    const void *data;
    size_t size;
};

typedef void (*adreno_gmu_write)(void *context, const struct adreno_gmu_segment *segment);

/* Validate the complete image before writing the first register or memory block.
 * A null writer performs the same layout validation without touching hardware.
 */
int adreno_gmu_load(const void *image, size_t bytes,
                    const struct adreno_gmu_region *regions, size_t region_count,
                    adreno_gmu_write write, void *context);
