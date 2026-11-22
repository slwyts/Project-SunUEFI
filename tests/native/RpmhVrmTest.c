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

struct aux_hw {
    uint32_t reg[RPMH_UFS_SUPPLY_COUNT][3];
    unsigned messages, fail_next, first[20];
};

static int write_aux(void *context, const struct rpmh_rsc_command *cmd, unsigned n, unsigned wait)
{
    struct aux_hw *h = context;
    assert(n && n <= 3 && wait == (1u << n)-1);
    unsigned rail = (cmd[0].address - 0x40000)/0x10;
    assert(rail < RPMH_UFS_SUPPLY_COUNT);
    if (h->messages < 20) h->first[h->messages] = rail;
    ++h->messages;
    for (unsigned j = 0; j < n; ++j) {
        unsigned off = cmd[j].address - (0x40000+rail*0x10);
        assert(off == 0 || off == 4 || off == 8);
        h->reg[rail][off/4] = cmd[j].data;
    }
    if (h->fail_next) { h->fail_next = 0; return 0; }
    return 1;
}

static void check_boot_supplies(void)
{
    const enum rpmh_ufs_supply REF = RPMH_UFS_REFCLK, PARENT = RPMH_UFS_VCCQ_PARENT;
    uint8_t b[HEADER+RPMH_UFS_SUPPLY_COUNT*ENTRY] = {0};
    const char *const names[] = {"ldob17","ldod1","ldoi2","smpi7","ldoi3","ldof1","ldoj1","ldog3"};
    u32(b+4,0x0c0330db); u16(b+8,RPMH_VRM); u16(b+12,RPMH_UFS_SUPPLY_COUNT*ENTRY); u16(b+14,RPMH_UFS_SUPPLY_COUNT);
    for (unsigned i = 0; i < RPMH_UFS_SUPPLY_COUNT; ++i) {
        memcpy(b+HEADER+i*ENTRY,names[i],strlen(names[i]));
        u32(b+HEADER+i*ENTRY+16,0x40000+i*0x10);
    }
    struct rpmh_cmd_db db;
    struct rpmh_provider p;
    struct aux_hw h = {.fail_next = 1};
    struct rpmh_transport io = {&h,write_aux};
    struct rpmh_client disk, wifi, bt, audio;
    assert(rpmh_db_open(&db,b,sizeof(b)) && rpmh_provider_start(&p,&db,&io,3));
    assert(rpmh_client_open(&p,&disk) && rpmh_client_open(&p,&wifi) && rpmh_client_open(&p,&bt) &&
           rpmh_client_open(&p,&audio));
    /* No previous AP votes are supplied. A failed boot request is never
     * marked applied; the still-pending enable is acknowledged on retry. */
    assert(!rpmh_provider_boot(&p,0,0,15,1) && p.vrm.restore);
    assert(rpmh_provider_boot(&p,0,0,15,1) && !p.vrm.restore && p.vrm.boot_holds == 15);
    assert(h.first[0] == REF && h.first[1] == REF && h.first[2] == PARENT &&
           h.first[3] == RPMH_UFS_VCC && h.first[4] == RPMH_UFS_VCCQ);
    assert(h.reg[PARENT][0] == 1340 && h.reg[PARENT][1] == 1 && h.reg[PARENT][2] == 7);
    struct rpmh_supply_request storage_ref = {.ua=100,.enable=1}, storage_parent = {.ua=210000,.enable=1};
    struct rpmh_supply_request wlan = {.min_mv=1312,.max_mv=1340,.enable=1};
    struct rpmh_supply_request codec = {.ua=5000,.min_mv=1200,.max_mv=1200,.enable=1};
    assert(rpmh_set_aux_supply(&p,&disk,REF,&storage_ref) && rpmh_set_aux_supply(&p,&disk,PARENT,&storage_parent));
    assert(rpmh_set_aux_supply(&p,&wifi,PARENT,&wlan) && rpmh_set_aux_supply(&p,&bt,PARENT,&wlan));
    assert(rpmh_set_aux_supply(&p,&audio,REF,&codec));
    assert(rpmh_set_ufs_supplies(&p,&disk,RPMH_SUPPLY_HIGH,RPMH_SUPPLY_HIGH));
    assert(rpmh_provider_boot(&p,0,0,15,0) && !p.vrm.boot_holds);
    assert(h.reg[PARENT][0] == 1312 && h.reg[PARENT][1] == 1 && h.reg[PARENT][2] == 7);
    assert(h.reg[REF][0] == 1200 && h.reg[REF][2] == 3);
    unsigned msg = h.messages;
    struct rpmh_supply_request clash = {.min_mv=1224,.max_mv=1256,.enable=1};
    assert(!rpmh_set_aux_supply(&p,&disk,PARENT,&clash) && h.messages == msg);
    assert(rpmh_client_close(&p,&disk));
    assert(h.reg[PARENT][0] == 1312 && h.reg[PARENT][1] == 1 && h.reg[PARENT][2] == 3);
    assert(h.reg[REF][1] == 1 && !h.reg[RPMH_UFS_VCC][1]);
    assert(rpmh_client_close(&p,&wifi) && h.reg[PARENT][1]);
    assert(rpmh_client_close(&p,&bt) && !h.reg[PARENT][1]);
    assert(rpmh_client_close(&p,&audio) && !h.reg[REF][1] && rpmh_provider_quiet(&p));

    /* Across D3, a still-open consumer's upper bound constrains the new AP
     * boot vote too. Do not briefly reprogram the rail outside this range. */
    struct rpmh_supply_request narrow = {.min_mv=1256,.max_mv=1264,.enable=1};
    assert(rpmh_client_open(&p,&disk) && rpmh_set_aux_supply(&p,&disk,PARENT,&narrow));
    rpmh_provider_power_lost(&p);
    assert(rpmh_provider_boot(&p,0,0,1u<<RPMH_UFS_VCC,1));
    assert(h.reg[PARENT][0]==1264 && h.reg[PARENT][1]);
    assert(rpmh_provider_boot(&p,0,0,(1u<<PARENT)|(1u<<RPMH_UFS_VCC),0));
    assert(h.reg[PARENT][0]==1256 && h.reg[PARENT][1] && rpmh_client_close(&p,&disk));
    /* A witnessed old request is not a probe hold and must not be erased
     * when other consumers finish enumerating. */
    rpmh_provider_power_lost(&p);
    struct rpmh_vrm_votes inherited = {.mask = 1u << PARENT, .vote[RPMH_UFS_VCCQ_PARENT] = {1224,1,3}};
    assert(rpmh_vrm_adopt(&p.vrm,&inherited));
    h.reg[PARENT][0] = 1224; h.reg[PARENT][1] = 1; h.reg[PARENT][2] = 3;
    assert(rpmh_provider_boot(&p,0,0,15,1) && rpmh_provider_boot(&p,0,0,15,0));
    assert(h.reg[PARENT][0] == 1224 && h.reg[PARENT][1] && h.reg[PARENT][2] == 3);
    assert(rpmh_client_open(&p,&disk));
    assert(rpmh_set_aux_supply(&p,&disk,PARENT,&storage_parent) && h.reg[PARENT][0] == 1224);
    assert(rpmh_client_close(&p,&disk) && h.reg[PARENT][0] == 1224 && h.reg[PARENT][1]);

    /* Display/USB put exact constraints on rails where the M-PHY accepts
     * a range. Do not use the board regulator's permissible MAX as a vote. */
    const enum rpmh_ufs_supply Q=RPMH_UFS_PHY_QREF, R=RPMH_UFS_PHY_REFGEN,
        A=RPMH_UFS_PHY_ANALOG, L=RPMH_UFS_PHY_PLL;
    unsigned phy = ((1u<<RPMH_UFS_SUPPLY_COUNT)-1) & ~15u;
    struct rpmh_client display,usb;
    const struct rpmh_supply_request qref={64500,880,912,1}, refgen={206000,880,920,1};
    const struct rpmh_supply_request analog={213000,912,920,1}, pll={18300,1200,1256,1};
    const struct rpmh_supply_request dsi={98000,880,880,1}, usb_pll={30000,1200,1200,1}, usb_ref={7000,880,880,1};
    assert(rpmh_provider_boot(&p,0,0,phy,1) && rpmh_client_open(&p,&disk));
    assert(h.reg[Q][0]==880 && h.reg[R][0]==880 && h.reg[A][0]==912 && h.reg[L][0]==1200);
    assert(rpmh_set_aux_supply(&p,&disk,Q,&qref) && rpmh_set_aux_supply(&p,&disk,R,&refgen) &&
           rpmh_set_aux_supply(&p,&disk,A,&analog) && rpmh_set_aux_supply(&p,&disk,L,&pll));
    assert(rpmh_client_open(&p,&usb) && rpmh_client_open(&p,&display));
    assert(rpmh_set_aux_supply(&p,&display,Q,&dsi) && rpmh_set_aux_supply(&p,&usb,L,&usb_pll) &&
           rpmh_set_aux_supply(&p,&usb,R,&usb_ref));
    assert(rpmh_provider_boot(&p,0,0,phy,0) && h.reg[L][2]==7);
    struct rpmh_supply_request too_high={18300,1248,1256,1};
    unsigned previous=h.messages;
    assert(!rpmh_set_aux_supply(&p,&disk,L,&too_high) && h.messages==previous);
    assert(rpmh_client_close(&p,&disk) && !h.reg[A][1] && h.reg[Q][1] && h.reg[R][1] && h.reg[L][1]);
    assert(h.reg[Q][2]==7 && h.reg[R][2]==4 && h.reg[L][2]==7);

    /* Learning an old PLL vote after unexpected D3 is not permission to
     * reissue a USB/M-PHY request before its power sequence reaches PLL. */
    rpmh_provider_power_lost(&p);
    unsigned pllbit=1u<<L;
    const struct rpmh_vrm_votes read_back={.mask=pllbit,.vote[RPMH_UFS_PHY_PLL]={1200,1,4}};
    assert(rpmh_vrm_adopt(&p.vrm,&read_back));
    previous=h.messages;
    assert(!rpmh_provider_can_reconcile(&p) && !rpmh_provider_reconcile(&p) && h.messages==previous);
    assert(rpmh_provider_boot(&p,0,0,(1u<<Q)|(1u<<R),1) && (p.vrm.deferred&pllbit));
    assert(h.reg[L][2]==7 && p.vrm.applied[L].mode==4);
    assert(rpmh_provider_boot(&p,0,0,pllbit,1) && !p.vrm.deferred && p.vrm.applied[L].mode==7);
    assert(rpmh_client_close(&p,&display) && rpmh_client_close(&p,&usb));
}

