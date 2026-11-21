/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

#define RPMH_APPS_DRV2_BASE UINT32_C(0x16520000)
#define RPMH_APPS_DRV2_SIZE UINT32_C(0x10000)
#define RPMH_TCS_MAX_COMMANDS 16

struct rpmh_rsc_io {
    void *context;
    /* Ordered device MMIO, e.g. READ/WRITE_REGISTER_ULONG, not ordinary RAM. */
    int (*read)(void *context, uint32_t offset, uint32_t *value);
    void (*write)(void *context, uint32_t offset, uint32_t value);
};

struct rpmh_rsc_command {
    uint32_t address, data;
};

enum rpmh_rsc_result {
    RPMH_RSC_OK, RPMH_RSC_PENDING, RPMH_RSC_BUSY, RPMH_RSC_INVALID, RPMH_RSC_IO_ERROR,
    RPMH_RSC_CANCELLED
};

struct rpmh_rsc {
    struct rpmh_rsc_io io;
    struct rpmh_rsc_command command[RPMH_TCS_MAX_COMMANDS];
    uint32_t reply[RPMH_TCS_MAX_COMMANDS];
    uint32_t control_base, wait_mask;
    unsigned commands_per_tcs, version, phase, slot, count, writing, irq_was_enabled, irq_seen, cancelling;
};

/* One zero-initialized instance per PnP-owned APPS DRV2 window. Serialize all
 * calls, including calls from the IRQ worker, against PnP/power transitions.
 * Command addresses come from the platform driver's command DB, not user mode. */
enum rpmh_rsc_result rpmh_rsc_init(struct rpmh_rsc *, const struct rpmh_rsc_io *,
                                   uint64_t physical, uint32_t bytes);
/* Before the OS connects the shared DRV interrupt, check that it won't
 * inherit an interrupt from a solver TCS or an unidentified transaction. */
enum rpmh_rsc_result rpmh_rsc_check_irq_handoff(struct rpmh_rsc *);
/* wait_mask orders commands within a TCS. The TCS always requests a
 * completion IRQ; read data is returned only after every reply is valid. */
enum rpmh_rsc_result rpmh_rsc_begin(struct rpmh_rsc *, const struct rpmh_rsc_command *,
                                    unsigned count, int write, unsigned wait_mask);
/* Under the same IRQ synchronization as begin/poll, acknowledge only this
 * transfer's level interrupt. Queue the completion worker when it returns 1. */
int rpmh_rsc_interrupt(struct rpmh_rsc *);
/* Withdraw a request whose trigger was never issued. For a started message
 * return BUSY without touching hardware; only its original completion retires it. */
enum rpmh_rsc_result rpmh_rsc_cancel_unsent(struct rpmh_rsc *);
/* A caller timeout does not cancel hardware. Continue polling the original
 * request and retain MMIO/power while !rpmh_rsc_idle(), even on read errors. */
enum rpmh_rsc_result rpmh_rsc_poll(struct rpmh_rsc *, uint32_t *read_data, unsigned capacity);
int rpmh_rsc_idle(const struct rpmh_rsc *);
