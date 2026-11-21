/* SPDX-License-Identifier: MIT */
#include "wddm_clock.h"

/* A8xx KGSL and Freedreno use the GMU CX always-on counter for host reads:
 * the CP register aperture may be fenced during an autonomous GX collapse.
 * Register word offsets in a6xx_gmu.xml are relative to the GPU origin. */
#define A830_GMU_ALWAYS_ON_COUNTER 0x1f840u
#define A830_COUNTER_HZ 19200000u

void AdrenoInitializeGpuClock(ADRENO_WDDM_ADAPTER *adapter)
{
    KeInitializeSpinLock(&adapter->clock_lock);
    adapter->gmu_counter = NULL;
    adapter->counter_powered = FALSE;
}

void AdrenoBindGpuClock(ADRENO_WDDM_ADAPTER *adapter, const ADRENO_REGISTER_WINDOW *registers)
{
    const ULONG offset = A830_GMU_ALWAYS_ON_COUNTER - ADRENO_GMU_BASE_WORD;
    KIRQL old;
    PAGED_CODE();
    KeAcquireSpinLock(&adapter->clock_lock, &old);
    adapter->counter_powered = FALSE;
    adapter->gmu_counter = registers && registers->words && registers->bytes / sizeof(ULONG) >= offset + 2 ?
                           registers->words + offset : NULL;
    KeReleaseSpinLock(&adapter->clock_lock, old);
}

void AdrenoSetGpuClockPowered(ADRENO_WDDM_ADAPTER *adapter, BOOLEAN powered)
{
    KIRQL old;
    KeAcquireSpinLock(&adapter->clock_lock, &old);
    adapter->counter_powered = powered && adapter->gmu_counter != NULL;
    KeReleaseSpinLock(&adapter->clock_lock, old);
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
