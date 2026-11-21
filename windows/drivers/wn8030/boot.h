/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

struct wn8030_boot_io {
    void *context;
    int (*read_image)(void *context, uint64_t offset, unsigned char *buffer, uint32_t bytes);
    int (*write)(void *context, const unsigned char *data, uint32_t bytes);
    int (*read_register)(void *context, unsigned char reg, unsigned char *value);
    void (*delay_ms)(void *context, unsigned int ms);
};

enum wn8030_boot_result {
    WN_BOOT_OK, WN_BOOT_IO, WN_BOOT_INVALID_IMAGE, WN_BOOT_ID,
    WN_BOOT_STATE, WN_BOOT_HEADER_REJECTED, WN_BOOT_VERIFY_FAILED
};

/* Cold start only. The I2C target must be awake, out of reset and in its
 * boot ROM; keep the same firmware file open for both passes. */
enum wn8030_boot_result wn8030_boot_ram(const struct wn8030_boot_io *io, uint64_t file_bytes);
