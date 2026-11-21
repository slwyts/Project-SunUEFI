/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "arbiter.h"

/* Only fields changed by this input connection are restored. The parent
 * owns the physical pin exclusively until dirty has returned to zero. */
struct spmi_gpio_input { uint8_t saved[6], dirty, lv_mv, active; };
enum spmi_status spmi_gpio_input_start(struct spmi_arbiter *, const struct spmi_peripheral *,
                                      struct spmi_gpio_input *, uint8_t vin, uint8_t pull);
enum spmi_status spmi_gpio_input_stop(struct spmi_arbiter *, const struct spmi_peripheral *,
                                     struct spmi_gpio_input *);
