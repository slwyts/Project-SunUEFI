/* SPDX-License-Identifier: MIT */
#pragma once
#include "gmu_resources.h"
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

/* Start is called only after Windows has assigned this PDO's resources and
 * the corresponding firmware files and GMU register views are available.
 * Stop must tolerate a partial start and must quiesce the device before its
 * mappings can be released. Neither callback owns a display controller. */
typedef NTSTATUS (*ADRENO_RENDER_POWER_UP)(ADRENO_RENDER_DEVICE *device);
typedef NTSTATUS (*ADRENO_RENDER_POWER_DOWN)(ADRENO_RENDER_DEVICE *device);

/* Embed as the first member of the renderer's MiniportDeviceContext. */
struct ADRENO_RENDER_DEVICE {
    ADRENO_WDDM_ADAPTER clock;
    PDRIVER_OBJECT driver;
    PDEVICE_OBJECT pdo;
    ADRENO_RENDER_POWER_UP power_up;
    ADRENO_RENDER_POWER_DOWN power_down;
    PDXGKDDI_QUERYADAPTERINFO query_engine;
    DXGKRNL_INTERFACE *dxgk;
    LUID luid;
    ADRENO_RENDER_RESOURCES resources;
    ADRENO_FIRMWARE firmware;
    ADRENO_GMU_TCM tcm;
    BOOLEAN engine_acquired;
};

_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoInitializeRenderDevice(ADRENO_RENDER_DEVICE *device, PDRIVER_OBJECT driver,
                                 PDEVICE_OBJECT pdo, ADRENO_RENDER_POWER_UP power_up,
                                 ADRENO_RENDER_POWER_DOWN power_down,
                                 PDXGKDDI_QUERYADAPTERINFO query_engine);

DXGKDDI_START_DEVICE AdrenoDdiStartRenderDevice;
DXGKDDI_STOP_DEVICE AdrenoDdiStopRenderDevice;
DXGKDDI_QUERYADAPTERINFO AdrenoDdiQueryRenderAdapterInfo;

/* The chip renderer supplies the rendering, memory, power, interrupt and
 * object DDIs, including AddDevice and RemoveDevice. Install the shared PnP
 * entry points in its table before passing it to Dxgkrnl. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoInitializeRenderDriver(PDRIVER_OBJECT driver, PUNICODE_STRING registry,
                                     DRIVER_INITIALIZATION_DATA *ddi);
