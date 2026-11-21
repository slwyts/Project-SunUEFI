/* SPDX-License-Identifier: MIT */
#include "gmu_resources.h"

NTSTATUS AdrenoMapRegisterWindow(const DXGKRNL_INTERFACE *dxgk,
                                PCM_PARTIAL_RESOURCE_DESCRIPTOR memory, ADRENO_REGISTER_WINDOW *registers)
{
    ULONGLONG base;
    PAGED_CODE();
    if (registers->words)
        return STATUS_ALREADY_INITIALIZED;
    if (!memory || (memory->Type != CmResourceTypeMemory && memory->Type != CmResourceTypeMemoryLarge) ||
        (memory->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    ULONGLONG size = RtlCmDecodeMemIoResource(memory, &base);
    if ((base & (PAGE_SIZE - 1)) || !size || size > MAXULONG || base > (ULONGLONG)MAXLONGLONG - (size-1))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    PHYSICAL_ADDRESS start;
    start.QuadPart = (LONGLONG)base;
    void *mapped = NULL;
    NTSTATUS status = dxgk->DxgkCbMapMemory(dxgk->DeviceHandle, start, (ULONG)size,
                                             FALSE, FALSE, MmNonCached, &mapped);
    if (status == STATUS_SUCCESS) {
        if (!mapped) return STATUS_DEVICE_CONFIGURATION_ERROR;
        registers->words = mapped;
        registers->bytes = (ULONG)size;
    }
    return status;
}

NTSTATUS AdrenoUnmapRegisterWindow(const DXGKRNL_INTERFACE *dxgk, ADRENO_REGISTER_WINDOW *registers)
{
    PAGED_CODE();
    if (!registers->words)
        return STATUS_SUCCESS;
    NTSTATUS status = dxgk->DxgkCbUnmapMemory(dxgk->DeviceHandle, (PVOID)registers->words);
    if (status == STATUS_SUCCESS)
        RtlZeroMemory(registers, sizeof(*registers));
    return status;
}
