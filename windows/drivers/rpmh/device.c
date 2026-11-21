/* SPDX-License-Identifier: BSD-2-Clause */
#include <ntddk.h>
#include <wdf.h>
#include "interface.h"

enum { DB_BYTES = 0x20000, APPS_GSI = 37,
       REQUEST_MS = 100, RETRY_MS = 100 };
#define DB_BASE UINT64_C(0x81c60000)
#define RPMH_TAG 'hmPR'

typedef struct {
    WDFDEVICE device;
    WDFWAITLOCK lock;
    WDFINTERRUPT interrupt;
    WDFTIMER retry;
    KEVENT completion;
    PUCHAR registers, db_copy;
    PHYSICAL_ADDRESS db_address;
    struct rpmh_provider provider;
    struct rpmh_rsc rsc;
    volatile LONG removed, retry_pending;
    BOOLEAN accessible, online, restart_provider, idle_reference;
} RPMH_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RPMH_DEVICE, rpmh_device)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD add_device;
static EVT_WDF_DEVICE_PREPARE_HARDWARE prepare_hardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE release_hardware;
static EVT_WDF_DEVICE_D0_ENTRY d0_entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED d0_ready;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED d0_drain;
static EVT_WDF_DEVICE_D0_EXIT d0_exit;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL surprise_remove;
static EVT_WDF_DEVICE_QUERY_REMOVE query_remove;
static EVT_WDF_DEVICE_QUERY_STOP query_stop;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP cleanup_device;
static EVT_WDF_OBJECT_CONTEXT_DESTROY destroy_device;
static EVT_WDF_INTERRUPT_ISR handle_interrupt;
static EVT_WDF_INTERRUPT_DPC interrupt_dpc;
static EVT_WDF_TIMER retry_transfer;

static void lock_device(RPMH_DEVICE *d) { (void)WdfWaitLockAcquire(d->lock, NULL); }
static void unlock_device(RPMH_DEVICE *d) { WdfWaitLockRelease(d->lock); }
static int gone(RPMH_DEVICE *d) { return InterlockedCompareExchange(&d->removed, 0, 0) != 0; }

static int read_register(void *ctx, uint32_t offset, uint32_t *value)
{
    RPMH_DEVICE *d = ctx;
    if (gone(d) || !d->accessible || (offset & 3) || offset > RPMH_APPS_DRV2_SIZE-4) return 0;
    *value = READ_REGISTER_ULONG((PULONG)(d->registers + offset));
    return 1;
}

static void write_register(void *ctx, uint32_t offset, uint32_t value)
{
    RPMH_DEVICE *d = ctx;
    if (!gone(d) && d->accessible && !(offset & 3) && offset <= RPMH_APPS_DRV2_SIZE-4)
        WRITE_REGISTER_ULONG((PULONG)(d->registers + offset), value);
}

static BOOLEAN handle_interrupt(WDFINTERRUPT irq, ULONG id)
{
    RPMH_DEVICE *d = rpmh_device(WdfInterruptGetDevice(irq));
    if (id || !d->accessible || gone(d) || !rpmh_rsc_interrupt(&d->rsc)) return FALSE;
    (void)WdfInterruptQueueDpcForIsr(irq);
    return TRUE;
}

static void interrupt_dpc(WDFINTERRUPT irq, WDFOBJECT device)
{
    UNREFERENCED_PARAMETER(device);
    RPMH_DEVICE *d = rpmh_device(WdfInterruptGetDevice(irq));
    KeSetEvent(&d->completion, IO_NO_INCREMENT, FALSE);
    if (!gone(d) && InterlockedCompareExchange(&d->retry_pending, 0, 0))
        (void)WdfTimerStart(d->retry, WDF_REL_TIMEOUT_IN_MS(1));
}

static void release_idle(RPMH_DEVICE *d)
{
    InterlockedExchange(&d->retry_pending, 0);
    if (d->idle_reference) {
        d->idle_reference = FALSE;
        WdfDeviceResumeIdle(d->device);
    }
}

