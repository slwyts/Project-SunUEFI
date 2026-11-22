/* SPDX-License-Identifier: MIT */
#include "fixed_pll.h"
#include "registers.h"

#define LOCKED (1u << 31)
enum { PLL_MODE = 0, OP = 1, L = 4, ALPHA = 5, USER = 6, APSS_VOTE = 0x52020 / 4 };
/* Fixed rate entries in Piano's ClockDxe, driven by a 19.2 MHz parent. */
static const struct {
    unsigned word, vote, l, alpha;
} fixed[] = {{0, 1u, 31, 0x4000}, {0x4000 / 4, 1u << 4, 41, 0xfaaa}};

void Sm8750InitializeGcc(SM8750_GCC_PLLS *p, PKSPIN_LOCK vote_lock)
{
    RtlZeroMemory(p, sizeof(*p));
    KeInitializeMutex(&p->lock, 0);
    p->vote_lock = vote_lock;
}

BOOLEAN Sm8750GccResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR r, ULONGLONG *start, ULONGLONG *end)
{
    if (!r || (r->Type != CmResourceTypeMemory && r->Type != CmResourceTypeMemoryLarge) ||
        r->ShareDisposition != CmResourceShareDeviceExclusive ||
        (r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY))) return FALSE;
    ULONGLONG size = RtlCmDecodeMemIoResource(r, start);
    if (size < SM8750_GCC_REQUIRED_BYTES || size > MAXLONGLONG || (*start & (sizeof(ULONG) - 1)) ||
        *start > (ULONGLONG)MAXLONGLONG - size) return FALSE;
    *end = *start + size;
    return TRUE;
}

