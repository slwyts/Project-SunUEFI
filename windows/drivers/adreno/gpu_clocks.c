/* SPDX-License-Identifier: MIT */
#include "gpu_clocks.h"
#include "../gcc/registers.h"

/* MiCode gpucc-sun, gcc-sun and KGSL gen8_0_1. Offsets are relative to
 * their provider's translated base. GCC source 600 MHz / 3 supplies the
 * 200 MHz hub; GPUCC's Taycan ELU even output supplies the 500 MHz GMU. */
enum clock_id {
    gcc_pll, gpll, hub_root, pll, gmu_root, gmu, cxo, axi, memnoc, ahb, hub, smmu, clock_count
};
enum { pll_mode = 0x00, pll_op = 0x04, pll_l = 0x10, pll_frac = 0x14, pll_user = 0x18,
       pll_user_hi = 0x1c, pll_cfg = 0x20, pll_cfg_hi = 0x24, pll_cfg_hi1 = 0x28,
       hub_rcg = 0x93ec, gmu_rcg = 0x9318 };
#define BIT(n) (1u << (n))
#define CLK_OFF BIT(31)
#define NOC_ON(v) (!((v) & CLK_OFF) || (((v) >> 28) & 7) == 2)
#define RCG_MASK (BIT(20) | (3u << 12) | (7u << 8) | 0x1f)
#define RCG(parent, hid) (BIT(20) | ((parent) << 8) | (hid))
_Static_assert(clock_count == RTL_NUMBER_OF(((ADRENO_GPU_CLOCKS *)0)->users), "GPU clock table");

static volatile ULONG *reg(ADRENO_GPU_CLOCKS *c, ULONG byte)
{
    return c->gpucc + byte / sizeof(ULONG);
}

/* The two shared roots park on CXO before their parent vote is released.
 * Force the root only for this update, preserving another owner's force. */
static NTSTATUS set_root(ADRENO_GPU_CLOCKS *c, unsigned offset, ULONG cfg)
{
    volatile ULONG *cmd = reg(c, offset);
    NTSTATUS s = wait_bits(cmd, 1, 0, 500);
    if (s != STATUS_SUCCESS) return s;
    ULONG live = READ_REGISTER_ULONG(cmd);
    if (live == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    ULONG root = offset == hub_rcg ? BIT(hub_root) : BIT(gmu_root);
    if (!(live & 2)) {
        c->forced |= root;
        if ((s = update(cmd, 2, 2)) != STATUS_SUCCESS) return s;
    }
    s = wait_bits(cmd, CLK_OFF, 0, 500);
    if (s == STATUS_SUCCESS) s = update(cmd + 1, RCG_MASK, cfg);
    if (s == STATUS_SUCCESS) {
        live = READ_REGISTER_ULONG(cmd);
        if (live == MAXULONG) s = STATUS_DEVICE_NOT_CONNECTED;
        else {
            WRITE_REGISTER_ULONG(cmd, live | 1);
            s = wait_bits(cmd, 1, 0, 500);
        }
    }
    if (s == STATUS_SUCCESS) s = wait_bits(cmd + 1, RCG_MASK, cfg, 0);
    if (s == STATUS_SUCCESS && (c->forced & root)) {
        NTSTATUS clear = update(cmd, 2, 0);
        if (clear != STATUS_SUCCESS) return clear;
        c->forced &= ~root;
    }
    return s;
}

static NTSTATUS stop_pll(ADRENO_GPU_CLOCKS *c)
{
    NTSTATUS s = update(reg(c, pll_mode), 1, 0);
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_op), 1, 0);
    /* The SM8750 GPU PLL has DISABLE_TO_OFF. Its local outputs keep
     * their factory configuration while global OUTCTRL is disabled. */
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_mode), 4, 0);
    return s;
}

static NTSTATUS run_pll(ADRENO_GPU_CLOCKS *c)
{
    NTSTATUS s = update(reg(c, pll_mode), 4, 4);
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_op), 1, 1);
    if (s == STATUS_SUCCESS) s = wait_bits(reg(c, pll_mode), BIT(31), BIT(31), 500);
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_mode), 1, 1);
    return s;
}

