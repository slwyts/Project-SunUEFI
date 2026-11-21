/* SPDX-License-Identifier: BSD-2-Clause */
#include "provider.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (1u << (n))
enum { HEAD = 144, ROW = 24, ARC_N = 3, BCM_N = 4,
       AUX = (ARC_N + BCM_N) * ROW, BYTES = HEAD + AUX + 8 * (ARC_N + BCM_N) };
static const char *const names[] = {"xo.lvl","cx.lvl","mx.lvl","CN0","CN1","QUP1","SH1"};

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p,v); put16(p+2,v >> 16); }
static uint32_t address(unsigned i) { return i < ARC_N ? 0x30000+i*4 : 0x50000+(i-ARC_N)*4; }
static void directory(uint8_t *p, unsigned group, unsigned start, unsigned count)
{
    put16(p,group); put16(p+2,start*ROW); put16(p+4,AUX); put16(p+6,count);
}

static void make_db(uint8_t *b)
{
    memset(b,0,BYTES);
    put32(b+4,0x0c0330db);
    directory(b+8,RPMH_ARC,0,ARC_N);
    directory(b+24,RPMH_BCM,ARC_N,BCM_N);
    for (unsigned i = 0; i < ARC_N+BCM_N; ++i) {
        uint8_t *e = b + HEAD + i*ROW, *aux = b + HEAD + AUX + i*8;
        memcpy(e,names[i],strlen(names[i]));
        put32(e+16,address(i)); put16(e+20,8); put16(e+22,i*8);
        if (i < ARC_N) {
            put16(aux+2,16); put16(aux+4,64); put16(aux+6,256);
        } else {
            put32(aux,1000); put16(aux+4,8); aux[6]=(uint8_t)(i-ARC_N);
        }
    }
}

struct hardware { uint32_t vote[ARC_N+BCM_N]; unsigned calls, fail_at, failures; };
static int write_group(void *context, const struct rpmh_rsc_command *cmd, unsigned n, unsigned wait)
{
    struct hardware *h = context;
    ++h->calls;
    assert(n && n <= RPMH_TCS_MAX_COMMANDS && wait == BIT(n - 1));
    for (unsigned i = 0; i < n; ++i) {
        unsigned j = 0;
        while (j < ARC_N+BCM_N && address(j) != cmd[i].address) ++j;
        assert(j < ARC_N+BCM_N);
        h->vote[j] = j < ARC_N ? cmd[i].data : (cmd[i].data & UINT32_C(0x0fffffff));
    }
    /* Hardware applied the message, but the acknowledgement may be late. */
    if (h->failures && h->calls >= h->fail_at) { --h->failures; return 0; }
    return 1;
}

