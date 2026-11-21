/* SPDX-License-Identifier: BSD-2-Clause */
#include "rsc.h"
#include <stddef.h>

/* Piano apps_rsc/drv@2/channel@0: active TCS 0..2, followed by the solver's
 * sleep/wake and fast-path slots. Only the active group's registers are used. */
enum {
    DRV_ID = 0, DRV_CONFIG = 0x0c, TCS_BASE = 0xd00, TCS_STRIDE = 0x2a0,
    IRQ_ENABLE = TCS_BASE, IRQ_STATUS = TCS_BASE + 4, IRQ_CLEAR = TCS_BASE + 8,
    ACTIVE_COUNT = 3, DRV_ID_APPS = 2,
    AMC_ENABLE = 1u << 16, AMC_TRIGGER = 1u << 24,
    RESP_REQUIRED = 1u << 8, MSG_WRITE = 1u << 16, MSG_LENGTH = 8,
    CMD_ISSUED = 1u << 8, CMD_COMPLETE = 1u << 16
};

enum phase { IDLE, ARMING, RUNNING, UNTRIGGER, DISABLE, CLEAR_COMMANDS, ACK_IRQ, RESTORE_IRQ };

static uint32_t block(unsigned slot)
{
    return TCS_BASE + slot * TCS_STRIDE;
}

static uint32_t wait_reg(const struct rpmh_rsc *r, unsigned slot)
{
    return block(slot) + (r->version == 3 ? 0x20u : 0x10u);
}

static uint32_t msg_reg(const struct rpmh_rsc *r, unsigned slot, unsigned i)
{
    return block(slot) + (r->version == 3 ? 0x34u + 24*i : 0x30u + 20*i);
}

static int read_reg(struct rpmh_rsc *r, uint32_t reg, uint32_t *out)
{
    return r->io.read(r->io.context, reg, out);
}

/* The control registers have reserved bits. An all-ones read is not idle or
 * a completion. Command addresses, payloads and response data are read raw. */
static int control(struct rpmh_rsc *r, uint32_t reg, uint32_t *out)
{
    return read_reg(r, reg, out) && *out != UINT32_MAX;
}

static void put(struct rpmh_rsc *r, uint32_t reg, uint32_t value)
{
    r->io.write(r->io.context, reg, value);
}

static uint32_t msg_id(const struct rpmh_rsc *r)
{
    return MSG_LENGTH | RESP_REQUIRED | (r->writing ? MSG_WRITE : 0u);
}

static int same_resource(uint32_t a, uint32_t b)
{
    /* Voltage, enable, mode and headroom of one VRM share a 16-byte block. */
    return a == b || (((a >> 16) & 15) == 4 && ((a ^ b) & ~UINT32_C(15)) == 0);
}

int rpmh_rsc_idle(const struct rpmh_rsc *r)
{
    return r && r->phase == IDLE;
}

enum rpmh_rsc_result rpmh_rsc_init(struct rpmh_rsc *r, const struct rpmh_rsc_io *io,
                                   uint64_t physical, uint32_t bytes)
{
    uint32_t version, config;
    if (!r || !io || !io->read || !io->write || physical != RPMH_APPS_DRV2_BASE ||
        bytes != RPMH_APPS_DRV2_SIZE) return RPMH_RSC_INVALID;
    if (!rpmh_rsc_idle(r)) return RPMH_RSC_BUSY;
    r->version = 0;
    r->io = *io;
    if (!control(r, DRV_ID, &version) || !control(r, DRV_CONFIG, &config))
        return RPMH_RSC_IO_ERROR;
    version = (version >> 16) & 255;
    unsigned slots = (config >> (6 * DRV_ID_APPS)) & 63, commands = config >> 27;
    if ((version != 2 && version != 3) || slots < ACTIVE_COUNT ||
        !commands || commands > RPMH_TCS_MAX_COMMANDS) return RPMH_RSC_INVALID;
    r->version = version;
    r->commands_per_tcs = commands;
    return RPMH_RSC_OK;
}

