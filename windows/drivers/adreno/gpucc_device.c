/* SPDX-License-Identifier: MIT */
#include <ntddk.h>
#include <wdf.h>
#include "gpu_clocks.h"
#include "../gcc/acpi.h"
#include "../gcc/query.h"

typedef struct {
    LIST_ENTRY link;
    PVOID client;
    enum adreno_clock_client type;
    ADRENO_CLOCK_REFERENCE clocks;
} GPUCC_REQUEST;

typedef struct {
    WDFDEVICE device;
    WDFWORKITEM gcc_remove_work;
    KMUTEX lock;
    ADRENO_GPU_CLOCKS clocks;
    LIST_ENTRY requests;
    SM8750_CLOCK_TARGET gcc_target;
    ULONGLONG gcc_generation;
    volatile LONG references;
    BOOLEAN online, removed, stale, idle_held;
} GPUCC_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GPUCC_DEVICE, gpucc_device)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD add_device;
static EVT_WDF_DEVICE_PREPARE_HARDWARE prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE release;
static EVT_WDF_DEVICE_D0_ENTRY d0_entry;
static EVT_WDF_DEVICE_D0_EXIT d0_exit;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL surprise;
static EVT_WDF_DEVICE_QUERY_REMOVE query_remove;
static EVT_WDF_DEVICE_QUERY_STOP query_stop;
static EVT_WDFDEVICE_WDM_IRP_PREPROCESS relations;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP cleanup;
static EVT_WDF_WORKITEM gcc_remove_complete;

static void lock(GPUCC_DEVICE *d) { KeWaitForSingleObject(&d->lock, Executive, KernelMode, FALSE, NULL); }
static void unlock(GPUCC_DEVICE *d) { KeReleaseMutex(&d->lock, FALSE); }

static void refresh_relations(GPUCC_DEVICE *d)
{
    if (!d->removed) IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(d->device), PowerRelations);
}

static void forget_gcc_target(GPUCC_DEVICE *d)
{
    if (!d->gcc_target.pdo) return;
    Sm8750CloseClockTarget(&d->gcc_target);
    refresh_relations(d);
}

static void gcc_removed(PVOID context)
{
    WdfWorkItemEnqueue(((GPUCC_DEVICE *)context)->gcc_remove_work);
}

/* Notify Windows even when no clock client is active. An active interface
 * survives until its caller retires any remaining hardware requests. */
static void gcc_remove_complete(WDFWORKITEM work)
{
    GPUCC_DEVICE *d = gpucc_device((WDFDEVICE)WdfWorkItemGetParentObject(work));
    lock(d);
    if (Sm8750ClockTargetStatus(&d->gcc_target) == STATUS_DEVICE_REMOVED)
        forget_gcc_target(d);
    unlock(d);
}

static NTSTATUS check_gcc(GPUCC_DEVICE *d)
{
    if (!d->gcc_target.pdo) return STATUS_DEVICE_REMOVED;
    ULONGLONG now;
    NTSTATUS s = Sm8750ClockTargetStatus(&d->gcc_target);
    if (NT_SUCCESS(s)) s = d->clocks.gcc.PowerGeneration(d->clocks.gcc.Header.Context, &now);
    if (!NT_SUCCESS(s)) return s;
    if (d->gcc_generation && now != d->gcc_generation) return STATUS_DEVICE_POWER_FAILURE;
    d->gcc_generation = now;
    return STATUS_SUCCESS;
}

