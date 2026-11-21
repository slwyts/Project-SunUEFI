/* SPDX-License-Identifier: BSD-2-Clause */
#include "rsc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { BASE = 0xd00, STRIDE = 0x2a0, ARM = 0x10000, TRIGGER = 0x1000000,
       DONE = 0x10100, WORDS = RPMH_APPS_DRV2_SIZE / 4 };
struct hw {
    uint32_t word[WORDS];
    uint32_t wait_off, msg_off, cmd_spacing, fail_read, stale_reg, stale_value;
    unsigned writes, triggers, acked, target;
};

static uint32_t slot(unsigned n) { return BASE + STRIDE*n; }
static uint32_t wait_reg(const struct hw *h, unsigned n) { return slot(n) + h->wait_off; }
static uint32_t msg(const struct hw *h, unsigned n, unsigned i)
{
    return slot(n) + h->msg_off + h->cmd_spacing*i;
}
static uint32_t *word(struct hw *h, uint32_t off)
{
    assert(!(off % 4) && off < RPMH_APPS_DRV2_SIZE);
    return &h->word[off/4];
}
static int read_reg(void *ctx, uint32_t off, uint32_t *v)
{
    struct hw *h = ctx;
    /* Include the fast-path and solver groups in the fixture, but forbid
     * even reading them; none participate in an AP active request. */
    assert(off < slot(3));
    if (h->fail_read == off) return 0;
    *v = h->stale_reg == off ? h->stale_value : *word(h, off);
    return 1;
}
static void write_reg(void *ctx, uint32_t off, uint32_t value)
{
    struct hw *h = ctx;
    assert(off == BASE || off == BASE+8 || (off >= wait_reg(h,h->target) && off < slot(h->target+1)));
    ++h->writes;
    if (off == BASE+8) {
        assert(value == (1u << h->target));
        *word(h,BASE+4) &= ~value;
        ++h->acked;
        return;
    }
    if (off == wait_reg(h,h->target)+4) {
        uint32_t previous = *word(h,off);
        /* The previous enable must be visible before the trigger. For
         * stopping, clear trigger first, then disable the sequencer. */
        if (value & TRIGGER) {
            assert(value & ARM);
            assert((previous & (ARM|TRIGGER)) == ARM);
            assert(*word(h,off+8) && (*word(h,BASE) & (1u<<h->target)));
            ++h->triggers;
            *word(h,off+4) = 0;
        } else if (!(value & ARM)) {
            assert(!(previous & TRIGGER));
        }
    }
    *word(h,off) = value;
}
static void configure(struct hw *h, unsigned major, unsigned ncpt)
{
    memset(h,0,sizeof(*h));
    h->fail_read = h->stale_reg = UINT32_MAX;
    h->wait_off = major == 3 ? 0x20 : 0x10;
    h->msg_off = major == 3 ? 0x34 : 0x30;
    h->cmd_spacing = major == 3 ? 24 : 20;
    *word(h,0) = major << 16 | 0x0700;
    *word(h,0x0c) = (ncpt << 27) | (8u << 12);
    for (unsigned i = 0; i < 3; ++i)
        *word(h,wait_reg(h,i)+8) = 1;
}
static void complete(struct hw *h, unsigned count, unsigned expected_mask)
{
    uint32_t mask = *word(h,wait_reg(h,h->target)+12);
    assert(mask == expected_mask && mask == *word(h,wait_reg(h,h->target)));
    for (unsigned i = 0; i < count; ++i)
        *word(h,msg(h,h->target,i)+12) = DONE;
    *word(h,wait_reg(h,h->target)+8) = 1;
    *word(h,BASE+4) |= 1u << h->target;
}

static void test_transaction(unsigned major)
{
    struct hw h;
    struct rpmh_rsc r = {0};
    const struct rpmh_rsc_io io = {&h,read_reg,write_reg};
    struct rpmh_rsc_command c[RPMH_TCS_MAX_COMMANDS];
    uint32_t result[RPMH_TCS_MAX_COMMANDS];
    configure(&h,major,16);
    for (unsigned i = 0; i < 16; ++i)
        c[i]=(struct rpmh_rsc_command){0x50000 + 4*i,0x42000001+i};
    assert(rpmh_rsc_init(&r,&io,RPMH_APPS_DRV2_BASE,RPMH_APPS_DRV2_SIZE) == RPMH_RSC_OK);
    /* TCS0 belongs to a previously programmed non-conflicting transfer.
     * There is a separate pre-existing pending IRQ for TCS2. */
    *word(&h,wait_reg(&h,0)+12) = 1;
    *word(&h,msg(&h,0,0)+4) = 0x40108;
    *word(&h,BASE+4) = 4;
    *word(&h,BASE) = 0xa4;
    h.target = 1;
    assert(rpmh_rsc_begin(&r,c,16,1)==RPMH_RSC_PENDING && h.triggers==1 && !rpmh_rsc_idle(&r));
    assert(!rpmh_rsc_interrupt(&r) && *word(&h,BASE+4)==4);
    assert(*word(&h,BASE)==0xa6);
    for (unsigned i=0;i<16;++i) {
        uint32_t m=msg(&h,1,i);
        assert(*word(&h,m)==0x10108 && *word(&h,m+4)==c[i].address && *word(&h,m+8)==c[i].data);
    }
    unsigned writes = h.writes;
    /* The old command's status is insufficient without a new completion IRQ. */
    *word(&h,wait_reg(&h,1)+8)=1;
    for (unsigned i=0;i<16;++i) *word(&h,msg(&h,1,i)+12)=DONE;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_PENDING);
    assert(rpmh_rsc_begin(&r,c,1,1)==RPMH_RSC_BUSY);
    assert(rpmh_rsc_cancel_unsent(&r)==RPMH_RSC_BUSY);
    assert(rpmh_rsc_init(&r,&io,RPMH_APPS_DRV2_BASE,RPMH_APPS_DRV2_SIZE)==RPMH_RSC_BUSY);
    assert(h.writes==writes);
    /* Even if the caller's wait expired, only the original slot completes. */
    complete(&h,15,0xffff);
    *word(&h,msg(&h,1,15)+12)=1u<<8;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_PENDING && h.writes==writes);
    *word(&h,msg(&h,1,15)+12)=DONE;
    h.fail_read=msg(&h,1,15)+12;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_IO_ERROR && !rpmh_rsc_idle(&r));
    h.fail_read=UINT32_MAX;
    *word(&h,BASE)|=0x40;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_OK && rpmh_rsc_idle(&r));
    assert(h.triggers==1 && h.acked==1 && *word(&h,BASE+4)==4 && *word(&h,BASE)==0xe4);
    assert(*word(&h,wait_reg(&h,0)+12)==1 && *word(&h,msg(&h,0,0)+4)==0x40108);
    /* Read every reply, including a valid all-ones data word. A short
     * output buffer doesn't consume the request. */
    *word(&h,BASE)|=2;
    assert(rpmh_rsc_begin(&r,c,2,0)==RPMH_RSC_PENDING);
    assert(*word(&h,msg(&h,1,0))==0x108 && *word(&h,msg(&h,1,1))==0x108);
    *word(&h,msg(&h,1,0)+16)=UINT32_MAX;
    *word(&h,msg(&h,1,1)+16)=0x51545800;
    complete(&h,2,3);
    assert(rpmh_rsc_interrupt(&r) && *word(&h,BASE+4)==4 && !rpmh_rsc_interrupt(&r));
    assert(rpmh_rsc_poll(&r,result,1)==RPMH_RSC_INVALID && !rpmh_rsc_idle(&r));
    assert(rpmh_rsc_poll(&r,result,2)==RPMH_RSC_OK && result[0]==UINT32_MAX && result[1]==0x51545800);
    assert(*word(&h,BASE)==0xe6 && h.acked==2);
}

