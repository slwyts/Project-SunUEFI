/* SPDX-License-Identifier: MIT */
#include "fixed_pll.h"
#include "registers.h"

#define LOCKED (1u << 31)
enum { PLL_MODE = 0, OP = 1, L = 4, ALPHA = 5, USER = 6, APSS_VOTE = 0x52020 / 4 };
/* Fixed rate entries in Piano's ClockDxe, driven by a 19.2 MHz parent. */
static const struct {
    unsigned word, vote, l, alpha;
} fixed[] = {{0, 1u, 31, 0x4000}, {0x4000 / 4, 1u << 4, 41, 0xfaaa}};

NTSTATUS Sm8750PrepareGccPlls(SM8750_GCC_PLLS *p, PCM_PARTIAL_RESOURCE_DESCRIPTOR gcc, PVOID mapping,
                              PKSPIN_LOCK vote_lock, const INTERFACE *device)
{
    ULONGLONG start, end;
    PAGED_CODE();
    if (!p || p->base || !mapping || ((ULONG_PTR)mapping & 3) || !vote_lock || !device ||
        !device->InterfaceReference || !device->InterfaceDereference || !window(gcc, &start, 0x52024, &end) ||
        start != 0x100000)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(p, sizeof(*p));
    KeInitializeMutex(&p->lock, 0);
    p->base = mapping;
    p->start = start;
    p->end = end;
    p->vote_lock = vote_lock;
    p->lifetime = *device;
    return STATUS_SUCCESS;
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
    if (!p || !p->base || !held || (unsigned)id >= Sm8750GccPllCount)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    unsigned i = id == Sm8750Gpll4 ? 1u : 0u;
    volatile ULONG *r = p->base + fixed[i].word;
    SM8750_GCC_PLL_STATE *g = &p->pll[i];
    NTSTATUS s = STATUS_SUCCESS;
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
            g->boot = !!(v & (voted ? fixed[i].vote : 1));
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
    if (!p || !p->base || (unsigned)id >= Sm8750GccPllCount)
        return STATUS_INVALID_PARAMETER;
    unsigned i = id == Sm8750Gpll4 ? 1u : 0u;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    SM8750_GCC_PLL_STATE *g = &p->pll[i];
    NTSTATUS s = g->initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    if (s == STATUS_SUCCESS && g->boot && !g->users)
        s = apply(p, i, FALSE);
    if (s == STATUS_SUCCESS)
        g->boot = FALSE;
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}

void Sm8750GccPllPowerLost(SM8750_GCC_PLLS *p)
{
    PAGED_CODE();
    if (!p || !p->base)
        return;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    for (unsigned i = 0; i < RTL_NUMBER_OF(p->pll); ++i) {
        p->pll[i].restore = p->pll[i].users != 0;
        p->pll[i].initialized = p->pll[i].boot = FALSE;
    }
    KeReleaseMutex(&p->lock, FALSE);
}

static VOID reference(PVOID ctx)
{
    SM8750_GCC_PLLS *p = ctx;
    p->lifetime.InterfaceReference(p->lifetime.Context);
}
static VOID dereference(PVOID ctx)
{
    SM8750_GCC_PLLS *p = ctx;
    p->lifetime.InterfaceDereference(p->lifetime.Context);
}
static NTSTATUS open_ufs(PVOID ctx, SM8750_UFS_GCC_WINDOWS *w)
{
    SM8750_GCC_PLLS *p = ctx;
    PAGED_CODE();
    if (!w || p->end - p->start < 0x9e008)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = STATUS_DEVICE_BUSY;
    if (!p->ufs) {
        w->clocks = (PULONG)(p->base + 0x77000 / 4);
        w->phy_domain = (PULONG)(p->base + 0x9e000 / 4);
        p->ufs = w;
        s = STATUS_SUCCESS;
    }
    KeReleaseMutex(&p->lock, FALSE);
    return s;
}
static VOID close_ufs(PVOID ctx, SM8750_UFS_GCC_WINDOWS *w)
{
    SM8750_GCC_PLLS *p = ctx;
    PAGED_CODE();
    KeWaitForSingleObject(&p->lock, Executive, KernelMode, FALSE, NULL);
    if (w && w == p->ufs) {
        p->ufs = NULL;
        w->clocks = w->phy_domain = NULL;
    }
    KeReleaseMutex(&p->lock, FALSE);
}
NTSTATUS Sm8750GetGccInterface(SM8750_GCC_PLLS *p, SM8750_GCC_INTERFACE *out)
{
    PAGED_CODE();
    if (!p || !p->base || !out)
        return STATUS_INVALID_PARAMETER;
    *out =
        (SM8750_GCC_INTERFACE){.Header = {sizeof(*out), SM8750_GCC_INTERFACE_VERSION, p, reference, dereference},
                               .Set = Sm8750SetGccPll,
                               .OpenUfs = open_ufs,
                               .CloseUfs = close_ufs};
    return STATUS_SUCCESS;
}
