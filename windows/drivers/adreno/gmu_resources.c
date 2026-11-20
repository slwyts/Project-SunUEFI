/* SPDX-License-Identifier: MIT */
#include "gmu_resources.h"

/* The standardized A830 GMU register region starts at register word 0x1b000.
 * ITCM (0x1b400) and DTCM (0x1c400) are adjacent 16 KiB apertures. Use the
 * CPU-translated bus address supplied by PnP, not a hard-coded physical base.
 */
#define A830_GMU_ITCM_OFFSET 0x1000u

NTSTATUS AdrenoMapGmuTcm(const DXGKRNL_INTERFACE *dxgk,
                         PCM_PARTIAL_RESOURCE_DESCRIPTOR memory, ADRENO_GMU_TCM *tcm)
{
    ULONGLONG base;
    PAGED_CODE();
    if (tcm->mapping)
        return STATUS_ALREADY_INITIALIZED;
    if (!memory || (memory->Type != CmResourceTypeMemory && memory->Type != CmResourceTypeMemoryLarge) ||
        (memory->Flags & CM_RESOURCE_MEMORY_READ_ONLY))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    ULONGLONG size = RtlCmDecodeMemIoResource(memory, &base);
    ULONG length = 2 * ADRENO_GMU_TCM_BYTES;
    if ((base & (sizeof(ULONG) - 1)) || size < A830_GMU_ITCM_OFFSET + length ||
        base > MAXLONGLONG - A830_GMU_ITCM_OFFSET - length)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    PHYSICAL_ADDRESS start;
    start.QuadPart = (LONGLONG)base + A830_GMU_ITCM_OFFSET;
    void *mapped = NULL;
    NTSTATUS status = dxgk->DxgkCbMapMemory(dxgk->DeviceHandle, start, length,
                                             FALSE, FALSE, MmNonCached, &mapped);
    if (NT_SUCCESS(status))
        tcm->mapping = mapped;
    return status;
}

NTSTATUS AdrenoUnmapGmuTcm(const DXGKRNL_INTERFACE *dxgk, ADRENO_GMU_TCM *tcm)
{
    PAGED_CODE();
    if (!tcm->mapping)
        return STATUS_SUCCESS;
    NTSTATUS status = dxgk->DxgkCbUnmapMemory(dxgk->DeviceHandle, tcm->mapping);
    if (NT_SUCCESS(status))
        tcm->mapping = NULL;
    return status;
}
