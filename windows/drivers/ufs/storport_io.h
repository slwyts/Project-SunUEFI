/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include <storport.h>
#include "transfer.h"

/* The PnP/power layer supplies a translated HCI resource, and UTRDs/UCDs in
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
    ULONG slot_count, active, faults;
    USHORT prds_per_command;
    BOOLEAN running, managing, device_ready;
} UFS_IO_QUEUE;

/* Maps the ACPI memory range using the reported bus interface, not PCI BARs.
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
ULONG UfsIoInterrupt(UFS_IO_QUEUE *queue);
/* Call Stop and release DMA only after every outstanding request completed.
 * After a controller fault the platform error handler must stop the engine
 * and prove DMA quiescence before aborting SRBs or freeing their mappings.
 */
BOOLEAN UfsIoStop(UFS_IO_QUEUE *queue);
