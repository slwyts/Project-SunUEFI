/* SPDX-License-Identifier: MIT */
#include "gmu_firmware.h"

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int adreno_gmu_load(const void *image, size_t bytes,
                    const struct adreno_gmu_region *regions, size_t region_count,
                    adreno_gmu_write write, void *context)
{
    const size_t header = 4 * sizeof(uint32_t);
    const unsigned char *data = image;

    if (!image || !regions || region_count == 0)
        return 0;
    for (unsigned int pass = 0; pass < (write ? 2u : 1u); ++pass) {
        int found_payload = 0;
        for (size_t cursor = 0; cursor < bytes;) {
            if (bytes - cursor < header)
                return 0;
            uint32_t address = le32(data + cursor);
            uint32_t length = le32(data + cursor + 4);
            cursor += header;
            /* Zero-length records describe metadata and version information. */
            if (!length)
                continue;
            if (((length | address) & 3) || length > bytes - cursor ||
                length - 1 > UINT32_MAX - address)
                return 0;

            size_t selected = region_count;
            size_t offset = 0;
            for (size_t i = 0; i < region_count; ++i) {
                if (address < regions[i].address)
                    continue;
                size_t start = address - regions[i].address;
                if (start < regions[i].size && length <= regions[i].size - start) {
                    if (selected != region_count)
                        return 0;
                    selected = i;
                    offset = start;
                }
            }
            if (selected == region_count)
                return 0;
            if (pass) {
                const struct adreno_gmu_segment segment = {
                    selected, offset, data + cursor, length
                };
                write(context, &segment);
            }
            cursor += length;
            found_payload = 1;
        }
        if (!found_payload)
            return 0;
    }
    return 1;
}