static NTSTATUS hold_idle(RPMH_DEVICE *d)
{
    if (d->idle_reference) return STATUS_SUCCESS;
    NTSTATUS s = WdfDeviceStopIdle(d->device, FALSE);
    if (NT_SUCCESS(s)) d->idle_reference = TRUE;
    return s;
}

static int can_restore(const struct rpmh_provider *p)
{
    return (p->restore_rails || p->restore_bcms || p->vrm.restore) && rpmh_provider_can_reconcile(p);
}

static void retry_later(RPMH_DEVICE *d)
{
    if (gone(d) || !d->online) return;
    (void)hold_idle(d);
    if (!InterlockedExchange(&d->retry_pending, 1))
        (void)WdfTimerStart(d->retry, WDF_REL_TIMEOUT_IN_MS(RETRY_MS));
}

static void update_retry(RPMH_DEVICE *d)
{
    if (!d->online || gone(d)) return;
    if (!rpmh_rsc_idle(&d->rsc) || can_restore(&d->provider)) retry_later(d);
    else release_idle(d);
}

static void wait_for_reply(RPMH_DEVICE *d)
{
    LARGE_INTEGER tick;
    tick.QuadPart = -10000;  /* Also progress posted writes before an IRQ exists. */
    (void)KeWaitForSingleObject(&d->completion, Executive, KernelMode, FALSE, &tick);
}

/* Caller holds the passive lock. Only write requests are exposed by the
 * platform. The ISR and all RSC transitions share the WDF interrupt lock. */
static enum rpmh_rsc_result poll_request(RPMH_DEVICE *d, BOOLEAN cancel_unsent)
{
    enum rpmh_rsc_result r;
    WdfInterruptAcquireLock(d->interrupt);
    if (gone(d) || !d->accessible) r = RPMH_RSC_IO_ERROR;
    else if (rpmh_rsc_idle(&d->rsc)) r = RPMH_RSC_OK;
    else {
        /* An untriggered request can be withdrawn; a triggered one cannot. */
        r = cancel_unsent ? rpmh_rsc_cancel_unsent(&d->rsc) : RPMH_RSC_BUSY;
        if (r == RPMH_RSC_BUSY) r = rpmh_rsc_poll(&d->rsc, NULL, 0);
    }
    WdfInterruptReleaseLock(d->interrupt);
    return r;
}

static int retire(RPMH_DEVICE *d, ULONGLONG until, BOOLEAN cancel_unsent)
{
    for (;;) {
        enum rpmh_rsc_result r = poll_request(d, cancel_unsent);
        if (r == RPMH_RSC_OK || r == RPMH_RSC_CANCELLED) return 1;
        if (r != RPMH_RSC_PENDING || KeQueryInterruptTime() >= until || gone(d)) return 0;
        wait_for_reply(d);
    }
}

static int submit(void *context, const struct rpmh_rsc_command *commands, unsigned count, unsigned wait_mask)
{
    RPMH_DEVICE *d = context;
    ULONGLONG until = KeQueryInterruptTime() + REQUEST_MS * 10000;
    if (!d->online || gone(d)) return 0;
    if (!NT_SUCCESS(hold_idle(d))) return 0;
    if (!retire(d, until, TRUE)) { retry_later(d); return 0; }
    /* A late completion above belongs to the previous command, never to
     * the caller's new vote. Submit the complete new batch explicitly. */
    enum rpmh_rsc_result r;
    do {
        WdfInterruptAcquireLock(d->interrupt);
        r = gone(d) ? RPMH_RSC_IO_ERROR : rpmh_rsc_begin(&d->rsc, commands, count, 1, wait_mask);
        WdfInterruptReleaseLock(d->interrupt);
        if (r != RPMH_RSC_BUSY || KeQueryInterruptTime() >= until) break;
        wait_for_reply(d);
    } while (!gone(d));
    int success = r == RPMH_RSC_PENDING && retire(d, until, FALSE);
    if (!rpmh_rsc_idle(&d->rsc)) retry_later(d);
    return success;
}

