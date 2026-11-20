/* SPDX-License-Identifier: MIT */
#include "gmu_memory_layout.h"
#include "gmu_firmware.h"
#include "hfi_queue.h"

enum {
    gmu_page = 4096, cache_end = 0x1000000, dtcm = 0x10004000,
    kernel_base = 0x60000000, kernel_size = 0x20000000,
    block_prealloc = 1, block_prealloc_persistent = 7
};
#define KERNEL_EXT_BASE UINT32_C(0xc0000000)

static const struct adreno_gmu_region loadable[] = {
    {0, ADRENO_GMU_CACHE_BASE}, {dtcm, ADRENO_GMU_CACHE_BASE},
    {ADRENO_GMU_CACHE_BASE, cache_end - ADRENO_GMU_CACHE_BASE}
};

static uint32_t page_end(uint32_t end)
{
    return (end + gmu_page - 1) & ~(uint32_t)(gmu_page - 1);
}

static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int inside(uint32_t address, uint32_t bytes, uint32_t base, uint32_t size)
{
    return address >= base && address - base < size && bytes <= size - (address - base);
}

static void cache_size(void *context, const struct adreno_gmu_segment *segment)
{
    struct adreno_gmu_layout *layout = context;
    if (segment->region == 2) {
        uint32_t size = page_end((uint32_t)(segment->offset + segment->size));
        if (size > layout->cache_bytes) layout->cache_bytes = size;
    }
}

/* Merge records sharing a page instead of trying to map an IOVA twice. */
static void insert(struct adreno_gmu_range *r, size_t *count, uint32_t start, uint32_t end)
{
    size_t first = 0;
    while (first < *count && r[first].address + r[first].bytes < start)
        ++first;
    size_t next = first;
    while (next < *count && r[next].address <= end) {
        if (r[next].address < start) start = r[next].address;
        if (r[next].address + r[next].bytes > end) end = r[next].address + r[next].bytes;
        ++next;
    }
    if (next == first) {
        for (size_t i = *count; i > first; --i) r[i] = r[i - 1];
        ++*count;
    } else {
        size_t removed = next - first - 1;
        for (size_t i = next; i < *count; ++i) r[i - removed] = r[i];
        *count -= removed;
    }
    r[first] = (struct adreno_gmu_range){start, end - start};
}

enum adreno_gmu_layout_status adreno_gmu_memory_layout(const void *image, size_t bytes,
        struct adreno_gmu_range *reserved, size_t capacity, struct adreno_gmu_layout *layout)
{
    if (!layout) return ADRENO_GMU_LAYOUT_INVALID;
    *layout = (struct adreno_gmu_layout){0};
    if (!adreno_gmu_load(image, bytes, loadable, sizeof(loadable) / sizeof(*loadable), cache_size, layout))
        return ADRENO_GMU_LAYOUT_INVALID;

    /* Qualcomm places all preallocation requests before the first payload.
     * A first pass sizes the array; the same immutable image is used at Start.
     */
    const unsigned char *p = image;
    size_t count = 0, required = 0;
    for (size_t i = 0; i < bytes && !word(p + i + 4); i += 16) {
        uint32_t type = word(p + i + 8);
        if (type != block_prealloc && type != block_prealloc_persistent) continue;
        uint32_t addr = word(p + i), length = word(p + i + 12);
        if (!length) return ADRENO_GMU_LAYOUT_INVALID;
        if (inside(addr, length, 0, ADRENO_GMU_CACHE_BASE) ||
            inside(addr, length, dtcm, ADRENO_GMU_CACHE_BASE))
            continue;  /* TCM is on the CM3, not system RAM. */
        if (inside(addr, length, ADRENO_GMU_CACHE_BASE, cache_end - ADRENO_GMU_CACHE_BASE)) {
            uint32_t size = page_end(addr + length - ADRENO_GMU_CACHE_BASE);
            if (size > layout->cache_bytes) layout->cache_bytes = size;
            continue;
        }
        if (!inside(addr, length, kernel_base, kernel_size) &&
            !inside(addr, length, KERNEL_EXT_BASE, kernel_size))
            return ADRENO_GMU_LAYOUT_INVALID;
        if (reserved && required < capacity)
            insert(reserved, &count, addr & ~(uint32_t)(gmu_page - 1), page_end(addr + length));
        ++required;
    }
    if (required && (!reserved || capacity < required)) {
        layout->reserved_count = required;
        return ADRENO_GMU_LAYOUT_NEEDS_SPACE;
    }
    layout->reserved_count = count;
    uint32_t hfi = kernel_base;
    uint32_t size = page_end(ADRENO_HFI_SIZE);
    for (size_t i = 0; i < count; ++i) {
        if (reserved[i].address >= hfi + size) break;
        hfi = reserved[i].address + reserved[i].bytes;
    }
    if (!inside(hfi, size, kernel_base, kernel_size))
        return ADRENO_GMU_LAYOUT_INVALID;
    layout->hfi = (struct adreno_gmu_range){hfi, size};
    return ADRENO_GMU_LAYOUT_OK;
}
