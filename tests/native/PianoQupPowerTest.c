/* SPDX-License-Identifier: BSD-2-Clause */
#include "gcc_vote.h"
#include "piano_qup_power.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (UINT32_C(1) << (n))
struct fixture {
    uint32_t word, drop_bits, keep_bits, cfg, cmd, pads[2];
    unsigned toggles, reg_writes, pin_writes, usec, fabric, reserved, remote;
    int deny_fabric, deny_pads, block_restore, stall_update, lose_readback, unreadable;
    struct gcc_vote_word votes;
    struct piano_qup_power p;
    struct piano_qup_io clk;
    struct piano_qup_board_io board;
};

static int read_word(void *ctx, uint32_t *value)
{
    struct fixture *f = ctx;
    if (f->unreadable) return 0;
    *value = f->word;
    return 1;
}
static void write_word(void *ctx, uint32_t v)
{
    struct fixture *f = ctx;
    f->word = (v & ~f->drop_bits) | f->keep_bits;
    if (f->lose_readback) f->unreadable = 1;
    ++f->toggles;
}
static int vote(void *ctx, unsigned bit, int acquire)
{
    struct fixture *f = ctx;
    assert(f->fabric && bit < 32);
    return gcc_vote_set_locked(&f->votes, bit, acquire);
}
static uint32_t read_reg(void *ctx, enum piano_qup_reg r)
{
    struct fixture *f = ctx;
    assert(f->fabric && r < QUP_CLOCK_REGS);
    if (r == QUP_SE6_CFG) return f->cfg;
    if (r == QUP_SE6_CMD) return f->cmd;
    if (r == QUP_SE6 && f->remote) return 0;
    return (f->word & BIT(piano_qup_vote_bits[r])) ? 0 : BIT(31);
}
static void write_reg(void *ctx, enum piano_qup_reg r, uint32_t v)
{
    struct fixture *f = ctx;
    assert(f->fabric && (r == QUP_SE6_CMD || r == QUP_SE6_CFG));
    ++f->reg_writes;
    if (r == QUP_SE6_CFG) {
        assert((f->cmd & BIT(1)) && !(f->cmd & BIT(31)));
        f->cfg = v;
    } else {
        f->cmd = v & ~BIT(31);
        if (!f->stall_update) f->cmd &= ~1u;
        if (!(v & 2)) f->cmd |= BIT(31);
    }
}
static void wait_us(void *ctx, unsigned us) { ((struct fixture *)ctx)->usec += us; }
static int fabric(void *ctx, int acquire)
{
    struct fixture *f = ctx;
    assert(f->fabric == (unsigned)!acquire);
    if (f->deny_fabric) return 0;
    if (!acquire) assert(!f->p.clocks.held && !f->reserved);
    f->fabric = (unsigned)acquire;
    return 1;
}
static int pads(void *ctx, int acquire)
{
    struct fixture *f = ctx;
    assert(f->fabric && f->reserved == (unsigned)!acquire);
    if (f->deny_pads) return 0;
    f->reserved = (unsigned)acquire;
    return 1;
}
static uint32_t read_pad(void *ctx, unsigned pin)
{
    struct fixture *f = ctx;
    assert(f->reserved && (pin == 56 || pin == 57));
    return f->pads[pin-56];
}
static void write_pad(void *ctx, unsigned pin, uint32_t v)
{
    struct fixture *f = ctx;
    assert(f->reserved && (pin == 56 || pin == 57));
    ++f->pin_writes;
    if (pin != 57 || !(v & 1) || !f->block_restore)
        f->pads[pin-56] = v;
}
static void fresh(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->cfg = BIT(26) | 0x201;
    f->cmd = BIT(31);
    f->pads[0] = BIT(25) | 1; f->pads[1] = BIT(11) | 1;
    f->votes = (struct gcc_vote_word){.context=f, .read=read_word, .write=write_word};
    f->clk = (struct piano_qup_io){f, read_reg, write_reg, wait_us, vote};
    f->board = (struct piano_qup_board_io){f, fabric, pads, read_pad, write_pad};
}
static int power(struct fixture *f, int on)
{
    return piano_qup_set_power(&f->p, &f->clk, &f->board, on);
}