enum rpmh_rsc_result rpmh_rsc_check_irq_handoff(struct rpmh_rsc *r)
{
    uint32_t enabled, pending;
    if (!r || !r->version) return RPMH_RSC_INVALID;
    if (!control(r, IRQ_ENABLE, &enabled) || !control(r, IRQ_STATUS, &pending)) return RPMH_RSC_IO_ERROR;
    /* Sleep/wake/fast-path TCSes can execute without our active driver
     * doing anything. This ISR cannot handle their level interrupts. */
    if (enabled & ~((1u << ACTIVE_COUNT)-1)) return RPMH_RSC_BUSY;
    uint32_t owned = r->phase == IDLE ? 0 : (1u << r->slot);
    if (enabled & pending & ~owned) return RPMH_RSC_BUSY;
    for (unsigned i = 0; i < ACTIVE_COUNT; ++i) {
        uint32_t state, ctl, mask, w = wait_reg(r,i);
        if (!(enabled & ~owned & (1u << i))) continue;
        if (!control(r,w+8,&state) || !control(r,w+4,&ctl) || !control(r,w+12,&mask))
            return RPMH_RSC_IO_ERROR;
        if (!(state & 1) || (mask && (ctl & AMC_TRIGGER))) return RPMH_RSC_BUSY;
    }
    return RPMH_RSC_OK;
}

static enum rpmh_rsc_result select_slot(struct rpmh_rsc *r, const struct rpmh_rsc_command *cmd,
                                        unsigned count, unsigned *slot, uint32_t *base)
{
    uint32_t irq, valid = (1u << r->commands_per_tcs) - 1;
    if (!control(r, IRQ_STATUS, &irq)) return RPMH_RSC_IO_ERROR;
    *slot = ACTIVE_COUNT;
    for (unsigned t = 0; t < ACTIVE_COUNT; ++t) {
        uint32_t w = wait_reg(r, t), mask, state, ctl, waiting;
        if (!control(r, w+12, &mask) || !control(r, w+8, &state) ||
            !control(r, w+4, &ctl) || !control(r, w, &waiting) || (mask & ~valid))
            return RPMH_RSC_IO_ERROR;
        /* Firmware may have left a request armed or awaiting its original
         * completion handler. Don't reuse its slot or issue a conflicting vote. */
        if (mask || waiting || !(state & 1) || (ctl & (AMC_ENABLE | AMC_TRIGGER)) ||
            (irq & (1u << t))) {
            if (!mask && (!(state & 1) || (ctl & AMC_TRIGGER)))
                return RPMH_RSC_BUSY;  /* No addresses to exclude for this transfer. */
            for (unsigned i = 0; i < r->commands_per_tcs; ++i) {
                uint32_t address;
                if (!(mask & (1u << i))) continue;
                if (!read_reg(r, msg_reg(r,t,i)+4, &address)) return RPMH_RSC_IO_ERROR;
                for (unsigned j = 0; j < count; ++j)
                    if (same_resource(cmd[j].address, address)) return RPMH_RSC_BUSY;
            }
        } else if (*slot == ACTIVE_COUNT) {
            *slot = t;
            *base = ctl;
        }
    }
    return *slot == ACTIVE_COUNT ? RPMH_RSC_BUSY : RPMH_RSC_OK;
}

