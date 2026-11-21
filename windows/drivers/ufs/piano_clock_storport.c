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

static uint32_t read_domain(void *ctx, enum piano_ufs_domain d, unsigned offset)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    return StorPortReadRegisterUlong(c->adapter, c->domain[d] + offset / sizeof(ULONG));
}

static void write_domain(void *ctx, enum piano_ufs_domain d, uint32_t value)
{
    PIANO_UFS_CLOCK_DEVICE *c = ctx;
    StorPortWriteRegisterUlong(c->adapter, c->domain[d], value);
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

static void unmap_controls(PIANO_UFS_CLOCK_DEVICE *c)
{
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i)
        if (c->domain[i]) StorPortFreeDeviceBase(c->adapter, c->domain[i]);
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i)
        if (c->reg[i]) StorPortFreeDeviceBase(c->adapter, c->reg[i]);
    RtlZeroMemory(c, sizeof(*c));
}

static PULONG map_resource(UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION cfg, uint32_t address, ULONG bytes)
{
    const ACCESS_RANGE *found = NULL;
    for (ULONG j = 2; j < cfg->NumberOfAccessRanges; ++j) {
        const ACCESS_RANGE *r = &(*cfg->AccessRanges)[j];
        if (r->RangeInMemory && r->RangeLength == bytes && r->RangeStart.QuadPart == address) {
            if (found) return NULL;
            found = r;
        }
    }
    return found ? StorPortGetDeviceBase(a, cfg->AdapterInterfaceType, cfg->SystemIoBusNumber,
                                         found->RangeStart, bytes, FALSE) : NULL;
}

ULONG UfsPianoFindAdapter(PIANO_UFS_CLOCK_DEVICE *c, UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION cfg,
                          const struct piano_ufs_bus_inputs *inputs)
{
    if (!c || !a || c->adapter || !inputs || !inputs->rails_and_paths || !inputs->base_clocks ||
        !cfg || !cfg->AccessRanges || cfg->NumberOfAccessRanges < 2 + PIANO_UFS_CLOCK_REGS + PIANO_UFS_DOMAINS)
        return SP_RETURN_BAD_CONFIG;
    struct piano_ufs_clock_io io = { .context = c, .read = read_clock, .write = write_clock, .wait_us = wait_us };
    struct ufs_platform_power power;
    c->power.io = (struct piano_ufs_domain_io){ c, read_domain, write_domain, wait_us };
    c->power.inputs = *inputs;
    if (!piano_ufs_clock_init(&c->clocks, &io, &c->power, piano_ufs_set_bus_power, &power))
        return SP_RETURN_BAD_CONFIG;
    c->adapter = a;
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i) {
        c->reg[i] = map_resource(a, cfg, piano_ufs_gcc_address[i], sizeof(ULONG));
        if (!c->reg[i]) goto failed;
    }
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i) {
        c->domain[i] = map_resource(a, cfg, piano_ufs_domain_address[i], 2 * sizeof(ULONG));
        if (!c->domain[i]) goto failed;
    }
    ULONG result = UfsMiniportFindPiano(a, cfg, &power);
    if (result == SP_RETURN_FOUND)
        piano_ufs_claim_domains(&c->power);
    else if (!a->platform.extension && piano_ufs_set_bus_power(&c->power, 0))
        unmap_controls(c);
    return result;
failed:
    unmap_controls(c);
    return SP_RETURN_BAD_CONFIG;
}

BOOLEAN UfsPianoRelease(PIANO_UFS_CLOCK_DEVICE *c, ULONGLONG now_us)
{
    if (!c || !c->adapter) return FALSE;
    if (!UfsPlatformRelease(&c->adapter->platform, now_us) || c->clocks.bus_held ||
        !piano_ufs_set_bus_power(&c->power, 0)) return FALSE;
    unmap_controls(c);
    return TRUE;
}
