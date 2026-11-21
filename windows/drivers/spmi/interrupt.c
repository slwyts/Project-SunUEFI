/* SPDX-License-Identifier: BSD-2-Clause */
#include "interrupt.h"

enum {
    RT_STATUS = 0x10, SET_TYPE = 0x11, LATCHED_CLEAR = 0x14, ENABLE_SET = 0x15, ENABLE_CLEAR = 0x16,
    ACCESS_ENABLE = 0x100, IRQ_STATUS = 0x104, IRQ_CLEAR = 0x108
};

enum spmi_status spmi_irq_open(const struct spmi_arbiter *a, unsigned ppid, unsigned bit, struct spmi_irq *out)
{
    struct spmi_irq line;
    if (!out || bit > 7 || ppid > 0xfff) return SPMI_INVALID;
    enum spmi_status s = spmi_v7_resolve(a, (uint16_t)ppid, &line.peripheral);
    if (s != SPMI_OK) return s;
    if (line.peripheral.irq == SPMI_NO_APID || line.peripheral.writer == SPMI_NO_APID) return SPMI_NOT_OWNER;
    line.bit = (uint8_t)bit;
    *out = line;
    return SPMI_OK;
}

static int valid(const struct spmi_arbiter *a, const struct spmi_irq *l)
{
    return a && a->count && l && l->bit < 8 && l->peripheral.irq >= a->first &&
        l->peripheral.irq < (unsigned)a->first + a->count;
}

enum spmi_status spmi_irq_configure(struct spmi_arbiter *a, const struct spmi_irq *l, enum spmi_trigger how)
{
    if (!valid(a,l) || (unsigned)how > SPMI_BOTH) return SPMI_INVALID;
    uint8_t v[3], mask = (uint8_t)(1u << l->bit);
    enum spmi_status s = spmi_v7_read(a, &l->peripheral, SET_TYPE, v, sizeof(v));
    if (s != SPMI_OK) return s;
    /* IRQ type, polarity high and polarity low are adjacent registers. Write
     * them in one bus transaction, retaining the configuration of all peers. */
    const unsigned state[3] = {
        how >= SPMI_FALLING,
        how == SPMI_HIGH || how == SPMI_RISING || how == SPMI_BOTH,
        how == SPMI_LOW || how == SPMI_FALLING || how == SPMI_BOTH
    };
    int changed = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t next = (uint8_t)((v[i] & ~mask) | (state[i] ? mask : 0u));
        changed |= next != v[i];
        v[i] = next;
    }
    return changed ? spmi_v7_write(a, &l->peripheral, SET_TYPE, v, sizeof(v)) : SPMI_OK;
}

static enum spmi_status enable(struct spmi_arbiter *a, const struct spmi_irq *l, int clear_stale)
{
    if (!valid(a,l)) return SPMI_INVALID;
    if (a->pending) return SPMI_BUSY;
    uint32_t reg = 0x1000u * l->peripheral.irq + ACCESS_ENABLE;
    uint32_t enabled;
    if (!a->io.read(a->io.context, SPMI_CHANNELS, reg, &enabled) || enabled == UINT32_MAX) return SPMI_IO_ERROR;
    if (!(enabled & 1) && !a->io.write(a->io.context, SPMI_CHANNELS, reg, enabled | 1)) return SPMI_IO_ERROR;
    uint8_t current, bit = (uint8_t)(1u << l->bit);
    enum spmi_status s = spmi_v7_read(a, &l->peripheral, ENABLE_SET, &current, 1);
    if (s != SPMI_OK || (current & bit)) return s;
    if (!clear_stale) return spmi_v7_write(a, &l->peripheral, ENABLE_SET, &bit, 1);
    const uint8_t enable[2] = { bit, bit };
    return spmi_v7_write(a, &l->peripheral, LATCHED_CLEAR, enable, sizeof(enable));
}

enum spmi_status spmi_irq_enable(struct spmi_arbiter *a, const struct spmi_irq *l)
{
    return enable(a, l, 1);
}

enum spmi_status spmi_irq_unmask(struct spmi_arbiter *a, const struct spmi_irq *l)
{
    /* This line is already connected. Preserve edges latched while masked. */
    return enable(a, l, 0);
}

