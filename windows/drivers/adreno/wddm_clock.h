/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include "gmu_resources.h"

/* MiniportDeviceContext. The PnP/power path serializes mapping, clock
 * transitions, and unmapping. Calibration can run concurrently at DISPATCH_LEVEL. */
typedef struct {
    KSPIN_LOCK clock_lock;
    volatile ULONG *gmu_counter;
    BOOLEAN counter_powered;
} ADRENO_WDDM_ADAPTER;

_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoInitializeGpuClock(ADRENO_WDDM_ADAPTER *adapter);

/* The counter and HFI share the render device's one translated mapping. */
_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoBindGpuClock(ADRENO_WDDM_ADAPTER *adapter, const ADRENO_REGISTER_WINDOW *registers);

/* Enable only while the GMU CX counter is accessible. Disable before
 * dropping CX power or clocks, including runtime idle in D0. */
_IRQL_requires_max_(DISPATCH_LEVEL)
void AdrenoSetGpuClockPowered(ADRENO_WDDM_ADAPTER *adapter, BOOLEAN powered);

DXGKDDI_CALIBRATEGPUCLOCK AdrenoDdiCalibrateGpuClock;
