/* SPDX-License-Identifier: MIT */
#include "wddm_clock.h"

/* A8xx KGSL and Freedreno use the GMU CX always-on counter for host reads:
 * the CP register aperture may be fenced during an autonomous GX collapse.
 * Register word offsets in a6xx_gmu.xml are relative to the GPU origin. */
#define A830_GMU_REGISTER_BASE 0x1b000u
#define A830_GMU_ALWAYS_ON_COUNTER 0x1f840u
#define A830_COUNTER_HZ 19200000u

void AdrenoInitializeGpuClock(ADRENO_WDDM_ADAPTER *adapter)
{
    KeInitializeSpinLock(&adapter->clock_lock);
    adapter->gmu_counter = NULL;
    adapter->counter_powered = FALSE;
}

NTSTATUS AdrenoMapGpuClock(ADRENO_WDDM_ADAPTER *adapter, const DXGKRNL_INTERFACE *dxgk,
                          PCM_PARTIAL_RESOURCE_DESCRIPTOR gmu_registers)
{
    const ULONG offset = (A830_GMU_ALWAYS_ON_COUNTER - A830_GMU_REGISTER_BASE) * sizeof(ULONG);
    const ULONG bytes = 2 * sizeof(ULONG);
    ULONGLONG base;
    void *mapping;
    PHYSICAL_ADDRESS address;
    PAGED_CODE();

    if (adapter->gmu_counter)
        return STATUS_ALREADY_INITIALIZED;
    if (!gmu_registers || (gmu_registers->Type != CmResourceTypeMemory &&
                           gmu_registers->Type != CmResourceTypeMemoryLarge) ||
        (gmu_registers->Flags & CM_RESOURCE_MEMORY_WRITE_ONLY))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONGLONG size = RtlCmDecodeMemIoResource(gmu_registers, &base);
    if ((base & (sizeof(ULONG) - 1)) || size < offset + bytes ||
        base > (ULONGLONG)MAXLONGLONG - (offset + bytes - 1))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    address.QuadPart = (LONGLONG)(base + offset);
    NTSTATUS status = dxgk->DxgkCbMapMemory(dxgk->DeviceHandle, address, bytes,
                                             FALSE, FALSE, MmNonCached, &mapping);
    if (NT_SUCCESS(status))
        adapter->gmu_counter = mapping;
    return status;
}

void AdrenoSetGpuClockPowered(ADRENO_WDDM_ADAPTER *adapter, BOOLEAN powered)
{
    KIRQL old;
    KeAcquireSpinLock(&adapter->clock_lock, &old);
    adapter->counter_powered = powered && adapter->gmu_counter != NULL;
    KeReleaseSpinLock(&adapter->clock_lock, old);
}

NTSTATUS AdrenoUnmapGpuClock(ADRENO_WDDM_ADAPTER *adapter, const DXGKRNL_INTERFACE *dxgk)
{
    PAGED_CODE();
    AdrenoSetGpuClockPowered(adapter, FALSE);
    if (!adapter->gmu_counter)
        return STATUS_SUCCESS;
    NTSTATUS status = dxgk->DxgkCbUnmapMemory(dxgk->DeviceHandle, (void *)adapter->gmu_counter);
    if (NT_SUCCESS(status))
        adapter->gmu_counter = NULL;
    return status;
}

_Use_decl_annotations_
NTSTATUS APIENTRY AdrenoDdiCalibrateGpuClock(HANDLE context, UINT32 node, UINT32 engine,
                                             DXGKARG_CALIBRATEGPUCLOCK *result)
{
    ADRENO_WDDM_ADAPTER *adapter = context;
    KIRQL old;
    if (!adapter || !result || node || engine)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&adapter->clock_lock, &old);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (adapter->counter_powered) {
        status = STATUS_RETRY;
        /* Same split-counter read as the Ace 5 Pro miniport. Bracket the low
         * word with QPC so a rollover retry doesn't widen the sample interval. */
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            ULONG high = READ_REGISTER_ULONG(adapter->gmu_counter + 1);
            ULONGLONG before = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
            ULONG low = READ_REGISTER_ULONG(adapter->gmu_counter);
            ULONGLONG after = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
            if (high != READ_REGISTER_ULONG(adapter->gmu_counter + 1) || after < before)
                continue;
            DXGKARG_CALIBRATEGPUCLOCK sample = {0};
            sample.GpuFrequency = A830_COUNTER_HZ;
            sample.GpuClockCounter = ((ULONGLONG)high << 32) | low;
            sample.CpuClockCounter = before + (after - before) / 2;
            *result = sample;
            status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&adapter->clock_lock, old);
    return status;
}
