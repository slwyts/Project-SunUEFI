/* SPDX-License-Identifier: MIT */
#include <ntddk.h>
#include <wdf.h>
#include "acpi.h"
#include "fixed_pll.h"

typedef struct {
    WDFDEVICE device;
    SM8750_GCC_PLLS clocks;
    KSPIN_LOCK registers;
    KMUTEX clients;
    volatile LONG references, started;
    BOOLEAN idle_held;
} GCC_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCC_DEVICE, gcc_device)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD add_device;
static EVT_WDF_DEVICE_PREPARE_HARDWARE prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE release;
static EVT_WDF_DEVICE_D0_ENTRY d0_entry;
static EVT_WDF_DEVICE_D0_EXIT d0_exit;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL surprise;
static EVT_WDF_DEVICE_QUERY_REMOVE query_remove;
static EVT_WDF_DEVICE_QUERY_STOP query_stop;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP cleanup;

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
        } else if (r->Type != CmResourceTypeDevicePrivate && r->Type != CmResourceTypeDeviceSpecific &&
                   r->Type != CmResourceTypeNull) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    ULONGLONG start, end;
    if (!Sm8750GccResource(memory, &start, &end))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* _UID distinguishes instances, not hardware IDs. Check the firmware's
     * model before assigning any vendor-specific meaning to a resource. */
    NTSTATUS s = Sm8750CheckGccModel(WdfDeviceGetIoTarget(device));
    if (s == STATUS_NOT_SUPPORTED) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!NT_SUCCESS(s)) return s;
    PHYSICAL_ADDRESS address;
    address.QuadPart = (LONGLONG)start;
    PVOID mapping = MmMapIoSpaceEx(address, SM8750_GCC_REQUIRED_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
    if (!mapping) return STATUS_INSUFFICIENT_RESOURCES;
    s = Sm8750PrepareGccPlls(&gcc_device(device)->clocks, memory, mapping);
    if (!NT_SUCCESS(s)) MmUnmapIoSpace(mapping, SM8750_GCC_REQUIRED_BYTES);
    return s;
}

static NTSTATUS release(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    GCC_DEVICE *d = gcc_device(device);
    InterlockedExchange(&d->started, FALSE);
    PVOID mapping = Sm8750DetachGcc(&d->clocks);
    if (mapping) MmUnmapIoSpace(mapping, SM8750_GCC_REQUIRED_BYTES);
    return STATUS_SUCCESS;
}

static NTSTATUS d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    GCC_DEVICE *d = gcc_device(device);
    NTSTATUS s = Sm8750GccPower(&d->clocks, TRUE, FALSE);
    if (NT_SUCCESS(s)) InterlockedExchange(&d->started, TRUE);
    return s;
}

static NTSTATUS d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
    UNREFERENCED_PARAMETER(next);
    return Sm8750GccPower(&gcc_device(device)->clocks, FALSE, FALSE);
}

static void resume_idle(WDFDEVICE device, GCC_DEVICE *d)
{
    if (d->idle_held) {
        d->idle_held = FALSE;
        WdfDeviceResumeIdle(device);
    }
}

/* Called under clients. Each PLL user and the UFS adapter share a single
 * S0 reference; Sx and surprise removal can still revoke register access. */
static NTSTATUS hold_idle(GCC_DEVICE *d)
{
    if (d->clocks.removed) return STATUS_DEVICE_REMOVED;
    if (!InterlockedCompareExchange(&d->started, 0, 0)) return STATUS_DEVICE_NOT_READY;
    if (d->idle_held) return STATUS_SUCCESS;
    NTSTATUS s = WdfDeviceStopIdle(d->device, FALSE);
    if (NT_SUCCESS(s)) d->idle_held = TRUE;
    return s;
}

static void release_unused(GCC_DEVICE *d)
{
    if (!d->clocks.ufs && !d->clocks.pll[0].users && !d->clocks.pll[1].users) resume_idle(d->device, d);
}

static void offline(WDFDEVICE device)
{
    GCC_DEVICE *d = gcc_device(device);
    KeWaitForSingleObject(&d->clients, Executive, KernelMode, FALSE, NULL);
    (void)Sm8750GccPower(&d->clocks, FALSE, TRUE);
    resume_idle(device, d);
    KeReleaseMutex(&d->clients, FALSE);
}

