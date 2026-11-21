/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_qup_wdm.h"

static int read_vote(void *context, uint32_t *value)
{
    PIANO_QUP_GCC *g = context;
    *value = READ_REGISTER_ULONG(g->vote_register);
    return 1;
}

static void write_vote(void *context, uint32_t value)
{
    PIANO_QUP_GCC *g = context;
    WRITE_REGISTER_ULONG(g->vote_register, value);
}

static PULONG assigned(const CM_PARTIAL_RESOURCE_DESCRIPTOR *r, PVOID mapping, uint32_t address)
{
    ULONGLONG start = r->u.Memory.Start.QuadPart;
    if (address < start || r->u.Memory.Length < sizeof(ULONG) ||
        address - start > r->u.Memory.Length - sizeof(ULONG)) return NULL;
    return (PULONG)((PUCHAR)mapping + (SIZE_T)(address - start));
}

NTSTATUS PianoQupPrepareGcc(PIANO_QUP_GCC *g, const CM_PARTIAL_RESOURCE_DESCRIPTOR *r, PVOID mapping)
{
    if (!g || !r || !mapping || g->vote_register || r->Type != CmResourceTypeMemory ||
        r->ShareDisposition == CmResourceShareShared ||
        (r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)) ||
        (r->u.Memory.Start.QuadPart & 3) || ((ULONG_PTR)mapping & 3)) return STATUS_INVALID_PARAMETER;
    /* Borrow the GCC driver's own translated mapping. Mapping a second
     * hardware resource from the I2C device would conflict with GCC's PnP
     * ownership and QCI2C could mistake that range for the serial engine.
     */
    PULONG vote = assigned(r, mapping, PIANO_GCC_QUP_VOTE_ADDRESS);
    if (!vote) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (unsigned i = 0; i < QUP_CLOCK_REGS; ++i)
        if (!assigned(r, mapping, piano_qup_clock_address[i])) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(g, sizeof(*g));
    for (unsigned i = 0; i < QUP_CLOCK_REGS; ++i)
        g->registers[i] = assigned(r, mapping, piano_qup_clock_address[i]);
    KeInitializeSpinLock(&g->lock);
    g->vote_register = vote;
    g->votes.context = g;
    g->votes.read = read_vote;
    g->votes.write = write_vote;
    return STATUS_SUCCESS;
}

NTSTATUS PianoQupReleaseGcc(PIANO_QUP_GCC *g)
{
    if (!g) return STATUS_INVALID_PARAMETER;
    if (!g->vote_register) return STATUS_SUCCESS;
    if (g->power.fabric_held || g->power.pads_held || g->power.clocks.held ||
        g->power.clocks.park_needed || g->power.clocks.forced) return STATUS_DEVICE_BUSY;
    for (unsigned i = 0; i < RTL_NUMBER_OF(g->votes.users); ++i)
        if (g->votes.users[i]) return STATUS_DEVICE_BUSY;
    RtlZeroMemory(g, sizeof(*g));
    return STATUS_SUCCESS;
}

int PianoGccBranchVote(PIANO_QUP_GCC *g, unsigned bit, int acquire)
{
    KIRQL old;
    if (!g || !g->vote_register) return 0;
    KeAcquireSpinLock(&g->lock, &old);
    int result = gcc_vote_set_locked(&g->votes, bit, acquire);
    KeReleaseSpinLock(&g->lock, old);
    return result;
}

static int vote_clock(void *context, unsigned bit, int acquire)
{
    return PianoGccBranchVote(context, bit, acquire);
}

static uint32_t read_clock(void *context, enum piano_qup_reg reg)
{
    PIANO_QUP_GCC *g = context;
    return READ_REGISTER_ULONG(g->registers[reg]);
}

static void write_clock(void *context, enum piano_qup_reg reg, uint32_t value)
{
    PIANO_QUP_GCC *g = context;
    WRITE_REGISTER_ULONG(g->registers[reg], value);
}

static void wait_us(void *context, unsigned us)
{
    UNREFERENCED_PARAMETER(context);
    KeStallExecutionProcessor(us);
}

NTSTATUS PianoQupPower(PIANO_QUP_GCC *g, const struct piano_qup_board_io *board, int enable)
{
    if (!g || !g->vote_register || !board) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    const struct piano_qup_io io = { g, read_clock, write_clock, wait_us, vote_clock };
    return piano_qup_set_power(&g->power, &io, board, enable) ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}
