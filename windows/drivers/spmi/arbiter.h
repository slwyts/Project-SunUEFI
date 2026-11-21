/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

enum spmi_region { SPMI_CORE, SPMI_CONFIG, SPMI_CHANNELS, SPMI_OBSERVER, SPMI_INTERRUPT, SPMI_REGIONS };
enum spmi_status {
    SPMI_OK, SPMI_INVALID, SPMI_NOT_FOUND, SPMI_NOT_OWNER, SPMI_IO_ERROR, SPMI_DENIED,
    SPMI_DROPPED, SPMI_FAILED, SPMI_TIMEOUT, SPMI_BUSY
};

struct spmi_register_io {
    void *context;
    int (*read)(void *, enum spmi_region, uint32_t offset, uint32_t *value);
    int (*write)(void *, enum spmi_region, uint32_t offset, uint32_t value);
    void (*delay_us)(void *, unsigned);
};

/* The OS parent serializes all its PMIC users, including complete read/modify/write
 * operations, and holds its assigned resources until the last command retires. */
struct spmi_arbiter {
    struct spmi_register_io io;
    uint16_t first, count;
    uint8_t ee, pending;
    enum spmi_region pending_region;
    uint32_t pending_channel;
};

#define SPMI_NO_APID UINT16_MAX
struct spmi_peripheral {
    uint16_t ppid, observer, writer, irq;
};

enum spmi_status spmi_v7_start(struct spmi_arbiter *, const struct spmi_register_io *, unsigned bus, unsigned ee);
enum spmi_status spmi_v7_resolve(const struct spmi_arbiter *, uint16_t ppid, struct spmi_peripheral *);
enum spmi_status spmi_v7_read(struct spmi_arbiter *, const struct spmi_peripheral *, unsigned reg,
                              uint8_t *data, unsigned length);
enum spmi_status spmi_v7_write(struct spmi_arbiter *, const struct spmi_peripheral *, unsigned reg,
                               const uint8_t *data, unsigned length);
/* A timeout is not a cancellation. This function never submits another command. */
enum spmi_status spmi_v7_retire(struct spmi_arbiter *);
