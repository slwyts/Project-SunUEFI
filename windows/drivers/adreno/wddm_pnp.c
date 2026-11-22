/* SPDX-License-Identifier: MIT */
#include "wddm_pnp.h"

#define RENDER_PNP_TAG 'pGdA'

static NTSTATUS collect_resources(PCM_RESOURCE_LIST list, ADRENO_RENDER_RESOURCES *out)
{
    /* WDM supplies one full descriptor for a PnP device. Only use the five
     * GPU ranges in the order defined by its firmware, never adjacent DPU,
     * GPUCC or SMMU registers. */
    static const ULONG sizes[AdrenoApertureCount] = {0x40000, 0x24000, 0x10000, 0x10000, 0x2000};
    ULONGLONG starts[AdrenoApertureCount], ends[AdrenoApertureCount];
    unsigned memory = 0, irqs = 0;
    if (!list || list->Count != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    PCM_PARTIAL_RESOURCE_LIST resources = &list->List[0].PartialResourceList;
    for (ULONG i = 0; i < resources->Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &resources->PartialDescriptors[i];
        if (r->Type == CmResourceTypeMemory || r->Type == CmResourceTypeMemoryLarge) {
            if (memory == AdrenoApertureCount)
                continue;
            ULONGLONG base;
            ULONGLONG size = RtlCmDecodeMemIoResource(r, &base);
            if (size != sizes[memory] || (base & (PAGE_SIZE - 1)) ||
                base > (ULONGLONG)MAXLONGLONG - (size - 1) ||
                (r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            ULONGLONG end = base + size - 1;
            for (unsigned j = 0; j < memory; ++j)
                if (base <= ends[j] && starts[j] <= end)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
            starts[memory] = base;
            ends[memory] = end;
            out->memory[memory++] = *r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if (irqs == AdrenoIrqCount)
                continue;
            if (r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            out->irq[irqs++] = *r;
        }
    }
    return memory == AdrenoApertureCount && irqs == AdrenoIrqCount ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

void AdrenoInitializeRenderDevice(ADRENO_RENDER_DEVICE *device, PDRIVER_OBJECT driver,
                                  PDEVICE_OBJECT pdo, const ADRENO_GMU_PLATFORM *platform,
                                  ADRENO_RENDER_POWER_UP power_up,
                                  ADRENO_RENDER_POWER_DOWN power_down,
                                  PDXGKDDI_QUERYADAPTERINFO query_engine)
{
    PAGED_CODE();
    RtlZeroMemory(device, sizeof(*device));
    AdrenoInitializeGpuClock(&device->clock);
    KeInitializeMutex(&device->power_lock, 0);
    device->pdo = pdo;
    device->driver = driver;
    device->platform = platform;
    device->power_up = power_up;
    device->power_down = power_down;
    device->query_engine = query_engine;
}

static NTSTATUS power_down(ADRENO_RENDER_DEVICE *device)
{
    AdrenoSetGpuClockPowered(&device->clock, FALSE);
    device->powered = FALSE;
    if (device->engine_acquired) {
        NTSTATUS status = AdrenoHoldGx(device);
        if (status != STATUS_SUCCESS)
            return status;
        status = device->power_down(device);
        if (status != STATUS_SUCCESS)
            return status;
        device->engine_acquired = FALSE;
    }
    return AdrenoStopGmu(device);
}

static NTSTATUS power_up(ADRENO_RENDER_DEVICE *device)
{
    if (device->powered) return STATUS_SUCCESS;
    NTSTATUS status;
    if (device->engine_acquired || device->gmu_state.rails) {
        status = power_down(device);
        if (status != STATUS_SUCCESS) return status;
    }
    status = AdrenoStartGmu(device);
    if (status == STATUS_SUCCESS) {
        device->engine_acquired = TRUE;
        status = device->power_up(device);
    }
    if (status == STATUS_SUCCESS) {
        AdrenoReleaseGx(device);
        device->powered = TRUE;
        AdrenoSetGpuClockPowered(&device->clock, TRUE);
    }
    return status;
}

static NTSTATUS stop_device(ADRENO_RENDER_DEVICE *device)
{
    device->started = FALSE;
    if (device->dxgk) {
        NTSTATUS status = power_down(device);
        if (status != STATUS_SUCCESS)
            return status;
        AdrenoBindGpuClock(&device->clock, NULL);
        status = AdrenoUnmapRegisterWindow(device->dxgk, &device->rsc);
        if (status == STATUS_SUCCESS)
            status = AdrenoUnmapRegisterWindow(device->dxgk, &device->gmu);
        if (status == STATUS_SUCCESS)
            status = AdrenoUnmapRegisterWindow(device->dxgk, &device->gpu);
        if (status != STATUS_SUCCESS)
            return status;
        ExFreePoolWithTag(device->dxgk, RENDER_PNP_TAG);
        device->dxgk = NULL;
    }
    AdrenoFreeFirmware(&device->firmware);
    RtlZeroMemory(&device->resources, sizeof(device->resources));
    RtlZeroMemory(&device->luid, sizeof(device->luid));
    return STATUS_SUCCESS;
}

static NTSTATUS start_device(ADRENO_RENDER_DEVICE *device, PDXGK_START_INFO start,
                             PDXGKRNL_INTERFACE dxgk, PULONG sources, PULONG children)
{
    DXGK_DEVICE_INFO info = {0};
    ADRENO_RENDER_RESOURCES resources = {0};
    PAGED_CODE();
    if (!device || !start || !sources || !children || !dxgk ||
        dxgk->Size < RTL_SIZEOF_THROUGH_FIELD(DXGKRNL_INTERFACE, DxgkCbUnmapMemory) ||
        dxgk->Version < DXGKDDI_INTERFACE_VERSION_WIN8 || !dxgk->DeviceHandle ||
        !dxgk->DxgkCbGetDeviceInformation || !dxgk->DxgkCbMapMemory || !dxgk->DxgkCbUnmapMemory ||
        !device->driver || !device->pdo || !device->power_up || !device->power_down || !device->query_engine)
        return STATUS_INVALID_PARAMETER;
    *sources = *children = 0;
    if (device->dxgk)
        return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = dxgk->DxgkCbGetDeviceInformation(dxgk->DeviceHandle, &info);
    if (status != STATUS_SUCCESS)
        return status;
    if (info.PhysicalDeviceObject != device->pdo)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    status = collect_resources(info.TranslatedResourceList, &resources);
    if (status != STATUS_SUCCESS)
        return status;
    /* Copy only the table actually provided. Embedding the current kit's
     * definition in the context would give it a different layout in a
     * rendering backend compiled for an older WDDM interface. */
    device->dxgk = ExAllocatePool2(POOL_FLAG_NON_PAGED, dxgk->Size, RENDER_PNP_TAG);
    if (!device->dxgk)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(device->dxgk, dxgk, dxgk->Size);
    device->resources = resources;
    device->luid = start->AdapterLuid;
    status = AdrenoReadFirmware(device->driver, &device->firmware);
    if (status == STATUS_SUCCESS)
        status = AdrenoMapRegisterWindow(dxgk, &device->resources.memory[AdrenoGpu], &device->gpu);
    if (status == STATUS_SUCCESS)
        status = AdrenoMapRegisterWindow(dxgk, &device->resources.memory[AdrenoGmu], &device->gmu);
    if (status == STATUS_SUCCESS)
        status = AdrenoMapRegisterWindow(dxgk, &device->resources.memory[AdrenoRsc], &device->rsc);
    if (status == STATUS_SUCCESS) {
        AdrenoBindGpuClock(&device->clock, &device->gmu);
        status = power_up(device);
    }
    if (status == STATUS_SUCCESS) {
        device->started = TRUE;
        return STATUS_SUCCESS;
    }
    NTSTATUS cleanup = stop_device(device);
    return cleanup == STATUS_SUCCESS ? status : cleanup;
}

_Use_decl_annotations_
NTSTATUS AdrenoDdiStartRenderDevice(PVOID context, PDXGK_START_INFO start, PDXGKRNL_INTERFACE dxgk,
                                    PULONG sources, PULONG children)
{
    ADRENO_RENDER_DEVICE *device = context;
    PAGED_CODE();
    if (!device) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&device->power_lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = start_device(device, start, dxgk, sources, children);
    KeReleaseMutex(&device->power_lock, FALSE);
    return s;
}

_Use_decl_annotations_
NTSTATUS AdrenoDdiStopRenderDevice(PVOID context)
{
    ADRENO_RENDER_DEVICE *device = context;
    PAGED_CODE();
    if (!device) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&device->power_lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = stop_device(device);
    KeReleaseMutex(&device->power_lock, FALSE);
    return s;
}

_Use_decl_annotations_
NTSTATUS AdrenoDdiSetRenderPowerState(PVOID context, ULONG uid, DEVICE_POWER_STATE state, POWER_ACTION action)
{
    ADRENO_RENDER_DEVICE *device = context;
    UNREFERENCED_PARAMETER(action);
    PAGED_CODE();
    if (!device || state < PowerDeviceD0 || state > PowerDeviceD3)
        return STATUS_INVALID_PARAMETER;
    /* A removed display child may still receive a pending power message.
     * This render-only adapter has never owned its display or power. */
    if (uid != DISPLAY_ADAPTER_HW_ID)
        return STATUS_SUCCESS;
    KeWaitForSingleObject(&device->power_lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = STATUS_DEVICE_NOT_READY;
    if (device->started) {
        if (state != PowerDeviceD0)
            s = power_down(device);
        else {
            s = power_up(device);
            if (s != STATUS_SUCCESS) {
                NTSTATUS cleanup = power_down(device);
                if (cleanup != STATUS_SUCCESS) s = cleanup;
            }
        }
    }
    KeReleaseMutex(&device->power_lock, FALSE);
    return s;
}

static DXGKDDI_QUERY_CHILD_RELATIONS render_children;
static DXGKDDI_QUERY_CHILD_STATUS render_child_status;
static DXGKDDI_QUERY_DEVICE_DESCRIPTOR render_child_descriptor;

_Use_decl_annotations_
NTSTATUS APIENTRY AdrenoDdiQueryRenderAdapterInfo(HANDLE context, const DXGKARG_QUERYADAPTERINFO *query)
{
    static const WCHAR architecture[] = L"Adreno 830";
    ADRENO_RENDER_DEVICE *device = context;
    if (!device || !query || !query->pOutputData)
        return STATUS_INVALID_PARAMETER;
    if (query->Type != DXGKQAITYPE_GPUVERSION)
        return device->query_engine && device->query_engine != AdrenoDdiQueryRenderAdapterInfo ?
               device->query_engine(context, query) : STATUS_DEVICE_NOT_READY;
    if (query->InputDataSize && (query->InputDataSize != sizeof(UINT) || !query->pInputData ||
                                *(const UINT *)query->pInputData))
        return STATUS_INVALID_PARAMETER;
    if (query->OutputDataSize < sizeof(DXGK_GPUVERSION))
        return STATUS_BUFFER_TOO_SMALL;
    DXGK_GPUVERSION *version = query->pOutputData;
    _Static_assert(sizeof(architecture) <= sizeof(version->GpuArchitecture), "DXGK GPU architecture name");
    RtlZeroMemory(version, sizeof(*version));
    RtlCopyMemory(version->GpuArchitecture, architecture, sizeof(architecture));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS render_children(PVOID context, PDXGK_CHILD_DESCRIPTOR children, ULONG bytes)
{
    UNREFERENCED_PARAMETER(context);
    if (!children || bytes < sizeof(*children))
        return STATUS_BUFFER_TOO_SMALL;
    /* Dxgkrnl allocated one zero terminator in addition to the zero outputs. */
    RtlZeroMemory(children, sizeof(*children));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS render_child_status(PVOID context, PDXGK_CHILD_STATUS child, BOOLEAN non_destructive)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(non_destructive);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
static NTSTATUS render_child_descriptor(PVOID context, ULONG uid, PDXGK_DEVICE_DESCRIPTOR descriptor)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(descriptor);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

NTSTATUS AdrenoInitializeRenderDriver(PDRIVER_OBJECT driver, PUNICODE_STRING registry,
                                      DRIVER_INITIALIZATION_DATA *ddi)
{
    PAGED_CODE();
    if (!driver || !registry || !ddi || ddi->Version < DXGKDDI_INTERFACE_VERSION_WDDM2_0 ||
        !ddi->DxgkDdiAddDevice || !ddi->DxgkDdiRemoveDevice ||
        !ddi->DxgkDdiQueryAdapterInfo || !ddi->DxgkDdiInterruptRoutine || !ddi->DxgkDdiDpcRoutine ||
        (!ddi->DxgkDdiSubmitCommand && !ddi->DxgkDdiSubmitCommandVirtual))
        return STATUS_INVALID_PARAMETER;
    ddi->DxgkDdiStartDevice = AdrenoDdiStartRenderDevice;
    ddi->DxgkDdiStopDevice = AdrenoDdiStopRenderDevice;
    ddi->DxgkDdiSetPowerState = AdrenoDdiSetRenderPowerState;
    ddi->DxgkDdiQueryAdapterInfo = AdrenoDdiQueryRenderAdapterInfo;
    ddi->DxgkDdiQueryChildRelations = render_children;
    ddi->DxgkDdiQueryChildStatus = render_child_status;
    ddi->DxgkDdiQueryDeviceDescriptor = render_child_descriptor;
    ddi->DxgkDdiCalibrateGpuClock = AdrenoDdiCalibrateGpuClock;
    return DxgkInitialize(driver, registry, ddi);
}