struct clock_hw { unsigned written, enabled, fail; };
static int write_clock(void *ctx,const struct rpmh_rsc_command *c,unsigned n,unsigned wait)
{
    struct clock_hw *h=ctx;
    assert(n==1 && wait==1 && c->address==0x45504 && c->data<=1);
    h->enabled=c->data;++h->written;
    if (h->fail) {--h->fail;return 0;}
    return 1;
}
static void reference_clock(void)
{
    uint8_t bytes[HEADER+ENTRY]={0};
    u32(bytes+4,0x0c0330db);u16(bytes+8,RPMH_VRM);u16(bytes+12,ENTRY);u16(bytes+14,1);
    memcpy(bytes+HEADER,"clka8",5);u32(bytes+HEADER+16,0x45500);
    struct rpmh_cmd_db db;struct rpmh_provider p;
    struct rpmh_client ufs,peer;struct clock_hw hw={0};
    struct rpmh_transport t={&hw,write_clock};
    unsigned mask=1u<<RPMH_UFS_REF_CLOCK;
    assert(rpmh_db_open(&db,bytes,sizeof(bytes)) && rpmh_provider_start(&p,&db,&t,4));
    assert(rpmh_client_open(&p,&ufs) && rpmh_client_open(&p,&peer));
    assert(rpmh_provider_boot(&p,mask,0,0,1) && hw.enabled && hw.written==1);
    assert(rpmh_set_level(&p,&ufs,RPMH_UFS_REF_CLOCK,1) && rpmh_set_level(&p,&peer,RPMH_UFS_REF_CLOCK,1));
    assert(rpmh_provider_boot(&p,mask,0,0,0) && hw.written==1);
    assert(rpmh_client_close(&p,&peer) && hw.enabled);
    rpmh_provider_power_lost(&p);hw.enabled=0;
    struct rpmh_boot_votes fresh={.rails=1u<<RPMH_UFS_REF_CLOCK};
    assert(rpmh_provider_adopt(&p,&fresh) && !rpmh_provider_reconcile(&p) && !hw.enabled);
    assert(rpmh_provider_boot(&p,mask,0,0,1) && hw.enabled && hw.written==2);
    hw.fail=1;
    assert(!rpmh_set_level(&p,&ufs,RPMH_UFS_REF_CLOCK,0) && !hw.enabled);
    assert(rpmh_provider_reconcile(&p) && hw.enabled && rpmh_client_close(&p,&ufs) && !hw.enabled);
}

int main(void)
{
    reference_clock();
    check_boot_supplies();
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