static void retry_transfer(WDFTIMER timer)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)WdfTimerGetParentObject(timer));
    lock_device(d);
    InterlockedExchange(&d->retry_pending, 0);
    if (d->online && !gone(d)) {
        if ((rpmh_rsc_idle(&d->rsc) || retire(d, 0, TRUE)) && can_restore(&d->provider))
            (void)rpmh_provider_reconcile(&d->provider);
        update_retry(d);
    }
    unlock_device(d);
}

static NTSTATUS prepare_hardware(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    RPMH_DEVICE *d = rpmh_device(device);
    PHYSICAL_ADDRESS regs = {0}, db = {0};
    unsigned irqs = 0, total = WdfCmResourceListGetCount(translated);
    if (WdfCmResourceListGetCount(raw) != total) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (unsigned i = 0; i < total; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR bus = WdfCmResourceListGetDescriptor(raw, i);
        if (!r || !bus) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeInterrupt) {
            if (bus->Type != CmResourceTypeInterrupt || bus->u.Interrupt.Vector != APPS_GSI ||
                (r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE|CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareDeviceExclusive || ++irqs != 1)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        } else if (r->Type == CmResourceTypeMemory || r->Type == CmResourceTypeMemoryLarge) {
            ULONGLONG start;
            ULONGLONG bytes = RtlCmDecodeMemIoResource(r, &start);
            if (start == RPMH_APPS_DRV2_BASE && bytes == RPMH_APPS_DRV2_SIZE && !regs.QuadPart &&
                !(r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY|CM_RESOURCE_MEMORY_WRITE_ONLY)) &&
                r->ShareDisposition == CmResourceShareDeviceExclusive)
                regs.QuadPart = (LONGLONG)start;
            else if (start == DB_BASE && bytes == DB_BYTES && !db.QuadPart &&
                     (r->Flags & CM_RESOURCE_MEMORY_READ_ONLY) && !(r->Flags & CM_RESOURCE_MEMORY_WRITE_ONLY) &&
                     r->ShareDisposition == CmResourceShareDeviceExclusive) db.QuadPart = (LONGLONG)start;
            else return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (!regs.QuadPart || !db.QuadPart || irqs != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    d->registers = MmMapIoSpaceEx(regs, RPMH_APPS_DRV2_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!d->registers) return STATUS_INSUFFICIENT_RESOURCES;
    d->db_address = db;
    return STATUS_SUCCESS;
}

static NTSTATUS load_database(RPMH_DEVICE *d)
{
    struct rpmh_transport transport = {d, submit};
    struct rpmh_cmd_db db;
    PUCHAR copy = ExAllocatePool2(POOL_FLAG_NON_PAGED, DB_BYTES, RPMH_TAG);
    if (!copy) return STATUS_INSUFFICIENT_RESOURCES;
    /* AOP cmd-db is reserved shared RAM, not the RSC register aperture.
     * Linux maps this region WC. Do not create a writable alias. */
    const VOID *mapping = MmMapIoSpaceEx(d->db_address, DB_BYTES, PAGE_READONLY | PAGE_WRITECOMBINE);
    if (!mapping) {
        ExFreePoolWithTag(copy, RPMH_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, mapping, DB_BYTES);
    MmUnmapIoSpace((PVOID)mapping, DB_BYTES);
    NTSTATUS status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (d->db_copy && !d->restart_provider) {
        /* A forced transition left requests or votes to reconcile. Their
         * addresses and corner indices belong to this exact database. */
        if (d->provider.slots == d->rsc.commands_per_tcs &&
            RtlCompareMemory(d->db_copy, copy, DB_BYTES) == DB_BYTES) status = STATUS_SUCCESS;
    } else {
        struct rpmh_client *clients = d->provider.clients;
        if (rpmh_db_open(&db, copy, DB_BYTES) &&
            rpmh_provider_start(&d->provider, &db, &transport, d->rsc.commands_per_tcs)) {
            /* A clean drain preserved the handles, not their old hardware
             * baseline. The platform must provide fresh AP votes this D0. */
            d->provider.clients = clients;
            if (d->db_copy) ExFreePoolWithTag(d->db_copy, RPMH_TAG);
            d->db_copy = copy;
            copy = NULL;
            d->restart_provider = FALSE;
            status = STATUS_SUCCESS;
        }
    }
    if (copy) ExFreePoolWithTag(copy, RPMH_TAG);
    return status;
}

static NTSTATUS d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    RPMH_DEVICE *d = rpmh_device(device);
    UNREFERENCED_PARAMETER(previous);
    struct rpmh_rsc_io io = {d, read_register, write_register};
    NTSTATUS status = STATUS_DEVICE_CONFIGURATION_ERROR;
    lock_device(d);
    if (gone(d) || !d->registers || !d->db_address.QuadPart) goto out;
    /* This callback runs before KMDF connects the interrupt; power is
     * already on. Don't issue requests until the post-interrupt callback. */
    d->accessible = TRUE;
    if (rpmh_rsc_idle(&d->rsc)) {
        if (rpmh_rsc_init(&d->rsc, &io, RPMH_APPS_DRV2_BASE, RPMH_APPS_DRV2_SIZE) != RPMH_RSC_OK) goto out;
    } else {
        struct rpmh_rsc actual = {0};
        if (rpmh_rsc_init(&actual, &io, RPMH_APPS_DRV2_BASE, RPMH_APPS_DRV2_SIZE) != RPMH_RSC_OK ||
            actual.version != d->rsc.version || actual.commands_per_tcs != d->rsc.commands_per_tcs) goto out;
    }
    if (rpmh_rsc_check_irq_handoff(&d->rsc) != RPMH_RSC_OK) { status = STATUS_DEVICE_BUSY; goto out; }
    status = load_database(d);
out:
    if (!NT_SUCCESS(status)) d->accessible = FALSE;
    unlock_device(d);
    return status;
}

static NTSTATUS d0_ready(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    RPMH_DEVICE *d = rpmh_device(device);
    UNREFERENCED_PARAMETER(previous);
    lock_device(d);
    NTSTATUS status = gone(d) ? STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
    if (NT_SUCCESS(status) && !rpmh_rsc_idle(&d->rsc)) status = hold_idle(d);
    d->online = NT_SUCCESS(status);
    update_retry(d);
    unlock_device(d);
    return status;
}

static NTSTATUS d0_drain(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
    RPMH_DEVICE *d = rpmh_device(device);
    UNREFERENCED_PARAMETER(next);
    lock_device(d);
    NTSTATUS status = STATUS_SUCCESS;
    if (!gone(d)) {
        if (!retire(d, KeQueryInterruptTime()+REQUEST_MS*10000, TRUE) ||
            !rpmh_provider_quiet(&d->provider)) status = STATUS_DEVICE_BUSY;
        else d->restart_provider = TRUE;
    }
    /* Once KMDF starts disabling the IRQ, no client can start new I/O,
     * even if Windows subsequently forces a transition after a failed drain. */
    d->online = FALSE;
    InterlockedExchange(&d->retry_pending, 0);
    if (rpmh_rsc_idle(&d->rsc)) release_idle(d);
    unlock_device(d);
    return status;
}

static NTSTATUS d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
    RPMH_DEVICE *d = rpmh_device(device);
    UNREFERENCED_PARAMETER(next);
    (void)WdfTimerStop(d->retry, TRUE);
    lock_device(d);
    /* The ISR is disconnected here. A forced power transition need not
     * honor a failed drain; no subsequent callback may access the aperture. */
    d->online = d->accessible = FALSE;
    if (d->db_copy && !d->restart_provider && !gone(d))
        rpmh_provider_power_lost(&d->provider);
    unlock_device(d);
    return STATUS_SUCCESS;
}

static NTSTATUS release_hardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    RPMH_DEVICE *d = rpmh_device(device);
    UNREFERENCED_PARAMETER(translated);
    lock_device(d);
    d->online = d->accessible = FALSE;
    NTSTATUS status = !gone(d) && !rpmh_rsc_idle(&d->rsc) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (d->registers) MmUnmapIoSpace(d->registers, RPMH_APPS_DRV2_SIZE);
    d->db_address.QuadPart = 0;
    d->registers = NULL;
    /* RSC commands contain values, not host DMA addresses. Keep the
     * unfinished software state, not stale mappings to released resources. */
    release_idle(d);
    unlock_device(d);
    return status;
}