static NTSTATUS connect_gcc(GPUCC_DEVICE *d)
{
    SM8750_GCC_GPU_INTERFACE *api = &d->clocks.gcc;
    if (api->SetClock) return check_gcc(d);
    if (Sm8750ClockTargetStatus(&d->gcc_target) == STATUS_DEVICE_REMOVED)
        forget_gcc_target(d);
    ULONG_PTR previous = (ULONG_PTR)d->gcc_target.pdo;
    NTSTATUS s = Sm8750QueryClockInterface(&GUID_DEVINTERFACE_SM8750_GCC, &GUID_SM8750_GCC_GPU_INTERFACE,
                                           &api->Header, sizeof(*api), WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(d->device)),
                                           &d->gcc_target);
    if (NT_SUCCESS(s) && (!api->SetClock || !api->Abandon || !api->PowerGeneration)) {
        api->Header.InterfaceDereference(api->Header.Context);
        RtlZeroMemory(api, sizeof(*api));
        Sm8750CloseClockTarget(&d->gcc_target);
        s = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if ((ULONG_PTR)d->gcc_target.pdo != previous) refresh_relations(d);
    return NT_SUCCESS(s) ? check_gcc(d) : s;
}

static void disconnect_gcc(GPUCC_DEVICE *d)
{
    SM8750_GCC_GPU_INTERFACE *api = &d->clocks.gcc;
    if (!api->Header.InterfaceDereference) return;
    api->Header.InterfaceDereference(api->Header.Context);
    RtlZeroMemory(api, sizeof(*api));
    d->gcc_generation = 0;
}

static void release_unused_gcc(GPUCC_DEVICE *d)
{
    for (unsigned i = 0; i < RTL_NUMBER_OF(d->clocks.gcc_held); ++i)
        if (d->clocks.gcc_held[i]) return;
    disconnect_gcc(d);
}

/* Supply the power manager with the actual translated GCC stack. Do not
 * lose relations already added by an upper driver, or duplicate its PDO. */
static NTSTATUS relations(WDFDEVICE device, PIRP irp)
{
    if (IoGetCurrentIrpStackLocation(irp)->Parameters.QueryDeviceRelations.Type == PowerRelations) {
        GPUCC_DEVICE *d = gpucc_device(device);
        lock(d);
        if (d->gcc_target.pdo && Sm8750ClockTargetStatus(&d->gcc_target) != STATUS_DEVICE_REMOVED) {
            PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)irp->IoStatus.Information;
            ULONG n = old ? old->Count : 0;
            ULONG i = 0;
            while (i < n && old->Objects[i] != d->gcc_target.pdo) ++i;
            if (i == n && n != MAXULONG) {
                SIZE_T bytes = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + ((SIZE_T)n + 1) * sizeof(PDEVICE_OBJECT);
                PDEVICE_RELATIONS result = ExAllocatePool2(POOL_FLAG_PAGED, bytes, 'rCgA');
                if (result) {
                    result->Count = n + 1;
                    if (old) {
                        RtlCopyMemory(result->Objects, old->Objects, n * sizeof(PDEVICE_OBJECT));
                        ExFreePool(old);
                    }
                    ObReferenceObject(d->gcc_target.pdo);
                    result->Objects[n] = d->gcc_target.pdo;
                    irp->IoStatus.Information = (ULONG_PTR)result;
                    irp->IoStatus.Status = STATUS_SUCCESS;
                }
            }
        }
        unlock(d);
    }
    IoSkipCurrentIrpStackLocation(irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(device, irp);
}

static GPUCC_REQUEST *find(GPUCC_DEVICE *d, PVOID client)
{
    for (LIST_ENTRY *e = d->requests.Flink; e != &d->requests; e = e->Flink) {
        GPUCC_REQUEST *r = CONTAINING_RECORD(e, GPUCC_REQUEST, link);
        if (r->client == client) return r;
    }
    return NULL;
}

static void idle(GPUCC_DEVICE *d)
{
    if (d->idle_held) {
        WdfDeviceResumeIdle(d->device);
        d->idle_held = FALSE;
    }
}

