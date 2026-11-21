/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scmi_channel.h"

/* An SCMI server speaking the wire protocol across the same 512-byte slot
 * used by the Windows transfer path. Domains need not have identical OPPs. */
struct cpucp {
    uint32_t ram[128], version, command, token_header, current, flags;
    uint64_t clock;
    unsigned calls, writes, sets, delay, bad, skip[64], pages, count;
};

static uint32_t selector(const struct cpucp *c, unsigned i)
{
    return c->flags & (1u << 25) ? (i * 13u + 7u) : (i + 1) * 100u;
}

static void answer(struct cpucp *s)
{
    uint32_t *b = s->ram;
    uint32_t domain = b[7], skip = b[8], n = 0, status = 0;
    unsigned v4 = s->version >> 16 == 4;
    switch (s->command) {
    case 0: b[8] = s->version; n = 1; break;
    case 1: b[8] = 2; b[9] = b[10] = b[11] = 0; n = 4; break;
    case 3:
        assert(domain < 2);
        b[8] = s->flags;
        b[9] = 234;
        b[10] = 1800000;
        b[11] = 1800;
        memset(b + 12, 0, 16);
        n = 8;
        break;
    case 4: {
        assert(domain < 2 && skip < s->count && s->pages < 64);
        s->skip[s->pages++] = skip;
        unsigned number = s->count - skip;
        if (number > 3) number = 3;  /* Pagination offset is not an OPP selector. */
        b[8] = number | (s->count - skip - number) << 16;
        n = 1;
        for (unsigned i = skip; i < skip + number; ++i) {
            unsigned pos = s->count - 1 - i;  /* Independent of firmware ordering. */
            b[8 + n++] = (pos + 1) * 100;
            b[8 + n++] = (pos + 1) * 31;
            b[8 + n++] = 4 + pos;
            if (v4) {
                b[8 + n++] = 300000 + pos * 120000;
                b[8 + n++] = s->bad == 4 ? 7 : selector(s, pos);
            }
        }
        if (s->bad == 1) n--;
        if (s->bad == 2 && skip) b[8] += 1u << 16;
        break;
    }
    case 7:
        assert(domain < 2);
        ++s->sets;
        if (s->bad == 3) status = (uint32_t)-3;
        else s->current = skip;
        break;
    case 8: assert(domain < 2); n = 1; b[8] = s->current; break;
    default: assert(0);
    }
    b[7] = status;
    b[5] = (n + 2u) * 4;
    b[6] = s->token_header ^ (s->bad == 5 ? (1u << 18) : 0);
    b[1] = s->bad == 6 ? 3 : 1;
}

static uint32_t rd(void *ctx, unsigned offset)
{
    struct cpucp *s = ctx;
    assert(!(offset & 3) && offset < sizeof(s->ram));
    return s->ram[offset / 4];
}

static void wr(void *ctx, unsigned offset, uint32_t v)
{
    struct cpucp *s = ctx;
    assert(!(offset & 3) && offset < sizeof(s->ram));
    s->ram[offset / 4] = v;
    ++s->writes;
}

static void ring(void *ctx, uint32_t header)
{
    struct cpucp *s = ctx;
    assert(!s->ram[1] && !s->ram[4] && s->ram[6] == header && (header >> 10 & 0xff) == 0x13);
    assert(s->ram[5] >= 4 && s->ram[5] <= sizeof(s->ram) - 24);
    s->token_header = header;
    s->command = header & 0xff;
    ++s->calls;
    if (!s->delay) answer(s);
}

static uint64_t clock_us(void *ctx) { return ((struct cpucp *)ctx)->clock; }
static void wait_slot(void *ctx)
{
    struct cpucp *s = ctx;
    ++s->clock;
    if (s->delay && !--s->delay) answer(s);
}

