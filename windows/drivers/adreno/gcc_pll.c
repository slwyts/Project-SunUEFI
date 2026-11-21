/* SPDX-License-Identifier: MIT */
#include "gcc_pll.h"
#include "clock_registers.h"

#define PLL_LOCK (1u << 31)
enum { mode = 0, op = 4 / 4, l_val = 0x10 / 4, alpha = 0x14 / 4, user = 0x18 / 4, votes = 0x52020 / 4 };

NTSTATUS Sm8750PrepareGccPll0(SM8750_GCC_PLL0 *p, PCM_PARTIAL_RESOURCE_DESCRIPTOR gcc,
                             void *mapping, PKSPIN_LOCK vote_lock)
{
    ULONGLONG start, end;
    PAGED_CODE();
    if (!p || p->base || !mapping || ((ULONG_PTR)mapping & 3) || !vote_lock || !window(gcc, &start, 0x71154, &end))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(p, sizeof(*p));
    KeInitializeMutex(&p->lock, 0);
    p->base = mapping;
    p->vote_lock = vote_lock;
    p->start = start;
    p->end = end;
    return STATUS_SUCCESS;
}

static NTSTATUS global_vote(SM8750_GCC_PLL0 *p, BOOLEAN on)
{
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    NTSTATUS s = update(p->base + votes, 1, on ? 1u : 0u);
    KeReleaseSpinLock(p->vote_lock, irql);
    return s;
}

static NTSTATUS apply(SM8750_GCC_PLL0 *p, BOOLEAN on)
{
    ULONG u = READ_REGISTER_ULONG(p->base + user);
    if (u == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if (!!(u & (1u << 25)) != p->voted) return STATUS_DEVICE_BUSY;
    NTSTATUS s;
    if (p->voted) {
        s = global_vote(p, on);
        /* Other masters may still keep this PLL locked when our vote ends. */
        if (s == STATUS_SUCCESS && on) s = wait_bits(p->base + mode, PLL_LOCK, PLL_LOCK, 500);
        return s;
    }
    if (on) {
        s = update(p->base + mode, 4, 4);
        if (s == STATUS_SUCCESS) s = update(p->base + op, 1, 1);
        if (s == STATUS_SUCCESS) s = wait_bits(p->base + mode, PLL_LOCK, PLL_LOCK, 500);
        if (s == STATUS_SUCCESS) s = update(p->base + mode, 1, 1);
    } else {
        s = update(p->base + mode, 1, 0);
        if (s == STATUS_SUCCESS) s = update(p->base + op, 1, 0);
    }
    return s;
}

NTSTATUS Sm8750SetGccPll0(SM8750_GCC_PLL0 *p, BOOLEAN *held, BOOLEAN on)
{
    PAGED_CODE();
    if (!p || !p->base || !held) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = STATUS_SUCCESS;
    if (on) {
        ULONG u = READ_REGISTER_ULONG(p->base + user);
        ULONG l = READ_REGISTER_ULONG(p->base + l_val), fraction = READ_REGISTER_ULONG(p->base + alpha);
        if (u == MAXULONG || l == MAXULONG || fraction == MAXULONG) { s = STATUS_DEVICE_NOT_CONNECTED; goto done; }
        /* Fixed GCC PLL0: 19.2 MHz * (31 + 1/4), not GPUCC's tunable PLL.
         * The hub requests 600 MHz from this parent. Do not retune it. */
        if ((l & 0xffff) != 31 || (fraction & 0xffff) != 0x4000) { s = STATUS_DEVICE_CONFIGURATION_ERROR; goto done; }
        BOOLEAN voted = !!(u & (1u << 25));
        if (!p->initialized || (p->voted != voted && !p->users && !p->boot)) {
            ULONG v = READ_REGISTER_ULONG(p->base + (voted ? votes : mode));
            if (v == MAXULONG) { s = STATUS_DEVICE_NOT_CONNECTED; goto done; }
            if (!voted) {
                ULONG run = READ_REGISTER_ULONG(p->base + op);
                if (run == MAXULONG) { s = STATUS_DEVICE_NOT_CONNECTED; goto done; }
                v &= run;
            }
            p->boot = !!(v & 1);
            p->voted = voted;
            p->initialized = TRUE;
        } else if (p->voted != voted) { s = STATUS_DEVICE_BUSY; goto done; }
        if (!*held) {
            if (p->users == MAXULONG) { s = STATUS_INTEGER_OVERFLOW; goto done; }
            ++p->users;
            *held = TRUE;
        }
        s = apply(p, TRUE);
    } else if (*held) {
        if (!p->users) { s = STATUS_INVALID_DEVICE_STATE; goto done; }
        if (p->users == 1 && !p->boot) s = apply(p, FALSE);
        if (s == STATUS_SUCCESS) { --p->users; *held = FALSE; }
    }
done:
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

NTSTATUS Sm8750ReleaseGccPll0BootReference(SM8750_GCC_PLL0 *p)
{
    PAGED_CODE();
    if (!p || !p->base) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = p->initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    if (p->boot && !p->users) s = apply(p, FALSE);
    if (s == STATUS_SUCCESS) p->boot = FALSE;
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}