int main(void)
{
    struct fixture f;
    fresh(&f);
    /* An inherited wrapper gate, a peer SE's vote and unrelated bits in
     * the same word must all survive the SE6 client's D0/D3 transition.
     */
    f.word = BIT(20) | BIT(15);
    assert(gcc_vote_set_locked(&f.votes, 18, 1));
    assert(power(&f, 1) && f.p.ready && f.fabric && f.reserved);
    assert(f.cfg == (BIT(26) | 1) && (f.word & BIT(28)) && f.votes.users[18] == 2);
    assert(f.pads[0] == (BIT(25) | 4) && f.pads[1] == (BIT(11) | BIT(12) | 4));
    unsigned operations = f.toggles + f.pin_writes + f.reg_writes;
    assert(power(&f, 1) && operations == f.toggles + f.pin_writes + f.reg_writes);
    assert(gcc_vote_set_locked(&f.votes, 2, 1));
    f.pads[0] |= BIT(24);
    assert(power(&f, 0) && !f.p.ready && !f.fabric);
    assert(f.cfg == BIT(26) && f.word == (BIT(20) | BIT(18) | BIT(15) | BIT(2)));
    assert(f.pads[0] == (BIT(25) | BIT(24) | 1) && f.pads[1] == (BIT(11) | 1));
    assert(gcc_vote_set_locked(&f.votes, 18, 0) && gcc_vote_set_locked(&f.votes, 2, 0));
    assert(f.word == (BIT(20) | BIT(15)));
    assert(power(&f, 0) && power(&f, 1) && power(&f, 0));

    fresh(&f); f.remote=1;
    assert(!power(&f,1) && !f.reg_writes && !f.fabric && !f.pin_writes);
    f.cfg=0;
    assert(power(&f,1) && power(&f,0) && !f.reg_writes);

    fresh(&f); f.deny_fabric=1;
    assert(!power(&f,1) && !f.toggles && !f.reg_writes && !f.pin_writes);
    fresh(&f); f.word=UINT32_MAX;
    assert(!power(&f,1) && !f.toggles && !f.fabric);
    fresh(&f); f.lose_readback=1;
    assert(!power(&f,1) && f.fabric && f.p.clocks.held && f.votes.uncertain==BIT(20));
    f.lose_readback=f.unreadable=0;
    assert(!gcc_vote_set_locked(&f.votes,20,1));
    assert(power(&f,0) && !f.fabric && !f.word && !f.votes.uncertain);
    fresh(&f); f.drop_bits=BIT(21);
    assert(!power(&f,1) && !f.word && !f.fabric && !f.pin_writes);
    fresh(&f); f.deny_pads=1;
    assert(!power(&f,1) && !f.fabric && !f.word && f.cfg==BIT(26));

    fresh(&f); f.stall_update=1;
    assert(!power(&f,1) && !f.p.ready && f.fabric && f.p.clocks.park_needed && !f.pin_writes);
    f.stall_update=0; f.cmd &= ~1u;
    assert(power(&f,0) && !f.fabric && !f.word);
    fresh(&f); assert(power(&f,1)); f.block_restore=1;
    assert(!power(&f,0) && f.reserved && f.fabric && (f.word & BIT(28)));
    f.block_restore=0; assert(power(&f,0) && !f.fabric && !f.word);
    fresh(&f); assert(power(&f,1)); f.keep_bits=BIT(28);
    assert(!power(&f,0) && f.fabric && f.votes.users[28]==1 && (f.word & BIT(28)));
    f.keep_bits=0; assert(power(&f,0) && !f.word && !f.fabric);
    fresh(&f); assert(power(&f,1)); f.lose_readback=1;
    assert(!power(&f,0) && f.fabric && f.votes.uncertain==BIT(28));
    f.lose_readback=f.unreadable=0;
    assert(power(&f,0) && !f.fabric && !f.word && !f.votes.uncertain);
    fresh(&f); assert(power(&f,1)); f.remote=1;
    assert(power(&f,0) && f.cfg==(BIT(26)|1) && !f.word && !f.fabric);
    fresh(&f); assert(!gcc_vote_set_locked(&f.votes,31,0) && !gcc_vote_set_locked(&f.votes,32,1));
    f.votes.users[18]=UINT_MAX;
    assert(!gcc_vote_set_locked(&f.votes,18,1) && !f.toggles);

    /* The Python test compares these compiled values with the real DT and
     * ROM clock records rather than duplicating the C register constants.
     */
    printf("%x", PIANO_GCC_QUP_VOTE_ADDRESS);
    for (unsigned i=0; i<QUP_CLOCK_REGS; ++i) printf(" %x", piano_qup_clock_address[i]);
    for (unsigned i=0; i<=QUP_SE6; ++i) printf(" %x", BIT(piano_qup_vote_bits[i]));
    for (unsigned i=0; i<2; ++i) printf(" %x", piano_qup_i2c_pins[i]);
    puts("");
    return 0;
}