static void run(unsigned version, int indexed)
{
    struct cpucp s = { .version = version << 16, .flags = 1u << 30 | (unsigned)indexed << 25,
                       .current = 123, .count = 21 };
    struct scmi_channel ch;
    struct scmi_channel_io io = { &s, rd, wr, ring, clock_us, wait_slot };
    assert(!scmi_channel_init(&ch, &io, sizeof(s.ram), 10));
    struct scmi_transport t = { &ch, scmi_channel_message };
    struct scmi_perf perf = {0};
    s.ram[1] = 1;
    assert(!scmi_perf_open(&t, &perf) && perf.domains == 2 && perf.version == s.version);
    if (indexed) assert(version == 4);
    struct scmi_perf_domain domain = {0};
    size_t needed, state;
    assert(scmi_perf_describe(&perf, 1, NULL, 0, &domain, &needed) == SCMI_PERF_NO_SPACE);
    assert(needed == s.count && s.pages == 1 && s.sets == 0 && !domain.count);
    s.pages = 0;
    struct scmi_perf_state table[21];
    assert(!scmi_perf_describe(&perf, 1, table, 21, &domain, &needed));
    assert(needed == s.count && s.pages == 7 && s.skip[1] == 3 && s.skip[6] == 18 && s.sets == 0);
    assert(domain.id == 1 && domain.count == 21 && domain.rate_limit_us == 234);
    for (unsigned i = 0; i < 21; ++i) {
        unsigned pos = 20 - i;
        assert(table[i].performance == (pos+1)*100 && table[i].selector == selector(&s, pos));
        assert(table[i].khz == (indexed ? 300000 + pos * 120000 : (pos+1)*100000));
        assert(table[i].transition_us == 4 + pos);
    }
    assert(!scmi_perf_choose(&domain, 200, 455, 700, &state) && domain.states[state].performance == 500);
    assert(!scmi_perf_choose(&domain, 200, 455, 470, &state) && domain.states[state].performance == 400);
    assert(!scmi_perf_choose(&domain, 501, 300, 807, &state) && domain.states[state].performance == 600);
    assert(scmi_perf_choose(&domain, 410, 430, 450, &state) == SCMI_PERF_NOT_FOUND);
    assert(!scmi_perf_set(&perf, &domain, 2) && s.current == selector(&s, 18));
    s.current = selector(&s, 4); /* The firmware can thermal-limit a previous request. */
    assert(!scmi_perf_get(&perf, &domain, &state) && state == 16);
    unsigned sent = s.sets;
    domain.can_set = 0;
    assert(scmi_perf_set(&perf, &domain, 0) == SCMI_PERF_DENIED && s.sets == sent);
    domain.can_set = 1; s.bad = 3;
    assert(scmi_perf_set(&perf, &domain, 0) == -3 && s.sets == sent + 1);
    for (s.bad = 1; s.bad <= 2; ++s.bad) {
        struct scmi_perf_domain unpublished = {0};
        assert(scmi_perf_describe(&perf, 1, table, 21, &unpublished, &needed) == SCMI_PERF_PROTOCOL_ERROR);
        assert(!unpublished.count);
    }
    if (indexed) {
        s.bad = 4;
        assert(scmi_perf_describe(&perf, 1, table, 21, &domain, &needed) == SCMI_PERF_PROTOCOL_ERROR);
    }
    s.bad = 5;
    assert(scmi_perf_open(&t, &perf) == SCMI_PERF_PROTOCOL_ERROR);
    s.bad = 6;
    assert(scmi_perf_open(&t, &perf) == SCMI_PERF_PROTOCOL_ERROR);
    s.bad = 0;
    s.delay = 31;
    assert(scmi_perf_open(&t, &perf) == SCMI_PERF_TIMEOUT && ch.pending);
    unsigned called = s.calls, writes = s.writes;
    assert(scmi_perf_open(&t, &perf) == SCMI_PERF_TIMEOUT && s.calls == called && s.writes == writes);
    assert(scmi_channel_drain(&ch) == SCMI_PERF_TIMEOUT && ch.pending);
    assert(!scmi_channel_drain(&ch) && !ch.pending && s.calls == called);
    assert(!scmi_perf_open(&t, &perf) && s.calls == called + 2);
    s.ram[1] = 0;
    assert(scmi_perf_open(&t, &perf) == SCMI_PERF_BUSY && !ch.pending);
}

int main(void)
{
    run(3, 0);
    run(4, 0);
    run(4, 1);
    puts("CPUCP SCMI: paginated PERF, exact selectors, bounded requests and late completions passed.");
    return 0;
}
