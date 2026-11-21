/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "scmi_perf.h"

struct scmi_channel_io {
    void *context;
    uint32_t (*read)(void *, unsigned offset);
    void (*write)(void *, unsigned offset, uint32_t value);
    void (*ring)(void *, uint32_t header);
    uint64_t (*now_us)(void *);
    void (*wait)(void *);
};

/* One owner for the TX slot, including any other SCMI protocols sharing it.
 * The caller serializes access and keeps both PnP resources powered/mapped. */
struct scmi_channel {
    struct scmi_channel_io io;
    uint32_t bytes, timeout_us, token, pending_header;
    int pending;
};

int scmi_channel_init(struct scmi_channel *channel, const struct scmi_channel_io *io,
                      uint32_t bytes, uint32_t timeout_us);
int scmi_channel_message(void *context, unsigned protocol, unsigned command,
                         const uint32_t *args, size_t nargs, uint32_t *result,
                         size_t capacity, size_t *count);
/* Retire a timed-out transfer before suspending or unmapping its doorbell. */
int scmi_channel_drain(struct scmi_channel *channel);
