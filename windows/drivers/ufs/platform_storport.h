/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "platform.h"
#include "storport_io.h"

typedef struct {
    struct ufs_platform phy;
    struct ufs_platform_power power;
    UFS_IO_QUEUE *queue;
    PVOID extension;
    PUCHAR phy_registers;
    BOOLEAN bus_vote;
    volatile LONG *media_off;
    PVOID worker;
    volatile LONG worker_active;
} UFS_PLATFORM_DEVICE;

/* Normal PnP discovery at PASSIVE_LEVEL. The first two ACPI memory ranges
 * are the HCI and PHY, respectively; Storport translates each separately.
 * The DMA width comes from the board's IORT. Do not use this path from a
 * crash dump or while resuming an adapter at interrupt level.
 */
_IRQL_requires_(PASSIVE_LEVEL)
enum ufs_platform_error UfsPlatformPrepare(UFS_PLATFORM_DEVICE *device, UFS_IO_QUEUE *queue, PVOID extension,
                                          PPORT_CONFIGURATION_INFORMATION config,
                                          const struct ufs_platform_power *power, UCHAR dma_bits,
                                          USHORT prds_per_command, ULONGLONG now_us);
/* Poll device->phy at PASSIVE_LEVEL; only start UfsLinkBegin once READY.
 * After media and HCI shutdown, or a failed Prepare, call Release; retry a
 * failed release before disposing of the device extension or its resources.
 */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPlatformRelease(UFS_PLATFORM_DEVICE *device, ULONGLONG now_us);