static NTSTATUS start_pll(ADRENO_GPU_CLOCKS *c)
{
    ULONG mode = READ_REGISTER_ULONG(reg(c, pll_mode)), op = READ_REGISTER_ULONG(reg(c, pll_op));
    ULONG user = READ_REGISTER_ULONG(reg(c, pll_user)), l = READ_REGISTER_ULONG(reg(c, pll_l));
    ULONG alpha = READ_REGISTER_ULONG(reg(c, pll_frac));
    if (mode == MAXULONG || op == MAXULONG || user == MAXULONG || l == MAXULONG || alpha == MAXULONG)
        return STATUS_DEVICE_NOT_CONNECTED;
    /* Both floor and ceil representations are within one fractional LSB
     * of 1 GHz. The vendor's initial PLL configuration uses 0x1555. */
    BOOLEAN target = (l & 0xffff) == 0x34 && (alpha == 0x1555 || alpha == 0x1556) &&
                     (user & (0x3c00 | 3)) == (0x400 | 3);
    if ((mode & 1) && (op & 1) && target) {
        NTSTATUS s = wait_bits(reg(c, pll_mode), BIT(31), BIT(31), 500);
        if (s == STATUS_SUCCESS && !(user & BIT(25))) c->dirty |= BIT(pll);
        return s;
    }
    if (user & BIT(25)) return STATUS_DEVICE_BUSY; /* Owned by a hardware voter. */
    /* A warm handoff can leave the GMU root on a different PLL rate. Its
     * clock is independent of the live SMMU and hub. Move it to CXO before
     * touching its only PLL; the renderer has already relinquished DMA. */
    c->park_gmu = TRUE;
    NTSTATUS s = set_root(c, gmu_rcg, RCG(0, 1));
    if (s != STATUS_SUCCESS) return s;
    c->park_gmu = FALSE;
    c->dirty |= BIT(pll);
    s = stop_pll(c);
    if (s != STATUS_SUCCESS) return s;
    if (!(l & 0xffff)) {
        s = update(reg(c, pll_l), MAXULONG, 0x44440034);
        if (s == STATUS_SUCCESS) s = update(reg(c, pll_cfg), MAXULONG, 0x19660387);
        if (s == STATUS_SUCCESS) s = update(reg(c, pll_cfg_hi), MAXULONG, 0x098060a0);
        if (s == STATUS_SUCCESS) s = update(reg(c, pll_cfg_hi1), MAXULONG, 0xb416cb20);
        if (s == STATUS_SUCCESS) s = update(reg(c, pll_user_hi), MAXULONG, 2);
    } else {
        /* Firmware provided its calibration: keep both calibrated L fields. */
        s = update(reg(c, pll_l), 0xffff, 0x34);
    }
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_frac), 0xffff, 0x1555);
    if (s == STATUS_SUCCESS) s = update(reg(c, pll_user), 0x3c00 | 7, 0x400 | 7);
    if (s != STATUS_SUCCESS) return s;
    mode = READ_REGISTER_ULONG(reg(c, pll_mode));
    if (mode == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if (mode & BIT(8)) {
        s = run_pll(c);
        if (s == STATUS_SUCCESS) s = stop_pll(c);
        if (s != STATUS_SUCCESS) return s;
    }
    return run_pll(c);
}

static enum sm8750_gcc_gpu_clock gcc_clock(enum clock_id id)
{
    switch (id) {
    case gcc_pll: return Sm8750GpuGpll0;
    case gpll: return Sm8750GpuGpll0Main;
    case axi: return Sm8750GpuDdrAxi;
    case memnoc: return Sm8750GpuMemnoc;
    default: return Sm8750GpuClockCount;
    }
}

static NTSTATUS set_gcc(ADRENO_GPU_CLOCKS *c, enum sm8750_gcc_gpu_clock id, BOOLEAN on)
{
    if (!on && !c->gcc_held[id]) return STATUS_SUCCESS;
    if (!c->gcc.SetClock) return STATUS_DEVICE_NOT_READY;
    return c->gcc.SetClock(c->gcc.Header.Context, id, &c->gcc_held[id], on);
}