NTSTATUS Sm8750PrepareGccPlls(SM8750_GCC_PLLS *p, PCM_PARTIAL_RESOURCE_DESCRIPTOR gcc, PVOID mapping)
{
    ULONGLONG start, end;
    PAGED_CODE();
    if (!mapping || ((ULONG_PTR)mapping & 3) || !Sm8750GccResource(gcc, &start, &end))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = p->removed ? STATUS_DEVICE_REMOVED : p->base ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (s == STATUS_SUCCESS) {
        p->base = mapping;
        p->start = start;
        p->end = end;
    }
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

static NTSTATUS apply(SM8750_GCC_PLLS *p, unsigned i, BOOLEAN on)
{
    volatile ULONG *r = p->base + fixed[i].word;
    ULONG u = READ_REGISTER_ULONG(r + USER), mask = fixed[i].vote;
    if (u == MAXULONG)
        return STATUS_DEVICE_NOT_CONNECTED;
    if (!!(u & (1u << 25)) != p->pll[i].voted)
        return STATUS_DEVICE_BUSY;
    NTSTATUS s;
    if (p->pll[i].voted) {
        KIRQL irql;
        KeAcquireSpinLock(p->vote_lock, &irql);
        s = update(p->base + APSS_VOTE, mask, on ? mask : 0);
        KeReleaseSpinLock(p->vote_lock, irql);
        if (s == STATUS_SUCCESS && on)
            s = wait_bits(r + PLL_MODE, LOCKED, LOCKED, 500);
    } else if (on) {
        s = update(r + PLL_MODE, 4, 4);
        if (s == STATUS_SUCCESS)
            s = update(r + OP, 1, 1);
        if (s == STATUS_SUCCESS)
            s = wait_bits(r + PLL_MODE, LOCKED, LOCKED, 500);
        if (s == STATUS_SUCCESS)
            s = update(r + PLL_MODE, 1, 1);
    } else {
        s = update(r + PLL_MODE, 1, 0);
        if (s == STATUS_SUCCESS)
            s = update(r + OP, 1, 0);
    }
    return s;
}

NTSTATUS Sm8750SetGccPll(PVOID ctx, enum sm8750_gcc_pll id, BOOLEAN *held, BOOLEAN on)
{
    SM8750_GCC_PLLS *p = ctx;
    PAGED_CODE();
    if (!p || !held || (unsigned)id >= Sm8750GccPllCount)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    unsigned i = id == Sm8750Gpll4 ? 1u : 0u;
    SM8750_GCC_PLL_STATE *g = &p->pll[i];
    NTSTATUS s = STATUS_SUCCESS;
    if (!p->online) {
        if (p->removed && !on && *held && g->users) {
            --g->users;
            *held = FALSE;
        } else if (on || *held) {
            s = p->removed ? STATUS_DEVICE_REMOVED : STATUS_DEVICE_NOT_READY;
        }
        goto out;
    }
    volatile ULONG *r = p->base + fixed[i].word;
    if (on || (*held && !g->initialized)) {
        ULONG u = READ_REGISTER_ULONG(r + USER), l = READ_REGISTER_ULONG(r + L),
              a = READ_REGISTER_ULONG(r + ALPHA);
        if (u == MAXULONG || l == MAXULONG || a == MAXULONG) {
            s = STATUS_DEVICE_NOT_CONNECTED;
            goto out;
        }
        if ((l & 0xffff) != fixed[i].l || (a & 0xffff) != fixed[i].alpha ||
            (id == Sm8750Gpll0Even && ((u >> 10) & 15) != 1)) {
            s = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto out;
        }
        BOOLEAN voted = !!(u & (1u << 25));
        if (!g->initialized || (!g->boot && !g->users && g->voted != voted)) {
            ULONG v = READ_REGISTER_ULONG(voted ? p->base + APSS_VOTE : r + PLL_MODE);
            if (v == MAXULONG) {
                s = STATUS_DEVICE_NOT_CONNECTED;
                goto out;
            }
            if (!voted) {
                ULONG op = READ_REGISTER_ULONG(r + OP);
                if (op == MAXULONG) {
                    s = STATUS_DEVICE_NOT_CONNECTED;
                    goto out;
                }
                v &= op;
            }
            /* A vote retained across D3 may be ours, not firmware's. */
            g->boot = !!(v & (voted ? fixed[i].vote : 1)) && (!g->restore || g->boot);
            g->voted = voted;
            g->initialized = TRUE;
        } else if (g->voted != voted) {
            s = STATUS_DEVICE_BUSY;
            goto out;
        }
    }
    if (on) {
        if (!*held) {
            if (g->users == MAXULONG) {
                s = STATUS_INTEGER_OVERFLOW;
                goto out;
            }
            ++g->users;
            *held = TRUE;
        }
        s = apply(p, i, TRUE);
        if (s == STATUS_SUCCESS)
            g->restore = FALSE;
    } else if (*held) {
        if (!g->users) {
            s = STATUS_INVALID_DEVICE_STATE;
            goto out;
        }
        /* The first returning child may release its clock while a sibling
         * still owns a reference. Restore that sibling even on this path. */
        if (g->restore && g->users > 1)
            s = apply(p, i, TRUE);
        else if (g->users == 1 && !g->boot)
            s = apply(p, i, FALSE);
        if (s == STATUS_SUCCESS) {
            --g->users;
            *held = FALSE;
            g->restore = FALSE;
        }
    }
out:
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

NTSTATUS Sm8750ReleaseGccPllBootReference(SM8750_GCC_PLLS *p, enum sm8750_gcc_pll id)
{
    PAGED_CODE();
    if (!p || (unsigned)id >= Sm8750GccPllCount)
        return STATUS_INVALID_PARAMETER;
    unsigned i = id == Sm8750Gpll4 ? 1u : 0u;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    SM8750_GCC_PLL_STATE *g = &p->pll[i];
    NTSTATUS s = p->online && g->initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    if (s == STATUS_SUCCESS && g->boot && !g->users)
        s = apply(p, i, FALSE);
    if (s == STATUS_SUCCESS)
        g->boot = FALSE;
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

NTSTATUS Sm8750GccPower(SM8750_GCC_PLLS *p, BOOLEAN online, BOOLEAN removed)
{
    PAGED_CODE();
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    if (removed) p->removed = TRUE;
    NTSTATUS s = !online ? STATUS_SUCCESS : p->removed ? STATUS_DEVICE_REMOVED :
                 p->base ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    p->online = online && s == STATUS_SUCCESS;
    if (!p->online) InterlockedExchangePointer(&p->retained_ufs, NULL);
    KeReleaseSpinLock(p->vote_lock, irql);
    if (!p->online) {
        for (unsigned i = 0; i < RTL_NUMBER_OF(p->pll); ++i) {
            p->pll[i].restore = p->pll[i].users != 0;
            p->pll[i].initialized = FALSE;
        }
    }
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

PVOID Sm8750DetachGcc(SM8750_GCC_PLLS *p)
{
    PAGED_CODE();
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    PVOID mapping = (PVOID)p->base;
    p->online = FALSE;
    InterlockedExchangePointer(&p->retained_ufs, NULL);
    p->base = NULL;
    p->start = p->end = 0;
    KeReleaseSpinLock(p->vote_lock, irql);
    KeReleaseMutex(&p->lock, FALSE);
    return mapping;
}

NTSTATUS Sm8750OpenUfsClocks(SM8750_GCC_PLLS *p, PVOID adapter)
{
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    NTSTATUS s = !adapter ? STATUS_INVALID_PARAMETER : p->removed ? STATUS_DEVICE_REMOVED :
                 !p->online ? STATUS_DEVICE_NOT_READY : p->ufs ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (s == STATUS_SUCCESS) {
        p->ufs = adapter;
        InterlockedExchangePointer(&p->retained_ufs, adapter);
    }
    KeReleaseSpinLock(p->vote_lock, irql);
    return s;
}

BOOLEAN Sm8750CloseUfsClocks(SM8750_GCC_PLLS *p, PVOID adapter)
{
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    BOOLEAN held = adapter && adapter == p->ufs;
    if (held) {
        InterlockedExchangePointer(&p->retained_ufs, NULL);
        p->ufs = NULL;
    }
    KeReleaseSpinLock(p->vote_lock, irql);
    return held;
}

BOOLEAN Sm8750UfsPowerRetained(SM8750_GCC_PLLS *p, PVOID adapter)
{
    /* Half-duplex Storport asks from StartIo at DIRQL. This snapshot does
     * not wait, call WDF, or touch registers. */
    return adapter && InterlockedCompareExchangePointer(&p->retained_ufs, NULL, NULL) == adapter;
}

/* The UFS driver owns these clocks, reset and two domains. The status
 * words are read-only. Other subsystems and APSS vote words stay private. */
static BOOLEAN ufs_register(ULONG offset, BOOLEAN write)
{
    switch (offset) {
    case 0x77008: case 0x9e004:
        return !write;
    case 0x77000: case 0x77004: case 0x9e000: /* reset and domains */
    case 0x77030: case 0x77068: case 0x770d8: case 0x770ec: case 0x7702c: case 0x77058: /* lanes */
    case 0x77034: case 0x77038: case 0x770a4: case 0x770a8: case 0x7708c: case 0x77090:
    case 0x770c0: case 0x770c4: /* root clocks */
    case 0x77018: case 0x770f0: case 0x77028: case 0x7706c: case 0x7707c: case 0x770bc: /* branches */
        return TRUE;
    default:
        return FALSE;
    }
}

ULONG Sm8750ReadUfsClock(SM8750_GCC_PLLS *p, PVOID adapter, ULONG offset)
{
    KIRQL irql;
    ULONG value = MAXULONG;
    KeAcquireSpinLock(p->vote_lock, &irql);
    if (adapter && adapter == p->ufs && p->online && ufs_register(offset, FALSE))
        value = READ_REGISTER_ULONG(p->base + offset / 4);
    KeReleaseSpinLock(p->vote_lock, irql);
    return value;
}

void Sm8750WriteUfsClock(SM8750_GCC_PLLS *p, PVOID adapter, ULONG offset, ULONG value)
{
    KIRQL irql;
    KeAcquireSpinLock(p->vote_lock, &irql);
    if (adapter && adapter == p->ufs && p->online && ufs_register(offset, TRUE))
        WRITE_REGISTER_ULONG(p->base + offset / 4, value);
    KeReleaseSpinLock(p->vote_lock, irql);
}