static VOID surprise(WDFDEVICE device) { offline(device); }
static VOID cleanup(WDFOBJECT object) { offline((WDFDEVICE)object); }

static NTSTATUS query_remove(WDFDEVICE device)
{
    return InterlockedCompareExchange(&gcc_device(device)->references, 0, 0) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
static NTSTATUS query_stop(WDFDEVICE device) { return query_remove(device); }

static VOID reference(PVOID context)
{
    GCC_DEVICE *d = context;
    WdfObjectReference(d->device);
    InterlockedIncrement(&d->references);
}

static VOID dereference(PVOID context)
{
    GCC_DEVICE *d = context;
    InterlockedDecrement(&d->references);
    WdfObjectDereference(d->device);
}

static NTSTATUS set_pll(PVOID context, enum sm8750_gcc_pll clock, BOOLEAN *held, BOOLEAN enable)
{
    GCC_DEVICE *d = context;
    KeWaitForSingleObject(&d->clients, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = enable ? hold_idle(d) : STATUS_SUCCESS;
    if (NT_SUCCESS(s)) s = Sm8750SetGccPll(&d->clocks, clock, held, enable);
    release_unused(d);
    KeReleaseMutex(&d->clients, FALSE);
    return s;
}

static NTSTATUS open_ufs(PVOID context, PVOID adapter)
{
    GCC_DEVICE *d = context;
    KeWaitForSingleObject(&d->clients, Executive, KernelMode, FALSE, NULL);
    /* Do not wait for a sibling stack to power up inside PnP. If D0 is
     * still pending the caller retries after the supplier is ready. */
    NTSTATUS s = hold_idle(d);
    if (NT_SUCCESS(s)) s = Sm8750OpenUfsClocks(&d->clocks, adapter);
    release_unused(d);
    KeReleaseMutex(&d->clients, FALSE);
    return s;
}

static VOID close_ufs(PVOID context, PVOID adapter)
{
    GCC_DEVICE *d = context;
    KeWaitForSingleObject(&d->clients, Executive, KernelMode, FALSE, NULL);
    (void)Sm8750CloseUfsClocks(&d->clocks, adapter);
    release_unused(d);
    KeReleaseMutex(&d->clients, FALSE);
}

static BOOLEAN ufs_power_retained(PVOID ctx, PVOID adapter)
{
    GCC_DEVICE *d = ctx;
    return Sm8750UfsPowerRetained(&d->clocks, adapter);
}
static ULONG read_ufs(PVOID ctx, PVOID adapter, ULONG offset)
{
    GCC_DEVICE *d = ctx;
    return Sm8750ReadUfsClock(&d->clocks, adapter, offset);
}
static VOID write_ufs(PVOID ctx, PVOID adapter, ULONG offset, ULONG value)
{
    GCC_DEVICE *d = ctx;
    Sm8750WriteUfsClock(&d->clocks, adapter, offset, value);
}

static NTSTATUS add_device(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
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
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, GCC_DEVICE);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    attrs.EvtCleanupCallback = cleanup;
    WDFDEVICE device;
    NTSTATUS s = WdfDeviceCreate(&init, &attrs, &device);
    if (!NT_SUCCESS(s)) return s;
    GCC_DEVICE *d = gcc_device(device);
    d->device = device;
    KeInitializeMutex(&d->clients, 0);
    KeInitializeSpinLock(&d->registers);
    Sm8750InitializeGcc(&d->clocks, &d->registers);
    SM8750_GCC_INTERFACE api = {
        .Header = {sizeof(api), SM8750_GCC_INTERFACE_VERSION, d, reference, dereference},
        .Set = set_pll, .OpenUfs = open_ufs, .CloseUfs = close_ufs, .UfsPowerRetained = ufs_power_retained,
        .ReadUfs = read_ufs, .WriteUfs = write_ufs,
    };
    WDF_QUERY_INTERFACE_CONFIG query;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&query, &api.Header, &GUID_SM8750_GCC_INTERFACE, NULL);
    s = WdfDeviceAddQueryInterface(device, &query);
    if (!NT_SUCCESS(s)) return s;
    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_SM8750_GCC, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, add_device);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