static NTSTATUS gate(ADRENO_GPU_CLOCKS *c, enum clock_id id, BOOLEAN on)
{
    volatile ULONG *addr;
    switch (id) {
    case gmu: addr = reg(c, 0x90d4); break;
    case cxo: addr = reg(c, 0x90e4); break;
    case ahb: addr = reg(c, 0x90bc); break;
    case hub: addr = reg(c, 0x90e8); break;
    case smmu: addr = reg(c, 0x7000); break;
    default: return STATUS_INVALID_PARAMETER;
    }
    /* These branch2_aon gates have no hardware disable operation. Their
     * parents still park on CXO or lose their respective software vote. */
    if (!on && (id == gmu || id == hub)) return STATUS_SUCCESS;
    NTSTATUS s = update(addr, 1, on ? 1u : 0u);
    if (s != STATUS_SUCCESS) return s;
    if (id == ahb || (!on && id != cxo)) {
        KeStallExecutionProcessor(10);
        return STATUS_SUCCESS;
    }
    for (unsigned i = 0; i <= 200; ++i) {
        ULONG v = READ_REGISTER_ULONG(addr);
        if (v == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
        if (on ? NOC_ON(v) : (v & CLK_OFF)) return STATUS_SUCCESS;
        if (i != 200) KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS enable_clock(ADRENO_GPU_CLOCKS *c, enum clock_id id)
{
    enum sm8750_gcc_gpu_clock source = gcc_clock(id);
    if (source != Sm8750GpuClockCount) return set_gcc(c, source, TRUE);
    if (id == pll) return start_pll(c);
    c->dirty |= BIT(id);
    if (id == hub_root) return set_root(c, hub_rcg, RCG(5, 5));
    if (id == gmu_root) return set_root(c, gmu_rcg, RCG(2, 1));
    return gate(c, id, TRUE);
}

static NTSTATUS disable_clock(ADRENO_GPU_CLOCKS *c, enum clock_id id)
{
    enum sm8750_gcc_gpu_clock source = gcc_clock(id);
    if (source != Sm8750GpuClockCount) return set_gcc(c, source, FALSE);
    if (id == pll && c->park_gmu) {
        NTSTATUS s = set_root(c, gmu_rcg, RCG(0, 1));
        if (s != STATUS_SUCCESS) return s;
        c->park_gmu = FALSE;
    }
    if (!(c->dirty & BIT(id))) return STATUS_SUCCESS;
    if (id == hub_root || id == gmu_root) return set_root(c, id == hub_root ? hub_rcg : gmu_rcg, RCG(0, 1));
    if (id == pll) return stop_pll(c);
    return gate(c, id, FALSE);
}

static ULONG client_clocks(enum adreno_clock_client client)
{
    const ULONG bus = BIT(gcc_pll) | BIT(gpll) | BIT(hub_root) | BIT(memnoc) | BIT(ahb);
    switch (client) {
    case AdrenoGmuClocks: return bus | BIT(pll) | BIT(gmu_root) | BIT(gmu) | BIT(cxo) | BIT(axi) | BIT(hub);
    case AdrenoRenderBusClocks: return bus;
    case AdrenoSmmuClocks: return BIT(smmu);
    default: return 0;
    }
}

static NTSTATUS keep_on(ADRENO_GPU_CLOCKS *c)
{
    /* Controller-wide CB/CXO_AON/GX_AHB_FF/RSCC_HUB/RSCC_XO/SLEEP. The
     * factory controller restores these after power loss, not per client. */
    static const ULONG offsets[] = { 0x93a4, 0x9008, 0x9064, 0x93a8, 0x9004, 0x90cc };
    for (unsigned i = 0; i < RTL_NUMBER_OF(offsets); ++i) {
        NTSTATUS s = update(reg(c, offsets[i]), 1, 1);
        if (s != STATUS_SUCCESS) return s;
    }
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoSetGpuClocks(ADRENO_GPU_CLOCKS *c, ADRENO_CLOCK_REFERENCE *r,
                           enum adreno_clock_client client, BOOLEAN on)
{
    PAGED_CODE();
    ULONG requested = client_clocks(client);
    if (!c || !c->gpucc || !r || (r->controller && r->controller != c) || !requested)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&c->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = STATUS_SUCCESS;
    if (on) {
        if (r->held & ~requested) { s = STATUS_INVALID_DEVICE_STATE; goto done; }
        if (c->partial & requested) { s = STATUS_DEVICE_BUSY; goto done; }
        if (r->held == requested) goto done;
        r->controller = c;
        s = AdrenoCxAcquire(&c->cx, &r->cx);
        if (s != STATUS_SUCCESS) goto done;
        s = keep_on(c);
        if (s != STATUS_SUCCESS) goto done;
        for (unsigned i = 0; i < clock_count; ++i) {
            if (!(requested & BIT(i)) || (r->held & BIT(i))) continue;
            if (c->users[i] == MAXULONG) { s = STATUS_INTEGER_OVERFLOW; break; }
            r->held |= BIT(i);
            if (c->users[i]++) continue;
            s = enable_clock(c, (enum clock_id)i);
            if (s != STATUS_SUCCESS) { c->partial |= BIT(i); break; }
        }
    } else {
        for (unsigned n = clock_count; n; --n) {
            unsigned i = n - 1;
            if (!(r->held & BIT(i))) continue;
            if (c->users[i] == 1) {
                s = disable_clock(c, (enum clock_id)i);
                if (s != STATUS_SUCCESS) { c->partial |= BIT(i); break; }
                c->partial &= ~BIT(i);
                c->dirty &= ~BIT(i);
            }
            --c->users[i];
            r->held &= ~BIT(i);
        }
        if (!r->held) {
            if (r->cx.domain) s = AdrenoCxRelease(&c->cx, &r->cx);
            if (s == STATUS_SUCCESS) r->controller = NULL;
        }
    }
done:
    KeReleaseMutex(&c->lock, FALSE);
    return s;
}

NTSTATUS AdrenoPrepareGpuClocks(ADRENO_GPU_CLOCKS *c, PCM_PARTIAL_RESOURCE_DESCRIPTOR gpucc)
{
    ULONGLONG cb;
    PAGED_CODE();
    if (!c || c->gpucc || !gpucc ||
        (gpucc->Type != CmResourceTypeMemory && gpucc->Type != CmResourceTypeMemoryLarge) ||
        gpucc->ShareDisposition != CmResourceShareDeviceExclusive ||
        (gpucc->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONGLONG bytes = RtlCmDecodeMemIoResource(gpucc, &cb);
    if (bytes < 0x9800 || bytes > MAXLONGLONG || cb > (ULONGLONG)MAXLONGLONG - bytes || (cb & 3))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    PHYSICAL_ADDRESS start;
    start.QuadPart = (LONGLONG)cb;
    void *mapping = MmMapIoSpaceEx(start, 0x9800, PAGE_READWRITE | PAGE_NOCACHE);
    if (!mapping) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(c, sizeof(*c));
    KeInitializeMutex(&c->lock, 0);
    NTSTATUS s = AdrenoCxInitialize(&c->cx, mapping, 0x9800);
    if (s != STATUS_SUCCESS) {
        MmUnmapIoSpace(mapping, 0x9800);
        return s;
    }
    c->gpucc = mapping;
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoReleaseGpuClocks(ADRENO_GPU_CLOCKS *c)
{
    PAGED_CODE();
    if (!c) return STATUS_INVALID_PARAMETER;
    if (!c->gpucc) return STATUS_SUCCESS;
    /* PnP has serialized removal against reference acquisition. No device
     * register access is needed after the parent rails have been removed. */
    for (unsigned i = 0; i < clock_count; ++i)
        if (c->users[i]) return STATUS_DEVICE_BUSY;
    if (c->cx.users) return STATUS_DEVICE_BUSY;
    MmUnmapIoSpace((void *)c->gpucc, 0x9800);
    c->gpucc = NULL;
    c->cx.control = c->cx.status = NULL;
    return STATUS_SUCCESS;
}

void AdrenoForgetGpuClocks(ADRENO_GPU_CLOCKS *c)
{
    PAGED_CODE();
    /* The caller serialized all clients. GCC takes responsibility for a
     * final release that cannot reach hardware until its next D0. */
    RtlZeroMemory(c->users, sizeof(c->users));
    c->cx.users = 0;
    if (c->cx.initialized) c->cx.resuming = TRUE;
    c->cx.initialized = FALSE;
    c->dirty = c->partial = c->forced = 0;
    c->park_gmu = FALSE;
    if (c->gcc.Abandon) c->gcc.Abandon(c->gcc.Header.Context, c->gcc_held);
}