static void surprise_remove(WDFDEVICE device)
{
    RPMH_DEVICE *d = rpmh_device(device);
    InterlockedExchange(&d->removed, 1);
    KeSetEvent(&d->completion, IO_NO_INCREMENT, FALSE);
    lock_device(d);
    d->online = FALSE;
    release_idle(d);
    unlock_device(d);
}

static NTSTATUS can_stop(WDFDEVICE device)
{
    RPMH_DEVICE *d = rpmh_device(device);
    lock_device(d);
    int busy = !rpmh_rsc_idle(&d->rsc) || (d->db_copy && !rpmh_provider_quiet(&d->provider));
    update_retry(d);
    unlock_device(d);
    return busy ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
static NTSTATUS query_remove(WDFDEVICE device) { return can_stop(device); }
static NTSTATUS query_stop(WDFDEVICE device) { return can_stop(device); }

static VOID interface_reference(PVOID context) { WdfObjectReference((WDFDEVICE)context); }
static VOID interface_dereference(PVOID context) { WdfObjectDereference((WDFDEVICE)context); }

static NTSTATUS client_open(PVOID context, struct rpmh_client **out)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    lock_device(d);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (gone(d)) status = STATUS_DEVICE_REMOVED;
    else if (d->online) {
        struct rpmh_client *client = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*client), RPMH_TAG);
        if (!client) status = STATUS_INSUFFICIENT_RESOURCES;
        else if (!rpmh_client_open(&d->provider, client)) {
            ExFreePoolWithTag(client, RPMH_TAG);
            status = STATUS_INTERNAL_ERROR;
        } else {
            interface_reference(context);
            *out = client;
            status = STATUS_SUCCESS;
        }
    }
    unlock_device(d);
    return status;
}

