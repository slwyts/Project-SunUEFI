/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_qup_power.h"

const unsigned piano_qup_i2c_pins[2] = {56, 57}; /* SDA, SCL */
#define CTL_FIELDS 0x3ffu   /* pull, mux, drive strength and output enable */
#define FUNCTION_1 (1u << 2)
#define EGPIO_PRESENT (1u << 11)
#define EGPIO_ENABLE (1u << 12)

static int update_pin(const struct piano_qup_board_io *b, unsigned pin, uint32_t mask, uint32_t target)
{
    uint32_t live = b->read_pad(b->context, pin);
    if (live == UINT32_MAX) return 0;
    if ((live & mask) != target)
        b->write_pad(b->context, pin, (live & ~mask) | target);
    uint32_t back = b->read_pad(b->context, pin);
    return back != UINT32_MAX && (back & mask) == target;
}

static int stop(struct piano_qup_power *p, const struct piano_qup_io *clock,
                const struct piano_qup_board_io *b)
{
    p->ready = 0;
    for (unsigned n = 2; n; --n) {
        unsigned i = n - 1;
        if (!(p->changed & (1u << i))) continue;
        if (!update_pin(b, piano_qup_i2c_pins[i], p->mask[i], p->old[i])) return 0;
        p->changed &= ~(1u << i);
    }
    if (p->pads_held) {
        if (!b->reserve_pads(b->context, 0)) return 0;
        p->pads_held = 0;
    }
    if (!piano_qup_stop_clocks(&p->clocks, clock)) return 0;
    if (p->fabric_held) {
        if (!b->fabric(b->context, 0)) return 0;
        p->fabric_held = 0;
    }
    return 1;
}

int piano_qup_set_power(struct piano_qup_power *p, const struct piano_qup_io *clock,
                        const struct piano_qup_board_io *b, int enable)
{
    if (!p || !clock || !b || !b->fabric || !b->reserve_pads || !b->read_pad || !b->write_pad)
        return 0;
    if (!enable) return stop(p, clock, b);
    if (p->ready) return 1;
    if (!stop(p, clock, b) || !b->fabric(b->context, 1)) return 0;
    p->fabric_held = 1;
    if (!piano_qup_start_clocks(&p->clocks, clock) || !b->reserve_pads(b->context, 1)) goto fail;
    p->pads_held = 1;
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t live = b->read_pad(b->context, piano_qup_i2c_pins[i]);
        if (live == UINT32_MAX) goto fail;
        /* Alt 1, no pull, 2 mA, no GPIO output; an eGPIO-capable pad needs
         * to select the application TLMM. Never touch IRQ or output data.
         */
        uint32_t extra = (live & EGPIO_PRESENT) ? EGPIO_ENABLE : 0;
        p->mask[i] = CTL_FIELDS | extra;
        p->old[i] = live & p->mask[i];
        if (p->old[i] != (FUNCTION_1 | extra)) {
            p->changed |= 1u << i;
            if (!update_pin(b, piano_qup_i2c_pins[i], p->mask[i], FUNCTION_1 | extra)) goto fail;
        }
    }
    p->ready = 1;
    return 1;
fail:
    (void)stop(p, clock, b);
    return 0;
}
