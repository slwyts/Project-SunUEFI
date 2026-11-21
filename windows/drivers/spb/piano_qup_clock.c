/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_qup_clock.h"
#include "gcc_vote.h"

const uint32_t piano_qup_clock_address[QUP_CLOCK_REGS] = {
    0x00123140, 0x00123144, 0x0012315c, 0x00123148, 0x00118644,
    0x00118654, 0x00118658
};
const unsigned piano_qup_vote_bits[QUP_SE6 + 1] = {20, 21, 18, 19, 28};
#define CMD_UPDATE 1u
#define CMD_ROOT_EN 2u
#define CLK_OFF (UINT32_C(1) << 31)
#define HW_CG 2u
#define NOC_FSM_MASK (7u << 28)
#define NOC_FSM_ON (2u << 28)
/* RCG2: GCC parent 0 is the 19.2 MHz crystal. A HID divider field of 1
 * gives divide-by-one, with fractional mode and HW control both disabled.
 * M/N/D are inactive in this mode and are never modified.
 */
#define CFG_RATE ((1u << 20) | (3u << 12) | (7u << 8) | 31u)
#define CFG_XO 1u

static int valid(const struct piano_qup_io *io)
{
    return io && io->read && io->write && io->wait_us && io->vote;
}

static int branch_on(uint32_t v)
{
    return v != UINT32_MAX && (!(v & CLK_OFF) || (v & NOC_FSM_MASK) == NOC_FSM_ON);
}

static int wait_branch(const struct piano_qup_io *io, enum piano_qup_reg reg)
{
    for (unsigned i = 0; i < 200; ++i) {
        uint32_t v = io->read(io->context, reg);
        if (v == UINT32_MAX) return 0;
        if (branch_on(v) || ((reg == QUP_M_AHB || reg == QUP_S_AHB) && (v & HW_CG))) return 1;
        io->wait_us(io->context, 1);
    }
    return 0;
}

static int wait_command(const struct piano_qup_io *io, uint32_t mask)
{
    for (unsigned i = 0; i < 500; ++i) {
        uint32_t cmd = io->read(io->context, QUP_SE6_CMD);
        if (cmd == UINT32_MAX) return 0;
        if (!(cmd & mask)) return 1;
        io->wait_us(io->context, 1);
    }
    return 0;
}

static int unforce(struct piano_qup_clocks *c, const struct piano_qup_io *io)
{
    if (!c->forced) return 1;
    uint32_t cmd = io->read(io->context, QUP_SE6_CMD);
    if (cmd == UINT32_MAX || (cmd & CMD_UPDATE)) return 0;
    io->write(io->context, QUP_SE6_CMD, cmd & ~CMD_ROOT_EN);
    if (io->read(io->context, QUP_SE6_CMD) & CMD_ROOT_EN) return 0;
    c->forced = 0;
    return 1;
}

static int set_cfg(struct piano_qup_clocks *c, const struct piano_qup_io *io, uint32_t rate)
{
    if (!wait_command(io, CMD_UPDATE)) return 0;
    uint32_t cmd = io->read(io->context, QUP_SE6_CMD);
    if (cmd == UINT32_MAX) return 0;
    if (!(cmd & CMD_ROOT_EN)) {
        c->forced = 1;
        io->write(io->context, QUP_SE6_CMD, cmd | CMD_ROOT_EN);
    }
    if (!(io->read(io->context, QUP_SE6_CMD) & CMD_ROOT_EN) || !wait_command(io, CLK_OFF))
        return 0;
    uint32_t cfg = io->read(io->context, QUP_SE6_CFG);
    if (cfg == UINT32_MAX) return 0;
    io->write(io->context, QUP_SE6_CFG, (cfg & ~CFG_RATE) | rate);
    cmd = io->read(io->context, QUP_SE6_CMD);
    if (cmd == UINT32_MAX || (cmd & CMD_UPDATE)) return 0;
    io->write(io->context, QUP_SE6_CMD, cmd | CMD_UPDATE);
    if (!wait_command(io, CMD_UPDATE)) return 0;
    cfg = io->read(io->context, QUP_SE6_CFG);
    return cfg != UINT32_MAX && (cfg & CFG_RATE) == rate && unforce(c, io);
}

static int take(struct piano_qup_clocks *c, const struct piano_qup_io *io, enum piano_qup_reg r)
{
    int result = io->vote(io->context, piano_qup_vote_bits[r], 1);
    if (result == GCC_VOTE_FAILED) return 0;
    c->held |= 1u << r;
    return result == GCC_VOTE_OK && wait_branch(io, r);
}

static int drop(struct piano_qup_clocks *c, const struct piano_qup_io *io, enum piano_qup_reg r)
{
    if (!(c->held & (1u << r))) return 1;
    if (!io->vote(io->context, piano_qup_vote_bits[r], 0)) return 0;
    c->held &= ~(1u << r);
    io->wait_us(io->context, 10); /* A remote vote may still keep this branch on. */
    return 1;
}

int piano_qup_stop_clocks(struct piano_qup_clocks *c, const struct piano_qup_io *io)
{
    if (!c || !valid(io)) return 0;
    c->ready = 0;
    if (!drop(c, io, QUP_SE6)) return 0;
    if (c->park_needed) {
        uint32_t branch = io->read(io->context, QUP_SE6);
        if (branch == UINT32_MAX) return 0;
        if (branch_on(branch)) {
            /* Another voter still needs this clock. Do not latch even an
             * equivalent divider on a running engine to park it.
             */
            if (!wait_command(io, CMD_UPDATE) || (io->read(io->context, QUP_SE6_CFG) & CFG_RATE) > 1)
                return 0;
        } else if (!set_cfg(c, io, 0)) {
            return 0;
        }
        c->park_needed = 0;
    }
    if (!unforce(c, io)) return 0;
    for (unsigned n = QUP_SE6; n; --n)
        if (!drop(c, io, (enum piano_qup_reg)(n - 1))) return 0;
    return 1;
}

int piano_qup_start_clocks(struct piano_qup_clocks *c, const struct piano_qup_io *io)
{
    if (!c || !valid(io)) return 0;
    if (c->ready) return 1;
    if (!piano_qup_stop_clocks(c, io)) return 0;
    for (unsigned i = 0; i < QUP_SE6; ++i)
        if (!take(c, io, (enum piano_qup_reg)i)) goto fail;
    if (!wait_command(io, CMD_UPDATE)) goto fail;
    uint32_t cmd = io->read(io->context, QUP_SE6_CMD);
    uint32_t cfg = io->read(io->context, QUP_SE6_CFG);
    uint32_t branch = io->read(io->context, QUP_SE6);
    if (cmd == UINT32_MAX || cfg == UINT32_MAX || branch == UINT32_MAX) goto fail;
    /* Both HID 0 and 1 bypass division; don't retune a running crystal. */
    if (branch_on(branch) || (cmd & CMD_ROOT_EN)) {
        if ((cfg & CFG_RATE) > 1) goto fail;
    } else {
        c->park_needed = 1;
        if (!set_cfg(c, io, CFG_XO)) goto fail;
    }
    if (!take(c, io, QUP_SE6)) goto fail;
    c->ready = 1;
    return 1;
fail:
    (void)piano_qup_stop_clocks(c, io);
    return 0;
}
