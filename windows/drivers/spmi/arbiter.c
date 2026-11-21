/* SPDX-License-Identifier: BSD-2-Clause */
#include "arbiter.h"

enum {
    VERSION = 0, FEATURES = 4, CHANNEL_MAP = 0x2000, IRQ_OWNER = 1 << 24, APID_LIMIT = 1024,
    REG_COMMAND = 0, REG_STATUS = 8, WRITE_DATA = 0x10, READ_DATA = 0x18,
    DONE = 1, FAILURE = 2, DENIED = 4, DROPPED = 8, EXT_READ_LONG = 1 << 27,
    TRANSACTION_BYTES = 8, TIMEOUT_US = 1000
};

enum spmi_status spmi_v7_start(struct spmi_arbiter *a, const struct spmi_register_io *io,
                               unsigned bus, unsigned ee)
{
    if (!a || !io || !io->read || !io->write || !io->delay_us || bus > 1 || ee > 7) return SPMI_INVALID;
    if (a->pending) return SPMI_BUSY;
    uint32_t version, value;
    if (!io->read(io->context, SPMI_CORE, VERSION, &version)) return SPMI_IO_ERROR;
    if (version >> 28 != 7) return SPMI_NOT_FOUND;
    unsigned first = 0, count = 0;
    for (unsigned i = 0; i <= bus; ++i) {
        first += count;
        if (!io->read(io->context, SPMI_CORE, FEATURES + 4 * i, &value)) return SPMI_IO_ERROR;
        count = value & 0x7ff;
        if (!count || first + count > APID_LIMIT) return SPMI_INVALID;
    }
    *a = (struct spmi_arbiter){ .io = *io, .first = (uint16_t)first, .count = (uint16_t)count, .ee = (uint8_t)ee };
    return SPMI_OK;
}

enum spmi_status spmi_v7_resolve(const struct spmi_arbiter *a, uint16_t ppid, struct spmi_peripheral *out)
{
    if (!a || !a->count || !out || ppid > 0xfff) return SPMI_INVALID;
    struct spmi_peripheral p = { ppid, SPMI_NO_APID, SPMI_NO_APID, SPMI_NO_APID };
    for (unsigned i = a->first; i < (unsigned)a->first + a->count; ++i) {
        uint32_t map, owner;
        if (!a->io.read(a->io.context, SPMI_CORE, CHANNEL_MAP + 4 * i, &map) || map == UINT32_MAX)
            return SPMI_IO_ERROR;
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        if (!a->io.read(a->io.context, SPMI_CONFIG, 4 * i, &owner) || owner == UINT32_MAX)
            return SPMI_IO_ERROR;
        int mine = (owner & 7) == a->ee;
        if (p.observer == SPMI_NO_APID || mine) p.observer = (uint16_t)i;
        if (mine) p.writer = (uint16_t)i;
        /* With duplicate mappings the last IRQ-owner entry wins, even if it
         * directs the interrupt to another execution environment. */
        if (map & IRQ_OWNER) p.irq = mine ? (uint16_t)i : SPMI_NO_APID;
    }
    if (p.observer == SPMI_NO_APID) return SPMI_NOT_FOUND;
    *out = p;
    return SPMI_OK;
}

static enum spmi_status finish(struct spmi_arbiter *a)
{
    uint32_t state;
    for (unsigned n = 0; n <= TIMEOUT_US; ++n) {
        if (!a->io.read(a->io.context, a->pending_region, a->pending_channel + REG_STATUS, &state) ||
            state == UINT32_MAX)
            return SPMI_IO_ERROR;
        if (state & DONE) {
            a->pending = 0;
            if (state & DENIED) return SPMI_DENIED;
            if (state & FAILURE) return SPMI_FAILED;
            return (state & DROPPED) ? SPMI_DROPPED : SPMI_OK;
        }
        if (n < TIMEOUT_US) a->io.delay_us(a->io.context, 1);
    }
    return SPMI_TIMEOUT;
}

enum spmi_status spmi_v7_retire(struct spmi_arbiter *a)
{
    if (!a || !a->count) return SPMI_INVALID;
    return a->pending ? finish(a) : SPMI_OK;
}

static enum spmi_status transfer(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                                 unsigned reg, uint8_t *dst, const uint8_t *src, unsigned len)
{
    if (!a || !a->count || !p || (!src && !dst) || !len || len > TRANSACTION_BYTES ||
        reg > 0x100u - len) return SPMI_INVALID;
    if (a->pending) return SPMI_BUSY;
    unsigned id = src ? p->writer : p->observer;
    if (id == SPMI_NO_APID) return SPMI_NOT_OWNER;
    if (id < a->first || id >= (unsigned)a->first + a->count) return SPMI_INVALID;
    enum spmi_region region = src ? SPMI_CHANNELS : SPMI_OBSERVER;
    uint32_t base = src ? 0x1000u * id : 0x8000u * a->ee + 0x20u * id;
    if (src) {
        for (unsigned i = 0; i < len; i += 4) {
            uint32_t word = 0;
            for (unsigned j = 0; j < 4 && i+j < len; ++j) word |= (uint32_t)src[i+j] << (8*j);
            if (!a->io.write(a->io.context, region, base + WRITE_DATA + i, word)) return SPMI_IO_ERROR;
        }
    }
    uint32_t cmd = (src ? 0u : EXT_READ_LONG) | (reg << 4) | (len - 1u);
    if (!a->io.write(a->io.context, region, base + REG_COMMAND, cmd)) return SPMI_IO_ERROR;
    a->pending_channel = base;
    a->pending_region = region;
    a->pending = 1;
    enum spmi_status status = finish(a);
    if (status != SPMI_OK || src) return status;
    uint8_t result[TRANSACTION_BYTES];
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t word;
        if (!a->io.read(a->io.context, region, base + READ_DATA + i, &word)) return SPMI_IO_ERROR;
        for (unsigned j = 0; j < 4 && i+j < len; ++j) result[i+j] = (uint8_t)(word >> (8*j));
    }
    for (unsigned i = 0; i < len; ++i) dst[i] = result[i];
    return SPMI_OK;
}

enum spmi_status spmi_v7_read(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                              unsigned reg, uint8_t *data, unsigned length)
{
    return transfer(a, p, reg, data, 0, length);
}

enum spmi_status spmi_v7_write(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                               unsigned reg, const uint8_t *data, unsigned length)
{
    return transfer(a, p, reg, 0, data, length);
}