static NTSTATUS client_close(PVOID context, struct rpmh_client *client)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    struct rpmh_client **it = &d->provider.clients;
    while (*it && *it != client) it = &(*it)->next;
    if (!*it) status = STATUS_INVALID_HANDLE;
    else if (gone(d)) {
        *it = client->next; /* Local handle release only. Hardware is gone. */
        status = STATUS_SUCCESS;
    } else if (rpmh_client_close(&d->provider, client)) status = STATUS_SUCCESS;
    else if (d->online) status = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(status)) ExFreePoolWithTag(client, RPMH_TAG);
    update_retry(d);
    unlock_device(d);
    if (NT_SUCCESS(status)) interface_dereference(context);
    return status;
}

static NTSTATUS set_level(PVOID context, struct rpmh_client *client, enum rpmh_rail rail, ULONG level)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS s = gone(d) ? STATUS_DEVICE_REMOVED : !d->online ? STATUS_DEVICE_NOT_READY :
                 rpmh_set_level(&d->provider, client, rail, level) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    update_retry(d);
    unlock_device(d);
    return s;
}

static NTSTATUS set_bandwidth(PVOID context, struct rpmh_client *client, enum rpmh_fabric_path path,
                              ULONG avg, ULONG peak)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS s = gone(d) ? STATUS_DEVICE_REMOVED : !d->online ? STATUS_DEVICE_NOT_READY :
                 rpmh_set_bandwidth(&d->provider, client, path, avg, peak) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    update_retry(d);
    unlock_device(d);
    return s;
}

static NTSTATUS set_ufs_supplies(PVOID context, struct rpmh_client *client,
                                 enum rpmh_supply_load vcc, enum rpmh_supply_load vccq)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS s = gone(d) ? STATUS_DEVICE_REMOVED : !d->online ? STATUS_DEVICE_NOT_READY :
                 rpmh_set_ufs_supplies(&d->provider, client, vcc, vccq) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    update_retry(d);
    unlock_device(d);
    return s;
}