static enum rpmh_rsc_result trigger_if_ready(struct rpmh_rsc *r)
{
    uint32_t bit = 1u << r->slot, w = wait_reg(r, r->slot), mask = (1u << r->count)-1;
    uint32_t val;
    if (!control(r, IRQ_STATUS, &val) || (val & bit)) return RPMH_RSC_IO_ERROR;
    if (!control(r, IRQ_ENABLE, &val)) return RPMH_RSC_IO_ERROR;
    if (!(val & bit)) return RPMH_RSC_PENDING;
    if (!control(r, w+4, &val)) return RPMH_RSC_IO_ERROR;
    if (val != (r->control_base | AMC_ENABLE)) return RPMH_RSC_PENDING;
    if (!control(r, w, &val)) return RPMH_RSC_IO_ERROR;
    if (val != mask) return RPMH_RSC_PENDING;
    if (!control(r, w+12, &val)) return RPMH_RSC_IO_ERROR;
    if (val != mask) return RPMH_RSC_PENDING;
    for (unsigned i = 0; i < r->count; ++i) {
        uint32_t m = msg_reg(r, r->slot, i), id, addr;
        if (!read_reg(r,m,&id) || !read_reg(r,m+4,&addr)) return RPMH_RSC_IO_ERROR;
        if (id != msg_id(r) || addr != r->command[i].address) return RPMH_RSC_PENDING;
        if (r->writing) {
            if (!read_reg(r,m+8,&val)) return RPMH_RSC_IO_ERROR;
            if (val != r->command[i].data) return RPMH_RSC_PENDING;
        }
    }
    r->phase = RUNNING;
    put(r, w+4, r->control_base | AMC_ENABLE | AMC_TRIGGER);
    return RPMH_RSC_PENDING;
}

enum rpmh_rsc_result rpmh_rsc_begin(struct rpmh_rsc *r, const struct rpmh_rsc_command *cmd,
                                    unsigned count, int write)
{
    uint32_t original, base = 0;
    unsigned slot;
    if (!r || !r->version || !cmd || !count || count > r->commands_per_tcs ||
        (write != 0 && write != 1)) return RPMH_RSC_INVALID;
    if (!rpmh_rsc_idle(r)) return RPMH_RSC_BUSY;
    for (unsigned i = 0; i < count; ++i)
        if (!cmd[i].address) return RPMH_RSC_INVALID;
    enum rpmh_rsc_result status = select_slot(r, cmd, count, &slot, &base);
    if (status != RPMH_RSC_OK) return status;
    if (!control(r, IRQ_ENABLE, &original)) return RPMH_RSC_IO_ERROR;
    uint32_t bit = 1u << slot, mask = (1u << count)-1;
    r->slot = slot;
    r->count = count;
    r->writing = (unsigned)write;
    r->control_base = base;
    r->irq_was_enabled = original & bit;
    r->irq_seen = 0;
    r->cancelling = 0;
    r->phase = ARMING;
    if (!r->irq_was_enabled) put(r, IRQ_ENABLE, original | bit);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t m = msg_reg(r, slot, i);
        r->command[i] = cmd[i];
        put(r, m, msg_id(r));
        put(r, m+4, cmd[i].address);
        if (write) put(r, m+8, cmd[i].data);
    }
    uint32_t w = wait_reg(r, slot);
    put(r, w, mask);
    put(r, w+12, mask);
    put(r, w+4, base | AMC_ENABLE);
    return trigger_if_ready(r);
}

int rpmh_rsc_interrupt(struct rpmh_rsc *r)
{
    uint32_t irq;
    if (!r || r->cancelling || r->phase < RUNNING || r->phase > ACK_IRQ ||
        !control(r, IRQ_STATUS, &irq) || !(irq & (1u << r->slot))) return 0;
    r->irq_seen = 1;
    put(r, IRQ_CLEAR, 1u << r->slot);
    return 1;
}

enum rpmh_rsc_result rpmh_rsc_cancel_unsent(struct rpmh_rsc *r)
{
    uint32_t state, ctl, irq;
    if (!r || !r->version || rpmh_rsc_idle(r)) return RPMH_RSC_INVALID;
    if (r->cancelling) return rpmh_rsc_poll(r, NULL, 0);
    if (r->phase != ARMING) return RPMH_RSC_BUSY;
    uint32_t w = wait_reg(r, r->slot);
    if (!control(r, w+8, &state) || !control(r, w+4, &ctl) || !control(r, IRQ_STATUS, &irq))
        return RPMH_RSC_IO_ERROR;
    if (!(state & 1) || (ctl & AMC_TRIGGER) || (irq & (1u << r->slot)))
        return RPMH_RSC_BUSY;
    r->cancelling = 1;
    put(r, w+4, r->control_base);
    r->phase = DISABLE;
    return rpmh_rsc_poll(r, NULL, 0);
}

