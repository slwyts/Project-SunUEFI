/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "arbiter.h"

enum spmi_trigger { SPMI_LOW, SPMI_HIGH, SPMI_FALLING, SPMI_RISING, SPMI_BOTH };
struct spmi_irq {
    struct spmi_peripheral peripheral;
    uint8_t bit;
};

enum spmi_status spmi_irq_open(const struct spmi_arbiter *, unsigned ppid, unsigned bit, struct spmi_irq *);
enum spmi_status spmi_irq_configure(struct spmi_arbiter *, const struct spmi_irq *, enum spmi_trigger);
enum spmi_status spmi_irq_enable(struct spmi_arbiter *, const struct spmi_irq *);
enum spmi_status spmi_irq_unmask(struct spmi_arbiter *, const struct spmi_irq *);
enum spmi_status spmi_irq_mask(struct spmi_arbiter *, const struct spmi_irq *);
enum spmi_status spmi_irq_ack(struct spmi_arbiter *, const struct spmi_irq *);
enum spmi_status spmi_irq_level(struct spmi_arbiter *, const struct spmi_irq *, unsigned *level);
enum spmi_status spmi_irq_enabled(struct spmi_arbiter *, const struct spmi_irq *, unsigned *enabled);
/* Read-only queries for dispatch of an aggregate IRQ. Neither reports nor
 * acknowledges a source which is not part of the caller's registered lines. */
enum spmi_status spmi_irq_pending(const struct spmi_arbiter *, const struct spmi_irq *, unsigned *pending);
/* A GpioClx bank has at most 64 pins, not necessarily consecutive in the PIC. */
enum spmi_status spmi_irq_active_bank(const struct spmi_arbiter *, const struct spmi_irq *,
                                      unsigned count, uint64_t enabled, uint64_t *active);