static NTSTATUS adopt_regulators(PVOID context, const struct rpmh_vrm_votes *votes)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS s = gone(d) ? STATUS_DEVICE_REMOVED : !d->online ? STATUS_DEVICE_NOT_READY :
                 rpmh_vrm_adopt(&d->provider.vrm, votes) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    update_retry(d);
    unlock_device(d);
    return s;
}

static NTSTATUS adopt(PVOID context, const struct rpmh_boot_votes *votes)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)context);
    lock_device(d);
    NTSTATUS s = gone(d) ? STATUS_DEVICE_REMOVED : !d->online ? STATUS_DEVICE_NOT_READY :
                 rpmh_provider_adopt(&d->provider, votes) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    update_retry(d);
    unlock_device(d);
    return s;
}

static void cleanup_device(WDFOBJECT object)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)object);
    InterlockedExchange(&d->removed, 1);
}

static void destroy_device(WDFOBJECT object)
{
    RPMH_DEVICE *d = rpmh_device((WDFDEVICE)object);
    if (d->db_copy) ExFreePoolWithTag(d->db_copy, RPMH_TAG);
    /* The wait lock is driver-parented so a referenced query interface can
     * still close its client after the device's PnP resources are removed. */
    if (d->lock) WdfObjectDelete(d->lock);
}

static NTSTATUS add_device(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = prepare_hardware;
    pnp.EvtDeviceReleaseHardware = release_hardware;
    pnp.EvtDeviceD0Entry = d0_entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = d0_ready;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = d0_drain;
    pnp.EvtDeviceD0Exit = d0_exit;
    pnp.EvtDeviceSurpriseRemoval = surprise_remove;
    pnp.EvtDeviceQueryRemove = query_remove;
    pnp.EvtDeviceQueryStop = query_stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, RPMH_DEVICE);
    a.SynchronizationScope = WdfSynchronizationScopeNone;
    a.EvtCleanupCallback = cleanup_device;
    a.EvtDestroyCallback = destroy_device;
    WDFDEVICE device;
    NTSTATUS s = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(s)) return s;
    RPMH_DEVICE *d = rpmh_device(device);
    d->device = device;
    KeInitializeEvent(&d->completion, SynchronizationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = driver;
    s = WdfWaitLockCreate(&a, &d->lock);
    if (!NT_SUCCESS(s)) return s;
    WDF_INTERRUPT_CONFIG irq;
    WDF_INTERRUPT_CONFIG_INIT(&irq, handle_interrupt, interrupt_dpc);
    irq.ShareVector = WdfFalse;
    irq.AutomaticSerialization = FALSE;
    s = WdfInterruptCreate(device, &irq, WDF_NO_OBJECT_ATTRIBUTES, &d->interrupt);
    if (!NT_SUCCESS(s)) return s;
    WDF_TIMER_CONFIG timer;
    WDF_TIMER_CONFIG_INIT(&timer, retry_transfer);
    timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = device;
    a.ExecutionLevel = WdfExecutionLevelPassive;
    s = WdfTimerCreate(&timer, &a, &d->retry);
    if (!NT_SUCCESS(s)) return s;
    PIANO_RPMH_INTERFACE api = {0};
    api.Header.Size = sizeof(api);
    api.Header.Version = PIANO_RPMH_INTERFACE_VERSION;
    api.Header.Context = device;
    api.Header.InterfaceReference = interface_reference;
    api.Header.InterfaceDereference = interface_dereference;
    api.Open = client_open;
    api.Close = client_close;
    api.SetLevel = set_level;
    api.SetBandwidth = set_bandwidth;
    api.Adopt = adopt;
    api.SetUfsSupplies = set_ufs_supplies;
    api.AdoptRegulators = adopt_regulators;
    WDF_QUERY_INTERFACE_CONFIG query;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&query, &api.Header, &GUID_PIANO_RPMH_INTERFACE, NULL);
    return WdfDeviceAddQueryInterface(device, &query);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG configuration;
    WDF_DRIVER_CONFIG_INIT(&configuration, add_device);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &configuration, WDF_NO_HANDLE);
}
