/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_clock_storport.h"

static uint32_t read_clock(void *ctx, enum piano_ufs_clock_reg reg)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    return StorPortReadRegisterUlong(c->adapter, c->reg[reg]);
}

static void write_clock(void *ctx, enum piano_ufs_clock_reg reg, uint32_t value)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    StorPortWriteRegisterUlong(c->adapter, c->reg[reg], value);
}

static void wait_us(void *ctx, unsigned usec)
{
    UNREFERENCED_PARAMETER(ctx);
    while (usec) {
        ULONG step = usec > 50 ? 50 : usec;
        StorPortStallExecution(step);
        usec -= step;
    }
}

static void unmap_clock(PIANO_UFS_CLOCK_DEVICE *c)
{
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i)
        if (c->reg[i]) StorPortFreeDeviceBase(c->adapter, c->reg[i]);
    RtlZeroMemory(c, sizeof(*c));
}

ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *c, UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION cfg,
                          void *ctx, int (*supply)(void *, int))
{
    if (!c || !a || c->adapter || !supply || !cfg || !cfg->AccessRanges ||
        cfg->NumberOfAccessRanges < 2 + PIANO_UFS_CLOCK_REGS) return SP_RETURN_BAD_CONFIG;
    struct piano_ufs_clock_io io = { .context = c, .read = read_clock, .write = write_clock, .wait_us = wait_us };
    struct ufs_platform_power power;
    if (!piano_ufs_clock_init(&c->clocks, &io, ctx, supply, &power)) return SP_RETURN_BAD_CONFIG;
    c->adapter = a;
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i) {
        const ACCESS_RANGE *found = NULL;
        for (ULONG j = 2; j < cfg->NumberOfAccessRanges; ++j) {
            const ACCESS_RANGE *r = &(*cfg->AccessRanges)[j];
            if (r->RangeInMemory && r->RangeLength == sizeof(ULONG) &&
                r->RangeStart.QuadPart == piano_ufs_gcc_address[i]) {
                if (found) goto failed;
                found = r;
            }
        }
        if (!found) goto failed;
        c->reg[i] = StorPortGetDeviceBase(a, cfg->AdapterInterfaceType, cfg->SystemIoBusNumber,
                                         found->RangeStart, sizeof(ULONG), FALSE);
        if (!c->reg[i]) goto failed;
    }
    ULONG result = UfsMiniportFindPiano(a, cfg, &power);
    if (result != SP_RETURN_FOUND && !a->platform.extension) unmap_clock(c);
    return result;
failed:
    unmap_clock(c);
    return SP_RETURN_BAD_CONFIG;
}

BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *c, ULONGLONG now_us)
{
    if (!c || !c->adapter) return FALSE;
    if (!UfsPlatformRelease(&c->adapter->platform, now_us) || c->clocks.bus_held) return FALSE;
    unmap_clock(c);
    return TRUE;
}