static NTSTATUS set_clocks(PVOID context, PVOID client, enum adreno_clock_client type, BOOLEAN on)
{
    GPUCC_DEVICE *d = context;
    PAGED_CODE();
    if (!client || (unsigned)type > AdrenoSmmuClocks) return STATUS_INVALID_PARAMETER;
    lock(d);
    GPUCC_REQUEST *r = find(d, client);
    NTSTATUS s = STATUS_SUCCESS;
    if (r && r->type != type) { s = STATUS_INVALID_PARAMETER; goto done; }
    if (on) {
        if (d->removed) { s = STATUS_DEVICE_REMOVED; goto done; }
        if (!d->online || !d->clocks.gpucc) { s = STATUS_DEVICE_NOT_READY; goto done; }
        if (d->stale) { s = STATUS_DEVICE_POWER_FAILURE; goto done; }
        if (!d->idle_held) {
            s = WdfDeviceStopIdle(d->device, FALSE);
            if (!NT_SUCCESS(s)) goto done;
            d->idle_held = TRUE;
        }
        if (type != AdrenoSmmuClocks) {
            s = connect_gcc(d);
            if (!NT_SUCCESS(s)) goto done;
        }
        if (!r) {
            r = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*r), 'qCgA');
            if (!r) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }
            r->client = client;
            r->type = type;
            InsertTailList(&d->requests, &r->link);
        }
        s = AdrenoSetGpuClocks(&d->clocks, &r->clocks, type, TRUE);
    } else if (r) {
        if (r->clocks.controller) {
            /* D3hot can retain the hub mux and the AP vote. After the
             * client has drained DMA, park child roots before dropping
             * their parents. Retry after D0 if the mapping still exists. */
            if (!d->online) { s = STATUS_DEVICE_NOT_READY; goto done; }
            s = AdrenoSetGpuClocks(&d->clocks, &r->clocks, type, FALSE);
        }
        if (NT_SUCCESS(s)) {
            RemoveEntryList(&r->link);
            ExFreePoolWithTag(r, 'qCgA');
        }
    }
done:
    release_unused_gcc(d);
    if (IsListEmpty(&d->requests)) {
        d->stale = FALSE;
        idle(d);
    }
    unlock(d);
    return s;
}

static NTSTATUS prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(raw);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory || r->Type == CmResourceTypeMemoryLarge) {
            if (memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type != CmResourceTypeNull && r->Type != CmResourceTypeDevicePrivate &&
                   r->Type != CmResourceTypeDeviceSpecific) return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    static const char model[] = "qcom,sm8750-gpucc";
    NTSTATUS s = Sm8750CheckClockModel(WdfDeviceGetIoTarget(device), model, sizeof(model));
    if (s == STATUS_NOT_SUPPORTED) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!NT_SUCCESS(s)) return s;
    GPUCC_DEVICE *d = gpucc_device(device);
    lock(d);
    s = d->removed ? STATUS_DEVICE_REMOVED : AdrenoPrepareGpuClocks(&d->clocks, memory);
    unlock(d);
    return s;
}

static NTSTATUS d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    GPUCC_DEVICE *d = gpucc_device(device);
    lock(d);
    NTSTATUS s = d->removed ? STATUS_DEVICE_REMOVED :
                 !d->clocks.gpucc ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    if (NT_SUCCESS(s)) {
        d->online = TRUE;
    }
    unlock(d);
    return s;
}

static NTSTATUS d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
    UNREFERENCED_PARAMETER(next);
    GPUCC_DEVICE *d = gpucc_device(device);
    lock(d);
    d->online = FALSE;
    if (d->clocks.cx.initialized) {
        d->clocks.cx.resuming = TRUE;
        d->clocks.cx.initialized = FALSE;
    }
    /* A system sleep can revoke D0 despite idle references. Do not touch
     * clocks under outstanding GPU DMA, and do not report old requests
     * as restored just because the provider is back in D0. */
    if (!IsListEmpty(&d->requests)) d->stale = TRUE;
    unlock(d);
    return STATUS_SUCCESS;
}

