/* SPDX-License-Identifier: BSD-2-Clause */
#include "boot.h"
#include <string.h>

enum {
    image_header = 0x7fc0, image_code = 0x8000, boot_reg = 0x5c,
    read_id = 4, read_state = 5, send_header = 6, send_data = 7, verify = 8, start = 9,
    chunk = 503, write_delay_ms = 3
};

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(unsigned char *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static void put_be32(unsigned char *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) p[3 - i] = (unsigned char)(value >> (8 * i));
}

static int write_command(const struct wn8030_boot_io *io, const unsigned char *data,
                         uint32_t bytes, unsigned int extra_ms)
{
    if (!io->write(io->context, data, bytes)) return 0;
    io->delay_ms(io->context, write_delay_ms + extra_ms);
    return 1;
}

static int simple_command(const struct wn8030_boot_io *io, unsigned char command,
                          unsigned int extra_ms)
{
    const unsigned char packet[] = {boot_reg, command};
    return write_command(io, packet, sizeof(packet), extra_ms);
}

static int state(const struct wn8030_boot_io *io, unsigned char command, unsigned char *value)
{
    return simple_command(io, command, 0) && io->read_register(io->context, boot_reg, value);
}

enum wn8030_boot_result wn8030_boot_ram(const struct wn8030_boot_io *io, uint64_t file_bytes)
{
    unsigned char header[image_code - image_header], packet[10 + chunk], value;
    if (file_bytes < image_code) return WN_BOOT_INVALID_IMAGE;
    if (!io->read_image(io->context, image_header, header, sizeof(header))) return WN_BOOT_IO;
    uint32_t size = le32(header + 48);
    if (memcmp(header, "NANO IC", 7) || !size || size % 4 || size > file_bytes - image_code)
        return WN_BOOT_INVALID_IMAGE;

    /* The ROM header uses a little-endian word sum, not the byte sum in
     * the file. Read bounded chunks without allocating the whole image. */
    uint32_t sum = 0;
    for (uint32_t pos = 0; pos < size;) {
        uint32_t n = size - pos < 512 ? size - pos : 512;
        if (!io->read_image(io->context, (uint64_t)image_code + pos, packet, n)) return WN_BOOT_IO;
        for (uint32_t i = 0; i < n; i += 4) sum += le32(packet + i);
        pos += n;
    }

    if (!state(io, read_id, &value)) return WN_BOOT_IO;
    if (value != 0xc8) return WN_BOOT_ID;
    if (!state(io, read_state, &value)) return WN_BOOT_IO;
    if (value != 0) return WN_BOOT_STATE;

    memset(packet, 0, 66);
    packet[0] = boot_reg;
    packet[1] = send_header;
    memcpy(packet + 2, header, 7);
    packet[9] = 0x40;
    packet[10] = 0x81; /* ROM's expected chip ID, distinct from the file ID. */
    put_le32(packet + 11, 0x2345);
    put_le32(packet + 15, 0x40);
    put_le32(packet + 19, size);
    memcpy(packet + 23, header + 21, 10); /* Entry, PID, VID, version. */
    put_le32(packet + 54, sum);
    for (unsigned int i = 2; i < 65; ++i) packet[65] = (unsigned char)(packet[65] + packet[i]);
    if (!write_command(io, packet, 66, 0) || !state(io, read_state, &value)) return WN_BOOT_IO;
    if (value != 1) return WN_BOOT_HEADER_REJECTED;

    packet[0] = boot_reg;
    packet[1] = send_data;
    for (uint32_t pos = 0; pos < size;) {
        uint32_t n = size - pos < chunk ? size - pos : chunk;
        put_be32(packet + 2, pos);
        put_be32(packet + 6, n);
        if (!io->read_image(io->context, (uint64_t)image_code + pos, packet + 10, n))
            return WN_BOOT_IO;
        memset(packet + 10 + n, 0, chunk - n);
        if (!write_command(io, packet, sizeof(packet), 0)) return WN_BOOT_IO;
        pos += n;
    }
    if (!simple_command(io, verify, 10) || !state(io, read_state, &value)) return WN_BOOT_IO;
    if (value != 3) return WN_BOOT_VERIFY_FAILED;
    return simple_command(io, start, 50) ? WN_BOOT_OK : WN_BOOT_IO;
}
