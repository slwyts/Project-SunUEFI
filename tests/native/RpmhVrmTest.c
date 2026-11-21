/* SPDX-License-Identifier: BSD-2-Clause */
#include "provider.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { HEADER = 144, ENTRY = 24, VCC = 0x40230, VCCQ = 0x40260, XO = 0x30040, BOTH = 3 };
static void u16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void u32(uint8_t *p, uint32_t n) { u16(p, n); u16(p+2, n >> 16); }
static void make_db(uint8_t *b, struct rpmh_cmd_db *db, uint32_t q)
{
    memset(b, 0, HEADER + 3*ENTRY);
    u32(b+4, 0x0c0330db);
    u16(b+8, RPMH_VRM); u16(b+12, 3*ENTRY); u16(b+14, 2);
    u16(b+24, RPMH_ARC); u16(b+26, 2*ENTRY); u16(b+28, 3*ENTRY); u16(b+30, 1);
    memcpy(b+HEADER, "ldob17", 6); u32(b+HEADER+16, VCC);
    memcpy(b+HEADER+ENTRY, "ldod1", 5); u32(b+HEADER+ENTRY+16, q);
    memcpy(b+HEADER+2*ENTRY, "xo.lvl", 6); u32(b+HEADER+2*ENTRY+16, XO);
    assert(rpmh_db_open(db, b, HEADER + 3*ENTRY));
}

struct mock {
    struct rpmh_rsc_command log[256];
    unsigned written, calls, fail, fails_left, xo;
    uint32_t reg[2][3];
};

static int write_vrm(void *ctx, const struct rpmh_rsc_command *cmd, unsigned count, unsigned wait)
{
    struct mock *m = ctx;
    assert(count && count <= 3 && wait == (1u << count) - 1u);
    for (unsigned i = 0; i < count; ++i) {
        if (cmd[i].address == XO) {
            assert(count == 1);
            m->xo = cmd[i].data;
            continue;
        }
        unsigned supply = (cmd[i].address & ~15u) == VCCQ, reg = (cmd[i].address & 15u) / 4;
        assert((cmd[i].address & ~15u) == (supply ? VCCQ : VCC));
        assert(!(cmd[i].address & 3u) && reg < 3);
        assert(m->written < sizeof(m->log)/sizeof(*m->log));
        m->log[m->written++] = cmd[i];
        m->reg[supply][reg] = cmd[i].data;
    }
    ++m->calls;
    if (m->fails_left && m->calls >= m->fail) { --m->fails_left; return 0; }
    return 1;
}

static void check(const struct mock *m, unsigned index, uint32_t addr, uint32_t value)
{
    assert(index < m->written && m->log[index].address == addr && m->log[index].data == value);
}