int main(void)
{
    uint8_t b[BYTES];
    struct rpmh_cmd_db db = {0};
    struct rpmh_db_entry entry;
    make_db(b);
    assert(!rpmh_db_open(&db,b,HEAD-1) && !rpmh_db_open(&db,b,BYTES-1));
    assert(rpmh_db_open(&db,b,BYTES));
    assert(rpmh_db_find(&db,"cx.lvl",RPMH_ARC,&entry) && entry.address==address(1));
    assert(!rpmh_db_find(&db,"cx.lvl",RPMH_VRM,&entry));
    assert(!rpmh_db_find(&db,"cx.lvlxxx",RPMH_ARC,&entry));
    uint32_t c;
    assert(rpmh_arc_corner(&entry,65,&c) && c==3 && !rpmh_arc_corner(&entry,257,&c));
    struct hardware hw = {0};
    struct rpmh_provider p;
    struct rpmh_transport tr = { &hw,write_group };
    assert(rpmh_provider_start(&p,&db,&tr,16));
    struct rpmh_client ufs, keyboard;
    rpmh_client_open(&p,&ufs); rpmh_client_open(&p,&keyboard);
    assert(!rpmh_set_level(&p,&ufs,RPMH_CX,64) && !hw.calls);
    struct rpmh_boot_votes boot = {.rails=BIT(RPMH_CX)|BIT(RPMH_XO)};
    boot.corner[RPMH_CX] = hw.vote[1] = 1;
    assert(rpmh_provider_adopt(&p,&boot));
    assert(!rpmh_set_level(&p,&ufs,RPMH_MX,64));
    assert(rpmh_set_level(&p,&ufs,RPMH_XO,1) && rpmh_set_level(&p,&keyboard,RPMH_XO,1));
    assert(hw.calls==1 && hw.vote[0]==3 && !rpmh_provider_quiet(&p));
    assert(rpmh_set_level(&p,&ufs,RPMH_CX,64) && hw.vote[1]==2);
    hw.fail_at = hw.calls+1; hw.failures=1;
    assert(!rpmh_set_level(&p,&keyboard,RPMH_CX,256) && hw.vote[1]==3 && !keyboard.corner[RPMH_CX]);
    assert(rpmh_provider_reconcile(&p) && hw.vote[1]==2 && !p.restore_rails);
    assert(rpmh_set_level(&p,&ufs,RPMH_CX,256) && hw.vote[1]==3);
    hw.fail_at = hw.calls+1; hw.failures=2;
    assert(!rpmh_set_level(&p,&ufs,RPMH_CX,0));
    assert(hw.vote[1]==1 && ufs.corner[RPMH_CX]==3 && (p.restore_rails & BIT(RPMH_CX)));
    assert(!rpmh_provider_reconcile(&p) && p.restore_rails);
    assert(rpmh_provider_reconcile(&p) && hw.vote[1]==3 && !p.restore_rails);
    assert(rpmh_set_level(&p,&keyboard,RPMH_XO,0) && hw.vote[0]==3);
    assert(!rpmh_set_bandwidth(&p,&keyboard,RPMH_CPU_QUP1,400,800));
    boot.bus.mask = rpmh_fabric_bcm_mask(&p.fabric,BIT(RPMH_CPU_QUP1));
    assert(boot.bus.mask==(BIT(RPMH_BCM_CN0)|BIT(RPMH_BCM_CN1)|BIT(RPMH_BCM_SH1)));
    boot.bus.xy[RPMH_BCM_CN1] = hw.vote[4] = 2;
    assert(rpmh_provider_adopt(&p,&boot) && rpmh_set_bandwidth(&p,&keyboard,RPMH_CPU_QUP1,400,800));
    uint32_t shared = hw.vote[4];
    assert(shared>2 && hw.vote[6]==1);
    assert(rpmh_set_bandwidth(&p,&ufs,RPMH_CPU_QUP1,400,50) && hw.vote[4]>shared);

    /* Forced D3 between a posted down-vote and its acknowledgement. The new
     * floor is different; no cached rail index or bus demand describes it. */
    const uint32_t active_bus = hw.vote[4];
    hw.fail_at = hw.calls+1; hw.failures=1;
    assert(!rpmh_set_level(&p,&ufs,RPMH_CX,0) && hw.vote[1]==1);
    unsigned before = hw.calls;
    rpmh_provider_power_lost(&p);
    assert(!p.adopted && !rpmh_provider_can_reconcile(&p));
    memset(hw.vote,0,sizeof(hw.vote));
    struct rpmh_boot_votes resume = { .rails=boot.rails };
    resume.corner[RPMH_CX] = hw.vote[1] = 2;
    assert(rpmh_provider_adopt(&p,&resume));
    assert(!rpmh_provider_reconcile(&p) && hw.calls==before);
    resume.bus = boot.bus;
    hw.vote[4] = resume.bus.xy[RPMH_BCM_CN1];
    assert(rpmh_provider_adopt(&p,&resume));
    assert(rpmh_provider_can_reconcile(&p) && rpmh_provider_reconcile(&p));
    assert(hw.vote[0]==3 && hw.vote[1]==3 && hw.vote[4]==active_bus && hw.vote[6]==1);
    assert(!p.restore_rails && !p.restore_bcms);
    assert(rpmh_client_close(&p,&ufs) && hw.vote[4]==shared && !hw.vote[0] && hw.vote[1]==2);
    hw.fail_at=hw.calls+2; hw.failures=2;
    assert(!rpmh_client_close(&p,&keyboard));
    assert(p.restore_bcms && rpmh_provider_reconcile(&p));
    assert(!p.restore_bcms && hw.vote[4]==shared && hw.vote[6]==1);
    assert(!rpmh_provider_quiet(&p));
    assert(rpmh_client_close(&p,&keyboard) && rpmh_provider_quiet(&p));
    assert(hw.vote[4]==2 && !hw.vote[6] && !p.clients && !p.restore_bcms);
    assert(!rpmh_client_close(&p,&keyboard));

    /* No previous AP values. The provider establishes new hardware requests
     * before accepting drivers' demands; failed handoff restores the holds. */
    assert(rpmh_provider_start(&p,&db,&tr,3));
    hw=(struct hardware){0};
    unsigned domains = BIT(RPMH_XO) | BIT(RPMH_CX) | BIT(RPMH_MX);
    unsigned paths = BIT(RPMH_CPU_QUP1);
    assert(rpmh_provider_boot(&p,domains,paths,0,1) && p.boot_rails == domains);
    assert(hw.vote[0] == 3 && hw.vote[1] == 3 && hw.vote[2] == 3);
    assert(p.boot_bcms && hw.vote[4]);
    assert(rpmh_client_open(&p,&keyboard));
    assert(rpmh_set_level(&p,&keyboard,RPMH_CX,16) && rpmh_set_bandwidth(&p,&keyboard,RPMH_CPU_QUP1,2,40));
    unsigned calls = hw.calls;
    assert(rpmh_provider_boot(&p,domains,paths,0,1) && hw.calls == calls);
    hw.fail_at=hw.calls+1; hw.failures=1;
    assert(!rpmh_provider_boot(&p,domains,paths,0,0) && p.boot_rails == domains && p.boot_bcms);
    assert(rpmh_provider_boot(&p,domains,paths,0,0) && !p.boot_rails && !p.boot_bcms);
    assert(!hw.vote[0] && hw.vote[1] == 1 && !hw.vote[2] && hw.vote[4] && rpmh_client_close(&p,&keyboard));

    /* The first driver back from forced D3 does not own the QUP route.
     * Re-establish the keyboard's accepted vote without re-opening it. */
    assert(rpmh_provider_boot(&p,0,BIT(RPMH_QUP1_CORE),0,1) && rpmh_client_open(&p,&keyboard));
    assert(rpmh_set_level(&p,&keyboard,RPMH_CX,16) && rpmh_set_bandwidth(&p,&keyboard,RPMH_QUP1_CORE,19200,0));
    assert(rpmh_provider_boot(&p,0,BIT(RPMH_QUP1_CORE),0,0));
    uint32_t before_suspend=hw.vote[5];
    assert(before_suspend);
    rpmh_provider_power_lost(&p);
    memset(hw.vote,0,sizeof(hw.vote));
    assert(rpmh_provider_boot(&p,BIT(RPMH_MX),0,0,1) && !p.restore_bcms && !p.restore_rails);
    assert(hw.vote[1] >= 1 && hw.vote[5] >= before_suspend);
    assert(rpmh_provider_boot(&p,BIT(RPMH_MX)|BIT(RPMH_CX),paths|BIT(RPMH_QUP1_CORE),0,0));
    assert(hw.vote[1] == 1 && hw.vote[5] == before_suspend && rpmh_client_close(&p,&keyboard));
    puts("RPMh: transferred AP floor, shared client votes, acknowledged VCDs and late writes verified.");
    return 0;
}
