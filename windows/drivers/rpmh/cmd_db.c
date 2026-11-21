/* SPDX-License-Identifier: BSD-2-Clause */
#include "cmd_db.h"

/* Qualcomm command DB: offsets in the directory are relative to the byte
 * following the common header. Entry auxiliary offsets are per accelerator.
 */
enum { HEADER = 144, GROUPS = 8, GROUP = 16, ENTRY = 24, NAME = 8 };

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)u16(p + 2) << 16 | u16(p);
}

static int fits(size_t size, size_t begin, size_t bytes)
{
    return begin <= size && bytes <= size - begin;
}

int rpmh_db_open(struct rpmh_cmd_db *db, const uint8_t *bytes, size_t size)
{
    if (!db || !bytes || size < HEADER || u32(bytes + 4) != 0x0c0330db)
        return 0;
    size_t payload = size - HEADER;
    int nonempty = 0;
    for (unsigned g = 0; g < GROUPS; ++g) {
        const uint8_t *dir = bytes + 8 + g * GROUP;
        if (!u16(dir)) break;
        unsigned count = u16(dir + 6), start = u16(dir + 2), aux = u16(dir + 4);
        if (!fits(payload, start, (size_t)count * ENTRY) || aux > payload) return 0;
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *e = bytes + HEADER + start + i * ENTRY;
            if (u16(e + 20) && !fits(payload, (size_t)aux + u16(e + 22), u16(e + 20))) return 0;
        }
        nonempty |= count != 0;
    }
    if (!nonempty) return 0;
    *db = (struct rpmh_cmd_db){ bytes, size };
    return 1;
}

int rpmh_db_find(const struct rpmh_cmd_db *db, const char *name, enum rpmh_accelerator type,
                 struct rpmh_db_entry *out)
{
    if (!db || !db->bytes || !name || !out || !*name) return 0;
    uint8_t key[NAME] = {0};
    unsigned len = 0;
    while (len < NAME && name[len]) { key[len] = (uint8_t)name[len]; ++len; }
    if (name[len]) return 0;
    const uint8_t *payload = db->bytes + HEADER;
    struct rpmh_db_entry match = {0};
    for (unsigned g = 0; g < GROUPS; ++g) {
        const uint8_t *dir = db->bytes + 8 + g * GROUP;
        unsigned id = u16(dir);
        if (!id) break;
        if (id != (unsigned)type) continue;
        for (unsigned i = 0; i < u16(dir + 6); ++i) {
            const uint8_t *e = payload + u16(dir + 2) + i * ENTRY;
            unsigned j = 0;
            while (j < NAME && key[j] == e[j]) ++j;
            if (j != NAME) continue;
            uint32_t address = u32(e + 16);
            if (match.address || !address || ((address >> 16) & 7) != id) return 0;
            match.address = address;
            match.aux_size = u16(e + 20);
            match.aux = match.aux_size ? payload + u16(dir + 4) + u16(e + 22) : NULL;
        }
    }
    if (!match.address) return 0;
    *out = match;
    return 1;
}

int rpmh_arc_corner(const struct rpmh_db_entry *e, uint32_t level, uint32_t *corner)
{
    if (!e || !corner || !e->aux || e->aux_size < 2 || (e->aux_size & 1) ||
        ((e->address >> 16) & 7) != RPMH_ARC || u16(e->aux)) return 0;
    size_t choice = 0;
    uint32_t previous = 0;
    int padded = 0;
    for (size_t i = 1; i < e->aux_size / 2; ++i) {
        uint32_t v = u16(e->aux + 2 * i);
        if (!v) { padded = 1; continue; }
        if (padded || v < previous) return 0;
        previous = v;
        if (level && !choice && v >= level) choice = i;
    }
    if (level && !choice) return 0;
    *corner = (uint32_t)choice;
    return 1;
}
