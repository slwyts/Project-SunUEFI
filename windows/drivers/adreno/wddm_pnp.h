/* SPDX-License-Identifier: MIT */
#pragma once
#include "gmu_resources.h"
#include "gmu_start.h"
#include "wddm_clock.h"

/* Order of each resource type in Piano's GPU0 _CRS. The OS translates the
 * bus addresses and interrupt vectors; GSI numbers aren't Windows vectors. */
enum adreno_aperture { AdrenoGpu, AdrenoGmu, AdrenoGmuAo, AdrenoRsc, AdrenoCx, AdrenoApertureCount };
enum adreno_irq { AdrenoGpuIrq, AdrenoCxIrq, AdrenoHfiIrq, AdrenoGmuIrq, AdrenoIrqCount };

typedef struct {
    CM_PARTIAL_RESOURCE_DESCRIPTOR memory[AdrenoApertureCount];
    CM_PARTIAL_RESOURCE_DESCRIPTOR irq[AdrenoIrqCount];
} ADRENO_RENDER_RESOURCES;

typedef struct ADRENO_RENDER_DEVICE ADRENO_RENDER_DEVICE;

/* Start/restore the command processors only after GMU startup. Stop saves
 * renderer state needed across D3, drains submissions and disables its
 * interrupts before the common path can put the GMU to sleep. */
typedef NTSTATUS (*ADRENO_RENDER_POWER_UP)(ADRENO_RENDER_DEVICE *device);
typedef NTSTATUS (*ADRENO_RENDER_POWER_DOWN)(ADRENO_RENDER_DEVICE *device);

/* Embed as the first member of the renderer's MiniportDeviceContext. */
struct ADRENO_RENDER_DEVICE {
    ADRENO_WDDM_ADAPTER clock;
    KMUTEX power_lock;
    PDRIVER_OBJECT driver;
    PDEVICE_OBJECT pdo;
    ADRENO_RENDER_POWER_UP power_up;
    ADRENO_RENDER_POWER_DOWN power_down;
    PDXGKDDI_QUERYADAPTERINFO query_engine;
    DXGKRNL_INTERFACE *dxgk;
    LUID luid;
    ADRENO_RENDER_RESOURCES resources;
    ADRENO_FIRMWARE firmware;
    ADRENO_REGISTER_WINDOW gpu, gmu, rsc;
    const ADRENO_GMU_PLATFORM *platform;
    ADRENO_GMU_DEVICE gmu_state;
    BOOLEAN engine_acquired, powered, started;
};

_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoInitializeRenderDevice(ADRENO_RENDER_DEVICE *device, PDRIVER_OBJECT driver,
                                 PDEVICE_OBJECT pdo, const ADRENO_GMU_PLATFORM *platform,
                                 ADRENO_RENDER_POWER_UP power_up,
                                 ADRENO_RENDER_POWER_DOWN power_down,
                                 PDXGKDDI_QUERYADAPTERINFO query_engine);

DXGKDDI_START_DEVICE AdrenoDdiStartRenderDevice;
DXGKDDI_STOP_DEVICE AdrenoDdiStopRenderDevice;
DXGKDDI_SET_POWER_STATE AdrenoDdiSetRenderPowerState;
DXGKDDI_QUERYADAPTERINFO AdrenoDdiQueryRenderAdapterInfo;

/* The chip renderer supplies the rendering, memory, power, interrupt and
 * object DDIs, including AddDevice and RemoveDevice. Install the shared PnP
 * entry points in its table before passing it to Dxgkrnl. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoInitializeRenderDriver(PDRIVER_OBJECT driver, PUNICODE_STRING registry,
                                     DRIVER_INITIALIZATION_DATA *ddi);