static void revoke(GPUCC_DEVICE *d)
{
    d->online = FALSE;
    d->stale = !IsListEmpty(&d->requests);
    AdrenoForgetGpuClocks(&d->clocks);
    disconnect_gcc(d);
    forget_gcc_target(d);
    idle(d);
    if (d->removed) {
        while (!IsListEmpty(&d->requests)) {
            LIST_ENTRY *entry = RemoveHeadList(&d->requests);
            ExFreePoolWithTag(CONTAINING_RECORD(entry, GPUCC_REQUEST, link), 'qCgA');
        }
        d->stale = FALSE;
    } else {
        /* A new mapping has no clock or CX references from this one.
         * Keep client identities until they release their old requests. */
        for (LIST_ENTRY *e = d->requests.Flink; e != &d->requests; e = e->Flink) {
            GPUCC_REQUEST *r = CONTAINING_RECORD(e, GPUCC_REQUEST, link);
            RtlZeroMemory(&r->clocks, sizeof(r->clocks));
        }
    }
}

static void surprise(WDFDEVICE device)
{
    GPUCC_DEVICE *d = gpucc_device(device);
    lock(d);
    d->removed = TRUE;
    revoke(d);
    unlock(d);
}

static void cleanup(WDFOBJECT object)
{
    if (gpucc_device((WDFDEVICE)object)->device) surprise((WDFDEVICE)object);
}

static NTSTATUS release(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    GPUCC_DEVICE *d = gpucc_device(device);
    lock(d);
    /* Stop watching before KMDF can delete the device's work item. */
    revoke(d);
    NTSTATUS s = AdrenoReleaseGpuClocks(&d->clocks);
    unlock(d);
    return s;
}

static void reference(PVOID context)
{
    GPUCC_DEVICE *d = context;
    WdfObjectReference(d->device);
    InterlockedIncrement(&d->references);
}

static void dereference(PVOID context)
{
    GPUCC_DEVICE *d = context;
    InterlockedDecrement(&d->references);
    WdfObjectDereference(d->device);
}

static NTSTATUS query_remove(WDFDEVICE device)
{
    return InterlockedCompareExchange(&gpucc_device(device)->references, 0, 0) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
static NTSTATUS query_stop(WDFDEVICE device) { return query_remove(device); }

static NTSTATUS add_device(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    UCHAR minor = IRP_MN_QUERY_DEVICE_RELATIONS;
    NTSTATUS s = WdfDeviceInitAssignWdmIrpPreprocessCallback(init, relations, IRP_MJ_PNP, &minor, 1);
    if (!NT_SUCCESS(s)) return s;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = prepare;
    pnp.EvtDeviceReleaseHardware = release;
    pnp.EvtDeviceD0Entry = d0_entry;
    pnp.EvtDeviceD0Exit = d0_exit;
    pnp.EvtDeviceSurpriseRemoval = surprise;
    pnp.EvtDeviceQueryRemove = query_remove;
    pnp.EvtDeviceQueryStop = query_stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, GPUCC_DEVICE);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    attrs.EvtCleanupCallback = cleanup;
    WDFDEVICE device;
    s = WdfDeviceCreate(&init, &attrs, &device);
    if (!NT_SUCCESS(s)) return s;
    GPUCC_DEVICE *d = gpucc_device(device);
    d->device = device;
    InitializeListHead(&d->requests);
    KeInitializeMutex(&d->lock, 0);
    WDF_WORKITEM_CONFIG work;
    WDF_WORKITEM_CONFIG_INIT(&work, gcc_remove_complete);
    work.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    s = WdfWorkItemCreate(&work, &attrs, &d->gcc_remove_work);
    if (!NT_SUCCESS(s)) return s;
    d->gcc_target.removed = gcc_removed;
    d->gcc_target.context = d;

    ADRENO_GPUCC_INTERFACE api = {
        .Header = {sizeof(api), 1, d, reference, dereference}, .SetClocks = set_clocks
    };
    WDF_QUERY_INTERFACE_CONFIG query;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&query, &api.Header, &GUID_SM8750_GPUCC_INTERFACE, NULL);
    s = WdfDeviceAddQueryInterface(device, &query);
    if (!NT_SUCCESS(s)) return s;
    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_SM8750_GPUCC, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, add_device);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
