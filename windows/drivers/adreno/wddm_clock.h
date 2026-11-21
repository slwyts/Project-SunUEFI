/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>

/* MiniportDeviceContext. The PnP/power path serializes mapping, clock
 * transitions, and unmapping. Calibration can run concurrently at DISPATCH_LEVEL. */
typedef struct {
    KSPIN_LOCK clock_lock;
    volatile ULONG *gmu_counter;
    BOOLEAN counter_powered;
} ADRENO_WDDM_ADAPTER;

_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoInitializeGpuClock(ADRENO_WDDM_ADAPTER *adapter);

/* Pass the same translated GMU register window used by AdrenoMapGmuTcm. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoMapGpuClock(ADRENO_WDDM_ADAPTER *adapter, const DXGKRNL_INTERFACE *dxgk,
                          PCM_PARTIAL_RESOURCE_DESCRIPTOR gmu_registers);

/* Enable only while the GMU CX counter is accessible. Disable before
 * dropping CX power or clocks, including runtime idle in D0. */
_IRQL_requires_max_(DISPATCH_LEVEL)
void AdrenoSetGpuClockPowered(ADRENO_WDDM_ADAPTER *adapter, BOOLEAN powered);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoUnmapGpuClock(ADRENO_WDDM_ADAPTER *adapter, const DXGKRNL_INTERFACE *dxgk);

DXGKDDI_CALIBRATEGPUCLOCK AdrenoDdiCalibrateGpuClock;