enum spmi_status spmi_irq_mask(struct spmi_arbiter *a, const struct spmi_irq *l)
{
    if (!valid(a,l)) return SPMI_INVALID;
    /* ACCESS_ENABLE belongs to the entire peripheral, not to this line. */
    uint8_t bit = (uint8_t)(1u << l->bit);
    return spmi_v7_write(a, &l->peripheral, ENABLE_CLEAR, &bit, 1);
}

enum spmi_status spmi_irq_pending(const struct spmi_arbiter *a, const struct spmi_irq *l, unsigned *pending)
{
    if (!valid(a,l) || !pending) return SPMI_INVALID;
    uint32_t block = 0x1000u * l->peripheral.irq, enable, status;
    if (!a->io.read(a->io.context, SPMI_CHANNELS, block + ACCESS_ENABLE, &enable) || enable == UINT32_MAX)
        return SPMI_IO_ERROR;
    if (!(enable & 1)) { *pending = 0; return SPMI_OK; }
    if (!a->io.read(a->io.context, SPMI_CHANNELS, block + IRQ_STATUS, &status) || status == UINT32_MAX)
        return SPMI_IO_ERROR;
    *pending = (status >> l->bit) & 1;
    return SPMI_OK;
}

enum spmi_status spmi_irq_ack(struct spmi_arbiter *a, const struct spmi_irq *l)
{
    if (!valid(a,l)) return SPMI_INVALID;
    if (a->pending) return SPMI_BUSY;
    uint32_t reg = 0x1000u * l->peripheral.irq + IRQ_CLEAR;
    uint8_t bit = (uint8_t)(1u << l->bit);
    if (!a->io.write(a->io.context, SPMI_CHANNELS, reg, bit)) return SPMI_IO_ERROR;
    return spmi_v7_write(a, &l->peripheral, LATCHED_CLEAR, &bit, 1);
}

enum spmi_status spmi_irq_active_bank(const struct spmi_arbiter *a, const struct spmi_irq *pins,
                                      unsigned count, uint64_t enabled, uint64_t *active)
{
    if (!a || !a->count || !pins || !active || !count || count > 64 ||
        (count < 64 && enabled >> count)) return SPMI_INVALID;
    uint32_t groups = 0, summary[32], seen = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!(enabled & (UINT64_C(1) << i))) continue;
        if (!valid(a, &pins[i])) return SPMI_INVALID;
        unsigned group = pins[i].peripheral.irq / 32u;
        uint32_t bit = UINT32_C(1) << group;
        if (!(groups & bit)) {
            unsigned local = group - a->first/32u;
            if (!a->io.read(a->io.context, SPMI_INTERRUPT, 0x1000u*a->ee + 4u*local, &summary[group]))
                return SPMI_IO_ERROR;
            groups |= bit;
            seen |= summary[group];
        }
    }
    uint64_t result = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t bit = UINT64_C(1) << i;
        if (!(enabled & bit)) continue;
        unsigned id = pins[i].peripheral.irq;
        /* As in the Qualcomm irqchip, if the aggregate summary is empty
         * after an interrupt, consult the peripheral's own status. */
        if (seen && !(summary[id/32] & (UINT32_C(1) << (id % 32)))) continue;
        unsigned pending;
        enum spmi_status s = spmi_irq_pending(a, &pins[i], &pending);
        if (s != SPMI_OK) return s;
        if (pending) result |= bit;
    }
    *active = result;
    return SPMI_OK;
}

enum spmi_status spmi_irq_level(struct spmi_arbiter *a, const struct spmi_irq *l, unsigned *level)
{
    if (!valid(a,l) || !level) return SPMI_INVALID;
    uint8_t raw;
    enum spmi_status s = spmi_v7_read(a, &l->peripheral, RT_STATUS, &raw, 1);
    if (s == SPMI_OK) *level = (raw >> l->bit) & 1;
    return s;
}

enum spmi_status spmi_irq_enabled(struct spmi_arbiter *a, const struct spmi_irq *l, unsigned *enabled)
{
    if (!valid(a,l) || !enabled) return SPMI_INVALID;
    uint8_t raw;
    enum spmi_status s = spmi_v7_read(a, &l->peripheral, ENABLE_SET, &raw, 1);
    if (s == SPMI_OK) *enabled = (raw >> l->bit) & 1;
    return s;
}
