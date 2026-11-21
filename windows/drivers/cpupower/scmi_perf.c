/* SPDX-License-Identifier: BSD-2-Clause */
#include "scmi_perf.h"
#include <stdlib.h>

enum {
    PROTOCOL_VERSION = 0, PROTOCOL_ATTRIBUTES = 1, DOMAIN_ATTRIBUTES = 3,
    DESCRIBE_LEVELS = 4, LEVEL_SET = 7, LEVEL_GET = 8
};

static int request(const struct scmi_transport *t, unsigned cmd, const uint32_t *args,
                   size_t nargs, uint32_t *out, size_t words)
{
    size_t n = 0;
    int err = t->message(t->context, SCMI_PERF_PROTOCOL, cmd, args, nargs, out, words, &n);
    if (err) return err;
    return n == words ? 0 : SCMI_PERF_PROTOCOL_ERROR;
}

int scmi_perf_open(const struct scmi_transport *transport, struct scmi_perf *perf)
{
    uint32_t version, attr[4];
    if (!transport || !transport->message || !perf) return SCMI_PERF_INVALID;
    int err = request(transport, PROTOCOL_VERSION, NULL, 0, &version, 1);
    if (err) return err;
    if ((version >> 16) < 1 || (version >> 16) > 4) return SCMI_PERF_UNSUPPORTED;
    err = request(transport, PROTOCOL_ATTRIBUTES, NULL, 0, attr, 4);
    if (err) return err;
    *perf = (struct scmi_perf){ *transport, version, attr[0] & 0xffff };
    return 0;
}

static int compare_performance(const void *a, const void *b)
{
    const struct scmi_perf_state *sa = a, *sb = b;
    return (sa->performance < sb->performance) - (sa->performance > sb->performance);
}

static int compare_selector(const void *a, const void *b)
{
    const struct scmi_perf_state *sa = a, *sb = b;
    return (sa->selector > sb->selector) - (sa->selector < sb->selector);
}

int scmi_perf_describe(const struct scmi_perf *perf, uint32_t id,
                       struct scmi_perf_state *storage, size_t capacity,
                       struct scmi_perf_domain *domain, size_t *needed)
{
    if (!perf || !perf->transport.message || id >= perf->domains || !domain || !needed ||
        (!storage && capacity)) return SCMI_PERF_INVALID;
    uint32_t attr[8], page[SCMI_PERF_REPLY_WORDS], args[2] = { id, 0 };
    int err = request(&perf->transport, DOMAIN_ATTRIBUTES, args, 1, attr, 8);
    if (err) return err;
    int v4 = (perf->version >> 16) == 4;
    int indexed = v4 && (attr[0] & (1u << 25));
    size_t stride = v4 ? 5u : 3u, total = 0, done = 0;
    for (;;) {
        size_t n = 0;
        args[1] = (uint32_t)done;  /* Enumeration offset, not the level selector. */
        err = perf->transport.message(perf->transport.context, SCMI_PERF_PROTOCOL, DESCRIBE_LEVELS,
                                      args, 2, page, SCMI_PERF_REPLY_WORDS, &n);
        if (err) return err;
        if (!n || n > SCMI_PERF_REPLY_WORDS) return SCMI_PERF_PROTOCOL_ERROR;
        size_t returned = page[0] & 0xfff, remaining = page[0] >> 16;
        if (!returned || n != 1 + returned * stride) return SCMI_PERF_PROTOCOL_ERROR;
        if (!done) {
            total = returned + remaining;
            *needed = total;
            if (capacity < total) return SCMI_PERF_NO_SPACE;
        } else if (done + returned + remaining != total) {
            return SCMI_PERF_PROTOCOL_ERROR;
        }
        for (size_t i = 0; i < returned; ++i) {
            const uint32_t *p = page + 1 + i * stride;
            uint64_t khz;
            if (indexed) khz = p[3];
            else if (attr[2] && attr[3]) khz = (uint64_t)p[0] * attr[2] / attr[3];
            else return SCMI_PERF_UNSUPPORTED;  /* No conversion from abstract units to kHz. */
            if (!khz || khz > UINT32_MAX) return SCMI_PERF_PROTOCOL_ERROR;
            storage[done + i] = (struct scmi_perf_state){ p[0], (uint32_t)khz,
                                                          indexed ? p[4] : p[0], p[2] & 0xffff };
        }
        done += returned;
        if (!remaining) break;
    }
    if (indexed) {
        qsort(storage, total, sizeof(*storage), compare_selector);
        for (size_t i = 1; i < total; ++i)
            if (storage[i-1].selector == storage[i].selector) return SCMI_PERF_PROTOCOL_ERROR;
    }
    qsort(storage, total, sizeof(*storage), compare_performance);
    for (size_t i = 1; i < total; ++i)
        if (storage[i-1].performance == storage[i].performance) return SCMI_PERF_PROTOCOL_ERROR;
    *domain = (struct scmi_perf_domain){ id, attr[2], attr[3], attr[1] & 0xfffff,
                                         (attr[0] >> 30) & 1u, total, storage };
    return 0;
}

int scmi_perf_get(const struct scmi_perf *perf, const struct scmi_perf_domain *domain, size_t *state)
{
    uint32_t value;
    if (!perf || !perf->transport.message || !domain || !domain->states || !state || domain->id >= perf->domains)
        return SCMI_PERF_INVALID;
    int err = request(&perf->transport, LEVEL_GET, &domain->id, 1, &value, 1);
    if (err) return err;
    for (size_t i = 0; i < domain->count; ++i)
        if (domain->states[i].selector == value) { *state = i; return 0; }
    return SCMI_PERF_NOT_FOUND;
}

int scmi_perf_set(const struct scmi_perf *perf, const struct scmi_perf_domain *domain, size_t state)
{
    if (!perf || !perf->transport.message || !domain || !domain->states || domain->id >= perf->domains ||
        state >= domain->count) return SCMI_PERF_INVALID;
    if (!domain->can_set) return SCMI_PERF_DENIED;
    uint32_t args[] = { domain->id, domain->states[state].selector };
    return request(&perf->transport, LEVEL_SET, args, 2, NULL, 0);
}

int scmi_perf_choose(const struct scmi_perf_domain *domain, uint32_t minimum,
                     uint32_t desired, uint32_t maximum, size_t *state)
{
    if (!domain || !domain->states || !state || minimum > maximum) return SCMI_PERF_INVALID;
    size_t choice = domain->count;
    for (size_t i = 0; i < domain->count; ++i) {
        uint32_t level = domain->states[i].performance;
        if (level > maximum) continue;
        if (level < minimum) break;
        if (choice == domain->count || level >= desired) choice = i;
        if (level <= desired) break;
    }
    if (choice == domain->count) return SCMI_PERF_NOT_FOUND;
    *state = choice;
    return 0;
}
