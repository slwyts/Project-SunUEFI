/* SPDX-License-Identifier: BSD-2-Clause */
#include "gpio_input.h"

/* Input direction first, enable last. Restore in reverse so a previously
 * driven output cannot see the temporary input pull or voltage selection. */
static const uint8_t reg[] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x46 };
static const uint8_t lv_mv_mask[] = { 0x03, 7, 7, 0x80, 0x0f, 0x80 };
static const uint8_t old_mask[] = { 0x7e, 7, 7, 0x0f, 0, 0x80 };

static enum spmi_status write_field(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                                    unsigned i, uint8_t mask, uint8_t old, uint8_t bits)
{
    uint8_t value = (uint8_t)((old & ~mask) | (bits & mask));
    if (value == old) return SPMI_OK;
    enum spmi_status s = spmi_v7_write(a, p, reg[i], &value, 1);
    if (s != SPMI_OK) return s;
    s = spmi_v7_read(a, p, reg[i], &old, 1);
    if (s != SPMI_OK) return s;
    return ((old ^ bits) & mask) ? SPMI_IO_ERROR : SPMI_OK;
}

enum spmi_status spmi_gpio_input_stop(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                                     struct spmi_gpio_input *pin)
{
    if (!a || !p || !pin) return SPMI_INVALID;
    pin->active = 0;
    const uint8_t *masks = pin->lv_mv ? lv_mv_mask : old_mask;
    for (unsigned n = sizeof(reg); n; --n) {
        unsigned i = n - 1;
        if (!(pin->dirty & (1u << i))) continue;
        uint8_t now;
        enum spmi_status s = spmi_v7_read(a, p, reg[i], &now, 1);
        if (s == SPMI_OK) s = write_field(a, p, i, masks[i], now, pin->saved[i]);
        if (s != SPMI_OK) return s;
        pin->dirty &= (uint8_t)~(1u << i);
    }
    return SPMI_OK;
}

enum spmi_status spmi_gpio_input_start(struct spmi_arbiter *a, const struct spmi_peripheral *p,
                                      struct spmi_gpio_input *pin, uint8_t vin, uint8_t pull)
{
    if (!a || !p || !pin || vin > 7 || pull > 5) return SPMI_INVALID;
    if (pin->dirty || pin->active) return SPMI_BUSY;
    uint8_t id[2];
    enum spmi_status s = spmi_v7_read(a, p, 4, id, sizeof(id));
    if (s != SPMI_OK) return s;
    if (id[0] != 0x10) return SPMI_NOT_FOUND;
    switch (id[1]) {
    case 0x01: case 0x05: case 0x09: case 0x0d: pin->lv_mv = 0; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: pin->lv_mv = 1; break;
    default: return SPMI_NOT_FOUND;
    }
    const uint8_t *masks = pin->lv_mv ? lv_mv_mask : old_mask;
    const uint8_t wanted[] = { 0, vin, pull, 0, 0, 0x80 };
    for (unsigned i = 0; i < sizeof(reg); ++i) {
        if (!masks[i]) continue;
        uint8_t old;
        s = spmi_v7_read(a, p, reg[i], &old, 1);
        if (s != SPMI_OK) break;
        if (!((old ^ wanted[i]) & masks[i])) continue;
        pin->saved[i] = old & masks[i];
        pin->dirty |= (uint8_t)(1u << i);
        s = write_field(a, p, i, masks[i], old, wanted[i]);
        if (s != SPMI_OK) break;
    }
    if (s == SPMI_OK) pin->active = 1;
    else (void)spmi_gpio_input_stop(a, p, pin);
    return s;
}
