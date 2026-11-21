/* SPDX-License-Identifier: BSD-2-Clause */
#include "platform_storport.h"

enum { HOST_BYTES = 0x3000, PHY_BYTES = 0x2000 };

static uint32_t read_register(void *context, enum ufs_platform_window window, unsigned reg)
{
    UFS_PLATFORM_DEVICE *d = context;
    PUCHAR base = window == UFS_HOST ? d->queue->registers : d->phy_registers;
    return StorPortReadRegisterUlong(d->extension, (PULONG)(base + reg));
}

static void write_register(void *context, enum ufs_platform_window window, unsigned reg, uint32_t value)
{
    UFS_PLATFORM_DEVICE *d = context;
    PUCHAR base = window == UFS_HOST ? d->queue->registers : d->phy_registers;
    StorPortWriteRegisterUlong(d->extension, (PULONG)(base + reg), value);
}

static int memory_resource(const ACCESS_RANGE *r, ULONG size)
{
    ULONGLONG start = (ULONGLONG)r->RangeStart.QuadPart;
    return r->RangeInMemory && r->RangeLength >= size && !(start & 3) &&
        r->RangeLength - 1 <= UINT64_MAX - start;
}

enum ufs_platform_error UfsPlatformPrepare(UFS_PLATFORM_DEVICE *d, UFS_IO_QUEUE *q, PVOID extension,
                                          PPORT_CONFIGURATION_INFORMATION config,
                                          const struct ufs_platform_power *power, UCHAR dma_bits,
                                          USHORT prds_per_command, ULONGLONG now_us)
{
    if (!d || !q || !extension || !config || !power || !power->set_bus_power ||
        !power->set_lane_clocks || !config->AccessRanges || config->NumberOfAccessRanges < 2)
        return UFS_PLATFORM_RESOURCES;
    if (d->extension || q->extension || q->registers) return UFS_PLATFORM_IN_USE;
    const ACCESS_RANGE *host = &(*config->AccessRanges)[0], *phy = &(*config->AccessRanges)[1];
    if (!memory_resource(host, HOST_BYTES) || !memory_resource(phy, PHY_BYTES))
        return UFS_PLATFORM_RESOURCES;
    ULONGLONG h = (ULONGLONG)host->RangeStart.QuadPart, p = (ULONGLONG)phy->RangeStart.QuadPart;
    if (h <= p ? p - h < host->RangeLength : h - p < phy->RangeLength) return UFS_PLATFORM_RESOURCES;

    if (!power->set_bus_power(power->context, 1)) return UFS_PLATFORM_POWER;
    d->bus_vote = TRUE;
    d->queue = q;
    d->extension = extension;
    d->power = *power;
    d->phy_registers = StorPortGetDeviceBase(extension, config->AdapterInterfaceType, config->SystemIoBusNumber,
                                             phy->RangeStart, PHY_BYTES, FALSE);
    enum ufs_platform_error error = UFS_PLATFORM_RESOURCES;
    if (d->phy_registers && UfsIoPrepare(q, extension, config, host, dma_bits, prds_per_command)) {
        struct ufs_platform_io io = { .context = d, .read = read_register, .write = write_register };
        error = ufs_platform_begin(&d->phy, &io, power, now_us);
        if (error == UFS_PLATFORM_OK) return error;
    }
    return UfsPlatformRelease(d, now_us) ? error : UFS_PLATFORM_POWER;
}

BOOLEAN UfsPlatformRelease(UFS_PLATFORM_DEVICE *d, ULONGLONG now_us)
{
    if (!d) return FALSE;
    if (!d->extension) return TRUE;
    UFS_IO_QUEUE *q = d->queue;
    if (q->running || q->active || q->managing || !ufs_platform_stop(&d->phy, now_us)) return FALSE;
    if (d->bus_vote && !d->power.set_bus_power(d->power.context, 0)) return FALSE;
    UfsIoRelease(q);
    if (d->phy_registers) StorPortFreeDeviceBase(d->extension, d->phy_registers);
    RtlZeroMemory(d, sizeof(*d));
    return TRUE;
}