int main(void)
{
    struct hw h;
    struct rpmh_rsc r = {0};
    const struct rpmh_rsc_io io = {&h,read_reg,write_reg};
    struct rpmh_rsc_command cmd = {0x40100, 0};
    configure(&h,3,3);
    assert(rpmh_rsc_init(&r,&io,0x16500000,RPMH_APPS_DRV2_SIZE)==RPMH_RSC_INVALID && !h.writes);
    assert(rpmh_rsc_init(&r,&io,RPMH_APPS_DRV2_BASE,RPMH_APPS_DRV2_SIZE-4)==RPMH_RSC_INVALID);
    *word(&h,0x0c)=(3u<<27)|2u<<12|63u;
    assert(rpmh_rsc_init(&r,&io,RPMH_APPS_DRV2_BASE,RPMH_APPS_DRV2_SIZE)==RPMH_RSC_INVALID);
    configure(&h,3,3);
    assert(rpmh_rsc_init(&r,&io,RPMH_APPS_DRV2_BASE,RPMH_APPS_DRV2_SIZE)==RPMH_RSC_OK);
    assert(rpmh_rsc_check_irq_handoff(&r)==RPMH_RSC_OK);
    *word(&h,BASE)=0x80;
    assert(rpmh_rsc_check_irq_handoff(&r)==RPMH_RSC_BUSY && !h.writes);
    *word(&h,BASE)=2; *word(&h,BASE+4)=2;
    assert(rpmh_rsc_check_irq_handoff(&r)==RPMH_RSC_BUSY && *word(&h,BASE+4)==2);
    *word(&h,BASE)=*word(&h,BASE+4)=0;
    assert(rpmh_rsc_begin(&r,&cmd,4,1)==RPMH_RSC_INVALID);
    /* Slot zero appears free but another active slot has an in-flight
     * different register of the same VRM. No controller writes are allowed. */
    *word(&h,wait_reg(&h,2)+12)=2;
    *word(&h,msg(&h,2,1)+4)=0x4010c;
    assert(rpmh_rsc_begin(&r,&cmd,1,1)==RPMH_RSC_BUSY && !h.writes);
    *word(&h,wait_reg(&h,2)+12)=0;
    h.stale_reg = msg(&h,0,0)+4; h.stale_value = 0x40104;
    assert(rpmh_rsc_begin(&r,&cmd,1,1)==RPMH_RSC_PENDING && !h.triggers);
    assert(rpmh_rsc_cancel_unsent(&r)==RPMH_RSC_CANCELLED && rpmh_rsc_idle(&r));
    assert(!h.triggers && !h.acked && !*word(&h,BASE) && !*word(&h,wait_reg(&h,0)+12));
    assert(rpmh_rsc_begin(&r,&cmd,1,1)==RPMH_RSC_PENDING && !h.triggers);
    h.stale_reg=UINT32_MAX;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_PENDING && h.triggers==1);
    complete(&h,1,1);
    h.stale_reg=wait_reg(&h,0)+4; h.stale_value=ARM|TRIGGER;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_PENDING && !h.acked && h.triggers==1);
    assert(rpmh_rsc_begin(&r,&cmd,1,1)==RPMH_RSC_BUSY);
    h.stale_reg=UINT32_MAX;
    assert(rpmh_rsc_poll(&r,NULL,0)==RPMH_RSC_OK && h.acked==1 && h.triggers==1);
    test_transaction(2);
    test_transaction(3);
    printf("APPS DRV2 %08x %x: active TCS 0..2, 2.7/3.0 command layouts, ACK and readback passed.\n",
           RPMH_APPS_DRV2_BASE, RPMH_APPS_DRV2_SIZE);
    return 0;
}
