/* SPDX-License-Identifier: BSD-2-Clause */
#include <ntddk.h>
#include <wdf.h>
#include "interface.h"
#include "piano_cpucp.h"

typedef struct {
    PIANO_CPUCP_TX Tx;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Shmem, Doorbell;
} CPUCP_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CPUCP_DEVICE, cpucp_device)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD add_device;
static EVT_WDF_DEVICE_PREPARE_HARDWARE prepare;
static EVT_WDF_DEVICE_D0_ENTRY start;
static EVT_WDF_DEVICE_D0_EXIT stop;
static EVT_WDF_DEVICE_RELEASE_HARDWARE release;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL surprise;

static NTSTATUS prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(raw);
    CPUCP_DEVICE *d = cpucp_device(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR shm = NULL, bell = NULL;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type != CmResourceTypeMemory) continue;
        /* TX0 owns one contiguous 512-byte slot and its channel's 32-bit
         * doorbell, not the RX notification slot or other clients' doorbells. */
        if (r->u.Memory.Length == sizeof(ULONG) && !bell) bell = r;
        else if (r->u.Memory.Length == 512 && !shm) shm = r;
        else return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!shm || !bell) return STATUS_DEVICE_CONFIGURATION_ERROR;
    d->Shmem = *shm;
    d->Doorbell = *bell;
    return STATUS_SUCCESS;
}

static NTSTATUS start(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    CPUCP_DEVICE *d = cpucp_device(device);
    return PianoCpucpStart(&d->Tx, &d->Shmem, &d->Doorbell);
}

static NTSTATUS stop(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
    UNREFERENCED_PARAMETER(next);
    return PianoCpucpStop(&cpucp_device(device)->Tx, FALSE);
}

static NTSTATUS release(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    PianoCpucpRelease(&cpucp_device(device)->Tx);
    return STATUS_SUCCESS;
}

static VOID surprise(WDFDEVICE device)
{
    (void)PianoCpucpStop(&cpucp_device(device)->Tx, TRUE);
}

static VOID reference(PVOID device) { WdfObjectReference((WDFDEVICE)device); }
static VOID dereference(PVOID device) { WdfObjectDereference((WDFDEVICE)device); }

static NTSTATUS add_device(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = prepare;
    pnp.EvtDeviceD0Entry = start;
    pnp.EvtDeviceD0Exit = stop;
    pnp.EvtDeviceReleaseHardware = release;
    pnp.EvtDeviceSurpriseRemoval = surprise;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, CPUCP_DEVICE);
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;
    CPUCP_DEVICE *d = cpucp_device(device);
    PianoCpucpInitialize(&d->Tx);
    CPUCP_SCMI_INTERFACE api = {0};
    api.Header.Size = sizeof(api);
    api.Header.Version = 1;
    api.Header.Context = device;
    api.Header.InterfaceReference = reference;
    api.Header.InterfaceDereference = dereference;
    api.Transport = (struct scmi_transport){ &d->Tx, PianoCpucpMessage };
    WDF_QUERY_INTERFACE_CONFIG query;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&query, &api.Header, &GUID_PIANO_CPUCP_SCMI_INTERFACE, NULL);
    return WdfDeviceAddQueryInterface(device, &query);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, add_device);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