int main(void)
{
    uint8_t bytes[HEADER + 3*ENTRY];
    struct rpmh_cmd_db db;
    struct mock m = {0};
    struct rpmh_transport transport = {&m, write_vrm};
    struct rpmh_provider p;
    struct rpmh_client disk, proxy;
    make_db(bytes, &db, VCC);
    assert(rpmh_provider_start(&p, &db, &transport, 3)); /* XO is independent of aliased LDOs. */
    assert(!p.vrm.address[0] && !p.vrm.address[1]);
    make_db(bytes, &db, VCCQ);
    assert(rpmh_provider_start(&p, &db, &transport, 3));
    assert(rpmh_client_open(&p, &disk) && rpmh_client_open(&p, &proxy));
    struct rpmh_vrm_votes floor = {.mask = 1};
    assert(rpmh_vrm_adopt(&p.vrm, &floor)); /* Verified AP VCC floor is zero. */
    assert(!rpmh_set_ufs_supplies(&p, &disk, RPMH_SUPPLY_HIGH, RPMH_SUPPLY_HIGH) && !m.written);
    floor.mask = 2; floor.vote[1].mv = 1300;
    assert(!rpmh_vrm_adopt(&p.vrm, &floor)); /* Never lower an out-of-range inherited request. */
    floor.vote[1] = (struct rpmh_vrm_vote){1100, 1, 4};
    m.reg[1][0] = 1100; m.reg[1][1] = 1; m.reg[1][2] = 4;
    assert(rpmh_vrm_adopt(&p.vrm, &floor));
    assert(rpmh_set_ufs_supplies(&p, &disk, RPMH_SUPPLY_HIGH, RPMH_SUPPLY_HIGH));
    assert(m.written == 5 && m.calls == 2);
    check(&m,0,VCC,2504); check(&m,1,VCC+8,7); check(&m,2,VCC+4,1);
    check(&m,3,VCCQ,1200); check(&m,4,VCCQ+8,7);
    assert(rpmh_set_ufs_supplies(&p, &proxy, RPMH_SUPPLY_OFF, RPMH_SUPPLY_HIGH) && m.written == 5);
    assert(rpmh_set_ufs_supplies(&p, &disk, RPMH_SUPPLY_OFF, RPMH_SUPPLY_LOW));
    check(&m,5,VCC+4,0); check(&m,6,VCC,0); check(&m,7,VCC+8,0);
    assert(m.written == 8 && m.reg[1][2] == 7); /* Proxy is still active. */
    assert(rpmh_client_close(&p, &proxy) && m.reg[1][2] == 4 && m.reg[1][1] == 1);

    m.fail = m.calls+2; m.fails_left = 2;
    assert(!rpmh_set_ufs_supplies(&p, &disk, RPMH_SUPPLY_HIGH, RPMH_SUPPLY_HIGH));
    assert(disk.ufs[0] == RPMH_SUPPLY_OFF && disk.ufs[1] == RPMH_SUPPLY_LOW && p.vrm.restore);
    assert(m.reg[0][1]); /* Retire the uncertain VCCQ change before turning VCC back off. */
    assert(rpmh_provider_reconcile(&p) && !p.vrm.restore && !m.reg[0][1] && m.reg[1][2] == 4);

    /* After a forced power transition a fresh AP floor is needed, even
     * when the client's request is identical to its request before D3. */
    unsigned count = m.written;
    rpmh_provider_power_lost(&p);
    assert(!rpmh_provider_reconcile(&p) && m.written == count);
    struct rpmh_boot_votes arc = {.rails = 1u << RPMH_XO};
    assert(rpmh_provider_adopt(&p, &arc));
    assert(rpmh_set_level(&p, &disk, RPMH_XO, 1) && m.xo == 3 && m.written == count);
    floor.vote[1] = (struct rpmh_vrm_vote){0}; m.reg[1][0] = m.reg[1][1] = m.reg[1][2] = 0;
    assert(rpmh_vrm_adopt(&p.vrm, &floor) && rpmh_provider_reconcile(&p));
    assert(m.reg[1][0] == 1200 && m.reg[1][1] == 1 && m.reg[1][2] == 4);
    assert(rpmh_client_close(&p, &disk) && rpmh_provider_quiet(&p) && !m.reg[1][1] && m.xo == 0);

    /* Aggregated load, not number of active high-mode clients, determines
     * LPM/HPM. Exercise the exact 10 mA VCC threshold with 1 mA votes. */
    struct rpmh_client c[10];
    floor.mask = BOTH;
    assert(rpmh_vrm_adopt(&p.vrm, &floor));
    for (unsigned i = 0; i < 10; ++i) {
        assert(rpmh_client_open(&p, &c[i]));
        assert(rpmh_set_ufs_supplies(&p, &c[i], RPMH_SUPPLY_LOW, RPMH_SUPPLY_OFF));
        assert(m.reg[0][2] == (i == 9 ? 7u : 4u));
    }
    for (unsigned i = 0; i < 10; ++i) assert(rpmh_client_close(&p, &c[i]));
    assert(!m.reg[0][1] && rpmh_provider_quiet(&p));
    /* A single-slot transport must still preserve register ordering. */
    p.slots = 1;
    assert(rpmh_client_open(&p, &disk));
    unsigned calls = m.calls;
    assert(rpmh_set_ufs_supplies(&p, &disk, RPMH_SUPPLY_HIGH, RPMH_SUPPLY_HIGH) && m.calls == calls+6);
    assert(rpmh_client_close(&p, &disk));
    puts("RPMh: UFS VCC/VCCQ, independent proxy, failed replies, load aggregation and resume passed.");
    return 0;
}