enum rpmh_rsc_result rpmh_rsc_poll(struct rpmh_rsc *r, uint32_t *read_data, unsigned capacity)
{
    if (!r || !r->version || rpmh_rsc_idle(r) ||
        (!r->cancelling && !r->writing && (!read_data || capacity < r->count))) return RPMH_RSC_INVALID;
    uint32_t bit = 1u << r->slot, w = wait_reg(r, r->slot), val;
    if (r->phase == ARMING) return trigger_if_ready(r);
    if (r->phase == RUNNING) {
        if (!r->irq_seen) {
            if (!control(r, IRQ_STATUS, &val)) return RPMH_RSC_IO_ERROR;
            if (!(val & bit)) return RPMH_RSC_PENDING;
        }
        if (!control(r, w+8, &val)) return RPMH_RSC_IO_ERROR;
        if (!(val & 1)) return RPMH_RSC_PENDING;
        for (unsigned i = 0; i < r->count; ++i) {
            uint32_t m = msg_reg(r, r->slot, i);
            if (!control(r, m+12, &val)) return RPMH_RSC_IO_ERROR;
            if ((val & (CMD_ISSUED | CMD_COMPLETE)) != (CMD_ISSUED | CMD_COMPLETE))
                return RPMH_RSC_PENDING;
            if (!r->writing && !read_reg(r, m+16, &r->reply[i])) return RPMH_RSC_IO_ERROR;
        }
        /* Clear trigger before disabling AMC. Don't send a second trigger
         * or discard an outstanding completion during late polling. */
        put(r, w+4, r->control_base | AMC_ENABLE);
        r->phase = UNTRIGGER;
    }
    if (r->phase == UNTRIGGER) {
        if (!control(r, w+4, &val)) return RPMH_RSC_IO_ERROR;
        if (val != (r->control_base | AMC_ENABLE)) return RPMH_RSC_PENDING;
        put(r, w+4, r->control_base);
        r->phase = DISABLE;
    }
    if (r->phase == DISABLE) {
        if (!control(r, w+4, &val)) return RPMH_RSC_IO_ERROR;
        if (val != r->control_base) return RPMH_RSC_PENDING;
        put(r, w+12, 0);
        put(r, w, 0);
        r->phase = CLEAR_COMMANDS;
    }
    if (r->phase == CLEAR_COMMANDS) {
        if (!control(r, w+12, &val)) return RPMH_RSC_IO_ERROR;
        if (val) return RPMH_RSC_PENDING;
        if (!control(r, w, &val)) return RPMH_RSC_IO_ERROR;
        if (val) return RPMH_RSC_PENDING;
        if (!r->irq_seen && !r->cancelling) put(r, IRQ_CLEAR, bit);
        r->phase = ACK_IRQ;
    }
    if (r->phase == ACK_IRQ) {
        if (!control(r, IRQ_STATUS, &val)) return RPMH_RSC_IO_ERROR;
        if (val & bit) return RPMH_RSC_PENDING;
        if (!control(r, IRQ_ENABLE, &val)) return RPMH_RSC_IO_ERROR;
        if (!r->irq_was_enabled && (val & bit)) put(r, IRQ_ENABLE, val & ~bit);
        r->phase = RESTORE_IRQ;
    }
    if (!control(r, IRQ_ENABLE, &val)) return RPMH_RSC_IO_ERROR;
    if ((val & bit) != r->irq_was_enabled) return RPMH_RSC_PENDING;
    if (!r->writing && !r->cancelling)
        for (unsigned i = 0; i < r->count; ++i) read_data[i] = r->reply[i];
    r->phase = IDLE;
    return r->cancelling ? RPMH_RSC_CANCELLED : RPMH_RSC_OK;
}
