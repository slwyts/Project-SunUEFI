/* SPDX-License-Identifier: MIT */
#pragma once
#include "gpu_clocks.h"
#include "gmu_memory.h"
#include "hfi.h"
#include "hfi_bandwidth.h"

struct ADRENO_RENDER_DEVICE;

/* The platform driver owns GPUCC and arbitrates the shared rails, bus and
 * clocks with the SMMU and other clients. Enable requests are synchronous;
 * disable must also retire a partially applied request after an error.
 * SetRailsAndBus may succeed only after firmware relinquishes the GPU. It
 * holds CX/MX, XO and GPU interconnect for the factory clock configuration.
 * Tables and provider references remain valid throughout PnP start/stop. */
typedef struct {
    void *context;
    ADRENO_GPU_CLOCKS *clocks;
    NTSTATUS (*SetRailsAndBus)(void *context, BOOLEAN enable);
    /* Optional GX clock controller recovery, only if RSC left GX powered. */
    NTSTATUS (*PowerOffGx)(void *context);
    struct adreno_perf_config perf;
    struct adreno_bw_config bandwidth;
    ADRENO_HFI_NOTIFY notify;
} ADRENO_GMU_PLATFORM;

typedef struct {
    ADRENO_CLOCK_REFERENCE clock_ref;
    ADRENO_GMU_MEMORY memory;
    ADRENO_HFI hfi;
    BOOLEAN rails, wake_pending, rsc_awake, reset_pending, reset_released, hfi_running, slumber_requested;
    BOOLEAN rsc_sleeping, gbif_pending;
} ADRENO_GMU_DEVICE;

/* The renderer serializes these with CP submission and D0/D3. On start
 * failure call Stop; a timeout retiring DMA retains the pages, domain and
 * clocks so subsequent cleanup can retry. Stop is called after CP shutdown.
 * GMU success does not mean the SQE/AQE or a rendering context is running. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoStartGmu(struct ADRENO_RENDER_DEVICE *device);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoStopGmu(struct ADRENO_RENDER_DEVICE *device);
