/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include <storport.h>
#include "transfer.h"

/* The PnP/power layer supplies the assigned HCI resource, and UTRDs/UCDs in
 * Storport DMA common buffers. Each UCD starts with struct ufs_upius followed
 * immediately by its PRDT. Keep every buffer mapped until Stop succeeds.
 * Serialize StartIo, ISR, Stop and Complete with Storport's interrupt lock.
 */
enum { UFS_SDB_MAX_SLOTS = 32 };
typedef struct {
    struct ufs_upius *ucd;
    STOR_PHYSICAL_ADDRESS dma;
    PVOID srb;
    PUCHAR tail_destination;
} UFS_IO_SLOT;

typedef struct {
    PVOID extension;
    PUCHAR registers;
    struct ufs_utrd *list;
    STOR_PHYSICAL_ADDRESS list_dma;
    ULONGLONG dma_limit;  /* Minimum of the HCI and platform DMA masks. */
    UFS_IO_SLOT slot[UFS_SDB_MAX_SLOTS];
    ULONG slot_count, active, faults, enabled_luns;
    ULONG sleeping_luns, flushing_luns;
    USHORT prds_per_command;
    BOOLEAN running, owns_list, managing, device_ready, resetting;
} UFS_IO_QUEUE;

/* Map the bus-relative ACCESS_RANGE with its reported bus interface.
 * Select the HCI resource in HwFindAdapter; clocks must already be active.
 * Storport owns the uncached extension. Unmap the HCI once Stop succeeds.
 */
BOOLEAN UfsIoPrepare(UFS_IO_QUEUE *queue, PVOID extension, PPORT_CONFIGURATION_INFORMATION config,
                     const ACCESS_RANGE *hci, UCHAR platform_dma_bits, USHORT prds_per_command);
VOID UfsIoRelease(UFS_IO_QUEUE *queue);
BOOLEAN UfsIoStart(UFS_IO_QUEUE *queue);
/* Wire as HwBuildIo (<= DISPATCH_LEVEL); configure SrbExtensionSize as set
 * by UfsIoPrepare. Odd SCSI allocation lengths use a private final DWORD,
 * never DMA extra bytes into the caller's buffer.
 */
BOOLEAN UfsIoBuildRequest(PVOID extension, PSCSI_REQUEST_BLOCK srb);
BOOLEAN UfsIoStartRequest(UFS_IO_QUEUE *queue, PVOID srb);
/* An individual LU D3 flushes and blocks only that LU; other LUs keep using
 * the powered device. Adapter D3 has its own device-wide sequence. */
BOOLEAN UfsIoPowerUnit(UFS_IO_QUEUE *queue, PVOID srb, STOR_DEVICE_POWER_STATE state);
ULONG UfsIoInterrupt(UFS_IO_QUEUE *queue);
/* Call Stop and release DMA only after every outstanding request completed.
 * After a controller fault the platform error handler must stop the engine
 * and prove DMA quiescence before aborting SRBs or freeing their mappings.
 */
BOOLEAN UfsIoStop(UFS_IO_QUEUE *queue);
/* An actual HCI reset at interrupt synchronization. Complete even failed
 * SRBs only once HCE and both lists and doorbells are observed stopped.
 * An unacknowledged reset retains every SRB and mapping for a later retry.
 */
BOOLEAN UfsIoReset(UFS_IO_QUEUE *queue);
