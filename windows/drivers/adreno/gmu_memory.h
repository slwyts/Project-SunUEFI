/* SPDX-License-Identifier: MIT */
#pragma once
#include "firmware.h"

typedef struct {
    PMDL mdl;
    void *cpu;
    ULONGLONG iova;
    ULONG bytes;
    BOOLEAN mapped;
} ADRENO_GMU_BUFFER;

typedef struct {
    DMA_IOMMU_INTERFACE_EX iommu;
    PIOMMU_DMA_DEVICE requester;
    PIOMMU_DMA_DOMAIN domain;
    BOOLEAN attached;
    ADRENO_GMU_BUFFER cache, hfi, log;
    ADRENO_GMU_BUFFER *reserved;
    SIZE_T reserved_count;
} ADRENO_GMU_MEMORY;

/* PnP start: CM3 must be held in reset. pdo and input_mapping select only
 * the GMU's ACPI/IORT input, never a render stream owned by Dxgkrnl. This is
 * an IORT input mapping ID, not the SMMU's downstream StreamID or a bank.
 * Zero-initialize memory. Serialize start/stop with the device power lock.
 * On error call Close again if cleanup itself failed. Do not release the PDO
 * while the requester exists. No mappings are exposed to user mode.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoGmuMemoryOpen(ADRENO_GMU_MEMORY *memory, PDEVICE_OBJECT pdo,
                            ULONG input_mapping, const ADRENO_IMAGE *firmware);

/* After all CM3 DMA is stopped. If HAL cannot detach or retire a mapping,
 * its backing pages remain locked and the same object can retry cleanup.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoGmuMemoryClose(ADRENO_GMU_MEMORY *memory);
