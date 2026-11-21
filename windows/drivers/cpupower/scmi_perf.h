/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The shared SCMI transport serializes commands from all protocol clients.
 * A successful call returns the response payload, without header or status,
 * as native little-endian words. Firmware error statuses pass through. */
struct scmi_transport {
    void *context;
    int (*message)(void *context, unsigned protocol, unsigned command,
                   const uint32_t *args, size_t nargs, uint32_t *result,
                   size_t capacity, size_t *count);
};

enum {
    SCMI_PERF_PROTOCOL = 0x13,
    SCMI_PERF_UNSUPPORTED = -1, SCMI_PERF_INVALID = -2, SCMI_PERF_DENIED = -3,
    SCMI_PERF_NOT_FOUND = -4, SCMI_PERF_BUSY = -6, SCMI_PERF_PROTOCOL_ERROR = -10,
    SCMI_PERF_NO_SPACE = -128, SCMI_PERF_TIMEOUT = -129,
    /* On Piano the 512-byte transmit channel contains a 32-byte response header. */
    SCMI_PERF_REPLY_WORDS = (512 - 32) / 4
};

struct scmi_perf {
    struct scmi_transport transport;
    uint32_t version, domains;
};

struct scmi_perf_state {
    uint32_t performance, khz, selector, transition_us;
};

struct scmi_perf_domain {
    uint32_t id, sustained_khz, sustained_performance, rate_limit_us;
    unsigned can_set;
    size_t count;
    /* Fastest first, as in PEP's discrete-state table. */
    const struct scmi_perf_state *states;
};

int scmi_perf_open(const struct scmi_transport *transport, struct scmi_perf *perf);
/* Supply NULL/0 to obtain the required state count in *needed. A second
 * call fills caller-owned storage. No domain state is published on failure. */
int scmi_perf_describe(const struct scmi_perf *perf, uint32_t id,
                       struct scmi_perf_state *storage, size_t capacity,
                       struct scmi_perf_domain *domain, size_t *needed);
int scmi_perf_get(const struct scmi_perf *perf, const struct scmi_perf_domain *domain, size_t *state);
int scmi_perf_set(const struct scmi_perf *perf, const struct scmi_perf_domain *domain, size_t state);
int scmi_perf_choose(const struct scmi_perf_domain *domain, uint32_t minimum,
                     uint32_t desired, uint32_t maximum, size_t *state);
