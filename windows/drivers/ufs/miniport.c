/* SPDX-License-Identifier: BSD-2-Clause */
#include "miniport.h"
#include <srbhelper.h>

enum { PRDS = 65, PCI_UFS_SUBCLASS = 9, PCI_UFSHCI = 1, HCI_INTERRUPT_ENABLE = 0x24, HCI_ENABLE = 0x34,
       START_POLL_US = 1000, RETIRE_TIMEOUT_US = 500000 };

DRIVER_INITIALIZE DriverEntry;
static HW_FIND_ADAPTER find_pci;
static HW_INITIALIZE initialize;
static HW_PASSIVE_INITIALIZE_ROUTINE initialize_passive;
static HW_STARTIO start_io;
static HW_INTERRUPT interrupt;
static HW_RESET_BUS reset_bus;
static HW_ADAPTER_CONTROL adapter_control;
static HW_TIMER poll_restart;
static HW_WORKITEM restore_worker;
static STOR_SYNCHRONIZED_ACCESS start_step;
static STOR_SYNCHRONIZED_ACCESS timed_step;
static STOR_SYNCHRONIZED_ACCESS fail_restore;
static STOR_SYNCHRONIZED_ACCESS retry_restore;
static STOR_SYNCHRONIZED_ACCESS synchronized_reset;
static STOR_SYNCHRONIZED_ACCESS surprise_removed;

static BOOLEAN clock_us(UFS_ADAPTER *a, ULONGLONG *us)
{
    LARGE_INTEGER counter, frequency;
    if (StorPortQueryPerformanceCounter(a, a->frequency ? NULL : &frequency, &counter) != STOR_STATUS_SUCCESS ||
        counter.QuadPart < 0) return FALSE;
    if (!a->frequency) {
        if (frequency.QuadPart <= 0 || (ULONGLONG)frequency.QuadPart > UINT64_MAX / 1000000) return FALSE;
        a->frequency = (ULONGLONG)frequency.QuadPart;
    }
    ULONGLONG ticks = (ULONGLONG)counter.QuadPart;
    *us = ticks / a->frequency * 1000000 + ticks % a->frequency * 1000000 / a->frequency;
    return TRUE;
}

static ULONG configure(UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION c)
{
    c->NumberOfBuses = 1;
    c->InitiatorBusId[0] = 0xff;
    c->MaximumNumberOfTargets = 1;
    c->MaximumNumberOfLogicalUnits = UFS_MAX_UNIT_NUMBERS;
    c->TaggedQueuing = c->MultipleRequestPerLu = c->AutoRequestSense = TRUE;
    c->CachesData = TRUE;
    a->state = UfsMapped;
    return SP_RETURN_FOUND;
}

ULONG UfsMiniportFindPiano(UFS_ADAPTER *a, PPORT_CONFIGURATION_INFORMATION c, const struct ufs_platform_power *power)
{
    ULONGLONG now;
    if (!a || !c || !power || !clock_us(a, &now)) return SP_RETURN_BAD_CONFIG;
    enum ufs_platform_error rc = UfsPlatformPrepare(&a->platform, &a->io, a, c, power, 36, PRDS, now);
    return rc == UFS_PLATFORM_OK ? configure(a, c) : SP_RETURN_ERROR;
}

static ULONG find_pci(PVOID extension, PVOID context, PVOID bus, PCHAR arguments,
                      PPORT_CONFIGURATION_INFORMATION c, PBOOLEAN again)
{
    UFS_ADAPTER *a = extension;
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(bus);
    UNREFERENCED_PARAMETER(arguments);
    *again = FALSE;
    if (c->AdapterInterfaceType != PCIBus || !c->AccessRanges || !c->NumberOfAccessRanges)
        return SP_RETURN_NOT_FOUND;
    PCI_COMMON_CONFIG pci;
    if (StorPortGetBusData(a, PCIConfiguration, c->SystemIoBusNumber, c->SlotNumber, &pci, PCI_COMMON_HDR_LENGTH)
        != PCI_COMMON_HDR_LENGTH || pci.VendorID == PCI_INVALID_VENDORID || !pci.VendorID ||
        (pci.HeaderType & 0x7f) != PCI_DEVICE_TYPE || pci.BaseClass != PCI_CLASS_MASS_STORAGE_CTLR ||
        pci.SubClass != PCI_UFS_SUBCLASS || pci.ProgIf != PCI_UFSHCI) return SP_RETURN_NOT_FOUND;
    ULONG bar = pci.u.type0.BaseAddresses[0], type = bar & PCI_ADDRESS_MEMORY_TYPE_MASK;
    if ((bar & PCI_ADDRESS_IO_SPACE) || (type != 0 && type != PCI_TYPE_64BIT)) return SP_RETURN_BAD_CONFIG;
    ULONGLONG base = bar & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    if (type == PCI_TYPE_64BIT) base |= (ULONGLONG)pci.u.type0.BaseAddresses[1] << 32;
    if (!base || (pci.Command & (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER)) !=
                 (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER)) return SP_RETURN_BAD_CONFIG;
    const ACCESS_RANGE *hci = NULL;
    for (ULONG i = 0; i < c->NumberOfAccessRanges; ++i) {
        const ACCESS_RANGE *r = &(*c->AccessRanges)[i];
        if (r->RangeInMemory && (ULONGLONG)r->RangeStart.QuadPart == base) { hci = r; break; }
    }
    if (!hci || !UfsIoPrepare(&a->io, a, c, hci, 64, PRDS)) return SP_RETURN_ERROR;
    return configure(a, c);
}

/* One register/management step with the interrupt lock; waiting is always
 * outside the lock. The ISR never consumes an initialization UPIU.
 */
static BOOLEAN start_step(PVOID extension, PVOID context)
{
    UFS_ADAPTER *a = extension;
    ULONGLONG now = *(ULONGLONG *)context;
    if (a->state != UfsStarting) return FALSE;
    if (!a->io.managing) {
        if (UfsLinkBegin(&a->io, &a->link, now, a->platform.extension ? &a->media : NULL,
                         a->power_operation) == UFS_LINK_OK) {
            a->link.read_parameters = (uint8_t)(a->link_power_ready != NULL);
            return TRUE;
        }
    } else {
        enum ufs_link_phase phase = UfsLinkPoll(&a->link, now);
        if (phase != UFS_LINK_READY && phase != UFS_LINK_FAILED) return TRUE;
        if (!UfsLinkStop(&a->io, &a->link)) {
            if (!a->retire_since) a->retire_since = now;
            if (now - a->retire_since < RETIRE_TIMEOUT_US) return TRUE;
            a->needs_reset = TRUE;
        } else if (a->link.phase == UFS_LINK_READY) {
            if (a->prepare_only) { a->state = UfsPrepared; return FALSE; }
            if (UfsIoStart(&a->io)) { a->state = UfsOnline; return FALSE; }
        }
        /* A timed-out management request may still DMA into its common
         * buffer. Leave it owned by the adapter until it really stops.
         */
    }
    a->state = UfsFault;
    return FALSE;
}

static BOOLEAN initialize(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    if (a->state != UfsMapped) return FALSE;
    a->state = UfsStarting;
    return StorPortEnablePassiveInitialization(a, initialize_passive);
}

static BOOLEAN initialize_passive(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    ULONGLONG now;
    for (;;) {
        if (!clock_us(a, &now)) break;
        if (a->state != UfsStarting) return FALSE;
        if (a->platform.extension && a->platform.phy.phase != UFS_PLATFORM_READY) {
            enum ufs_platform_phase phase = ufs_platform_poll(&a->platform.phy, now);
            if (phase == UFS_PLATFORM_FAILED || phase == UFS_PLATFORM_STOPPING) break;
        } else if (!StorPortSynchronizeAccess(a, start_step, &now)) {
            if (a->state == UfsOnline) {
                if (a->link_power_ready)
                    a->link_power_ready(a->link_power_context,
                        a->link.parameter_count == UfsPaParameterCount ? a->link.parameter : NULL);
                return TRUE;
            }
            break;
        }
        if (StorPortDelayExecution(a, START_POLL_US) != STOR_STATUS_SUCCESS) break;
    }
    a->state = UfsFault;
    if (!a->io.managing && !a->io.running && !a->io.owns_list && !a->io.active) {
        if (a->platform.extension) {
            if (clock_us(a, &now)) (void)UfsPlatformRelease(&a->platform, now);
        } else {
            UfsIoRelease(&a->io);
        }
    }
    return FALSE;
}

static void complete_power(UFS_ADAPTER *a, PVOID srb, UCHAR status)
{
    SrbSetSrbStatus(srb, status);
    StorPortNotification(RequestComplete, a, srb);
}

static void finish_power(UFS_ADAPTER *a, UCHAR status)
{
    PVOID srb = a->power_srb;
    a->power_srb = NULL;
    if (srb) complete_power(a, srb, status);
}

static PVOID request_data(PSTORAGE_REQUEST_BLOCK ex, SIZE_T size, ULONG length)
{
    PSRBEX_DATA p = SrbGetPrimarySrbExData(ex);
    if (!p) return NULL;
    SIZE_T offset = (SIZE_T)((PUCHAR)p - (PUCHAR)ex);
    return offset <= ex->SrbLength && size <= ex->SrbLength - offset && p->Length == length ? p : NULL;
}

static BOOLEAN power_request(UFS_ADAPTER *a, PSCSI_REQUEST_BLOCK srb, BOOLEAN pnp)
{
    STOR_DEVICE_POWER_STATE state = StorPowerDeviceD3;
    enum ufs_link_operation down = UFS_POWER_OFF;
    UCHAR status = SRB_STATUS_INVALID_REQUEST;
    BOOLEAN extended = srb->Function == SRB_FUNCTION_STORAGE_REQUEST_BLOCK;
    if (pnp) {
        ULONG flags;
        STOR_PNP_ACTION action;
        if (extended) {
            PSRBEX_DATA_PNP p = request_data((PSTORAGE_REQUEST_BLOCK)srb, sizeof(*p), SRBEX_DATA_PNP_LENGTH);
            if (!p) goto done;
            flags = p->SrbPnPFlags;
            action = p->PnPAction;
        } else {
            PSCSI_PNP_REQUEST_BLOCK p = (PSCSI_PNP_REQUEST_BLOCK)srb;
            if (p->Length < RTL_SIZEOF_THROUGH_FIELD(SCSI_PNP_REQUEST_BLOCK, SrbPnPFlags)) goto done;
            flags = p->SrbPnPFlags;
            action = p->PnPAction;
        }
        if (flags != SRB_PNP_FLAGS_ADAPTER_REQUEST ||
            (action != StorStopDevice && action != StorRemoveDevice)) goto done;
        if (a->state == UfsAbsent) { status = SRB_STATUS_SUCCESS; goto done; }
    } else {
        UCHAR flags;
        STOR_POWER_ACTION action;
        if (extended) {
            PSRBEX_DATA_POWER p = request_data((PSTORAGE_REQUEST_BLOCK)srb, sizeof(*p), SRBEX_DATA_POWER_LENGTH);
            if (!p) goto done;
            flags = p->SrbPowerFlags;
            state = p->DevicePowerState;
            action = p->PowerAction;
        } else {
            PSCSI_POWER_REQUEST_BLOCK p = (PSCSI_POWER_REQUEST_BLOCK)srb;
            if (p->Length < RTL_SIZEOF_THROUGH_FIELD(SCSI_POWER_REQUEST_BLOCK, PowerAction)) goto done;
            flags = p->SrbPowerFlags;
            state = p->DevicePowerState;
            action = p->PowerAction;
        }
        if (state != StorPowerDeviceD0 && state != StorPowerDeviceD3) goto done;
        /* A normal PnP instance cannot write or restore a hiberfile in dump mode. */
        if (state == StorPowerDeviceD3 && action == StorPowerActionHibernate) goto done;
        if (state == StorPowerDeviceD3 && action == StorPowerActionNone && a->can_retain_s0 &&
            a->can_retain_s0(a->link_power_context)) down = UFS_SLEEP;
        if (!flags) {
            /* One LU's D-state is not permission to power down the others. */
            if (a->state == UfsOnline) return UfsIoPowerUnit(&a->io, srb, state);
            status = SRB_STATUS_BUSY;
            goto done;
        }
        if (flags != SRB_POWER_FLAGS_ADAPTER_REQUEST) goto done;
    }
    if (a->power_srb || a->start_timer) { status = SRB_STATUS_BUSY; goto done; }
    if ((state == StorPowerDeviceD0 && a->state == UfsOnline) ||
        (state == StorPowerDeviceD3 && ((a->state == UfsStopped && a->media_off) ||
         (a->state == UfsAsleep && down == UFS_SLEEP)))) {
        status = SRB_STATUS_SUCCESS;
        goto done;
    }
    if (state == StorPowerDeviceD0 && a->state == UfsPrepared) {
        if (a->prepare_only) { status = SRB_STATUS_BUSY; goto done; }
        if (UfsIoStart(&a->io)) { a->state = UfsOnline; status = SRB_STATUS_SUCCESS; }
        else { a->state = UfsFault; status = SRB_STATUS_ERROR; }
        goto done;
    }
    if ((state == StorPowerDeviceD0 && ((a->state != UfsStopped && a->state != UfsAsleep) ||
         a->platform.phy.phase != UFS_PLATFORM_READY)) ||
        (state == StorPowerDeviceD3 && a->state != UfsOnline) || !clock_us(a, &a->power_since)) {
        status = SRB_STATUS_ERROR;
        goto done;
    }
    InterlockedExchange(&a->media_off, 0);
    InterlockedIncrement(&a->sequence);
    a->power_operation = state == StorPowerDeviceD3 ? down : a->state == UfsAsleep ? UFS_WAKE : UFS_DISCOVER;
    a->prepare_only = (state == StorPowerDeviceD0);
    a->power_touched = FALSE;
    a->retire_since = 0;
    a->worker_since = 0;
    a->state = state == StorPowerDeviceD3 ? UfsPoweringDown : UfsStarting;
    a->power_timeout = (ULONGLONG)SrbGetTimeOutValue(srb) * 1000000;
    if (!a->power_timeout) a->power_timeout = 30000000;
    a->power_srb = srb;
    a->start_timer = TRUE;
    SrbSetSrbStatus(srb, SRB_STATUS_PENDING);
    StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
    return TRUE;
done:
    complete_power(a, srb, status);
    return TRUE;
}

static BOOLEAN start_io(PVOID extension, PSCSI_REQUEST_BLOCK srb)
{
    UFS_ADAPTER *a = extension;
    if (a->platform.extension) {
        ULONG function = SrbGetSrbFunction(srb);
        if (function == SRB_FUNCTION_POWER || function == SRB_FUNCTION_PNP)
            return power_request(a, srb, function == SRB_FUNCTION_PNP);
        /* Storport can issue its final FLUSH after the POWER SRB and before
         * StopAdapter. No writes were admitted since every LU was flushed. */
        if (((a->media_off && a->state == UfsStopped) || a->state == UfsAsleep) &&
            (function == SRB_FUNCTION_FLUSH || function == SRB_FUNCTION_SHUTDOWN) &&
            !SrbGetPathId(srb) && !SrbGetTargetId(srb) && SrbGetLun(srb) < UFS_MAX_UNIT_NUMBERS &&
            (a->link.enabled_luns & (1u << SrbGetLun(srb)))) {
            complete_power(a, srb, SRB_STATUS_SUCCESS);
            return TRUE;
        }
    }
    if (a->state == UfsOnline) return UfsIoStartRequest(&a->io, srb);
    SrbSetSrbStatus(srb, a->state == UfsStarting || a->state == UfsPoweringDown ? SRB_STATUS_BUSY : SRB_STATUS_NO_HBA);
    if (SrbGetSrbFunction(srb) == SRB_FUNCTION_EXECUTE_SCSI) SrbSetDataTransferLength(srb, 0);
    StorPortNotification(RequestComplete, a, srb);
    return TRUE;
}

static BOOLEAN interrupt(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    return a->state != UfsAbsent && a->io.running && !a->io.resetting && UfsIoInterrupt(&a->io) != 0;
}

static VOID schedule_restart(UFS_ADAPTER *a)
{
    a->state = UfsStarting;
    a->power_operation = UFS_DISCOVER;
    a->prepare_only = a->platform.extension != NULL;
    InterlockedIncrement(&a->sequence);
    a->start_timer = TRUE;
    a->retire_since = 0;
    a->worker_since = 0;
    StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
}

static BOOLEAN power_down_step(UFS_ADAPTER *a, ULONGLONG now)
{
    if (!a->io.managing) {
        if (a->io.faults) goto failed;
        if (a->io.active) return TRUE; /* The ISR still completes ordinary SRBs. */
        a->power_touched = TRUE;
        if (UfsIoStop(&a->io) && UfsLinkBegin(&a->io, &a->link, now, &a->media, a->power_operation) == UFS_LINK_OK)
            return TRUE;
    } else {
        enum ufs_link_phase phase = UfsLinkPoll(&a->link, now);
        if (phase != UFS_LINK_FAILED && phase != UFS_LINK_OFF && phase != UFS_LINK_ASLEEP &&
            phase != UFS_LINK_KEEP_POWER) return TRUE;
        if (UfsLinkStop(&a->io, &a->link)) {
            if (phase == UFS_LINK_OFF) {
                a->state = UfsStopped;
                InterlockedExchange(&a->media_off, 1);
                return FALSE;
            }
            if (phase == UFS_LINK_ASLEEP) { a->state = UfsAsleep; return FALSE; }
            if (a->link.phase == UFS_LINK_KEEP_POWER && UfsIoStart(&a->io)) {
                a->state = UfsOnline;
                return FALSE;
            }
        }
    }
failed:
    a->state = UfsRecovering;
    return FALSE;
}

static BOOLEAN reset_hardware(UFS_ADAPTER *a)
{
    if (a->io.managing && !UfsLinkReset(&a->io, &a->link)) return FALSE;
    return UfsIoReset(&a->io);
}

static BOOLEAN timed_step(PVOID extension, PVOID unused)
{
    UFS_ADAPTER *a = extension;
    ULONGLONG now;
    BOOLEAN power_down = a->state == UfsPoweringDown;
    UNREFERENCED_PARAMETER(unused);
    if (!a->start_timer) return FALSE;
    if (a->state == UfsStarting && a->platform.extension) {
        LONG sequence = a->sequence;
        return retry_restore(a, &sequence);
    }
    if (!clock_us(a, &now) || (a->power_srb && now - a->power_since >= a->power_timeout)) {
        if (power_down && !a->power_touched && a->io.running && !a->io.faults) a->state = UfsOnline;
        else a->state = UfsRecovering;
    } else if (a->state == UfsRecovering) {
        if (reset_hardware(a)) { schedule_restart(a); return TRUE; }
        if (now - a->retire_since < RETIRE_TIMEOUT_US) {
            StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
            return TRUE;
        }
        a->state = UfsFault; /* Storport can issue another reset; retain all outstanding DMA. */
    } else if ((a->state == UfsStarting && !a->platform.extension && start_step(a, &now)) ||
               (a->state == UfsPoweringDown && power_down_step(a, now))) {
        StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
        return TRUE;
    }
    if (a->state == UfsRecovering && a->power_srb) {
        a->retire_since = a->power_since;
        (void)clock_us(a, &a->retire_since);
        finish_power(a, SRB_STATUS_ERROR);
        StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
        return TRUE;
    }
    if (a->state == UfsRecovering) a->state = UfsFault;
    a->start_timer = FALSE;
    BOOLEAN reached = power_down ? ((a->state == UfsStopped && a->media_off) || a->state == UfsAsleep)
                                 : (a->state == UfsOnline);
    finish_power(a, reached ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR);
    return FALSE;
}

static VOID poll_restart(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    if (a->platform.extension && a->state == UfsStarting && a->start_timer) {
        LONG sequence = a->sequence;
        if (InterlockedCompareExchange(&a->platform.worker_active, 1, 0)) {
            StorPortSynchronizeAccess(a, retry_restore, &sequence);
            return;
        }
        ULONG status = StorPortQueueWorkItem(a, restore_worker, a->platform.worker, (PVOID)(ULONG_PTR)(ULONG)sequence);
        if (status == STOR_STATUS_SUCCESS) return;
        InterlockedExchange(&a->platform.worker_active, 0);
        if (status == STOR_STATUS_BUSY) {
            StorPortSynchronizeAccess(a, retry_restore, &sequence);
            return;
        }
        /* Removal or resource failure is not a successful D0. Never replace
         * the PASSIVE-level power calls by register accesses from this timer. */
        StorPortSynchronizeAccess(a, fail_restore, &sequence);
        return;
    }
    StorPortSynchronizeAccess(a, timed_step, NULL);
}

static BOOLEAN synchronized_reset(PVOID extension, PVOID unused)
{
    UFS_ADAPTER *a = extension;
    UNREFERENCED_PARAMETER(unused);
    if ((a->state != UfsOnline && a->state != UfsFault && a->state != UfsStarting &&
         a->state != UfsPoweringDown && a->state != UfsRecovering && a->state != UfsPrepared) || !a->io.registers)
        return FALSE;
    a->state = UfsFault;
    InterlockedIncrement(&a->sequence);
    a->start_timer = FALSE;
    finish_power(a, SRB_STATUS_BUS_RESET); /* The management DMA uses only the adapter's own common buffer. */
    a->needs_reset = TRUE;
    if (!reset_hardware(a)) return FALSE;
    a->needs_reset = FALSE;
    schedule_restart(a);
    return TRUE;
}

static BOOLEAN reset_bus(PVOID extension, ULONG path)
{
    return path == 0 && StorPortSynchronizeAccess(extension, synchronized_reset, NULL);
}

static BOOLEAN surprise_removed(PVOID extension, PVOID context)
{
    UFS_ADAPTER *a = extension;
    UNREFERENCED_PARAMETER(context);
    a->state = UfsAbsent;
    InterlockedIncrement(&a->sequence);
    a->start_timer = FALSE;
    InterlockedExchange(&a->media_off, 0);
    finish_power(a, SRB_STATUS_NO_HBA);
    return TRUE;
}

struct restore_context {
    LONG sequence;
    BOOLEAN prepared, have_parameters, activate;
    uint8_t parameters[UfsPaParameterCount];
};

static BOOLEAN restore_step(PVOID extension, PVOID opaque)
{
    UFS_ADAPTER *a = extension;
    struct restore_context *c = opaque;
    ULONGLONG now;
    if (a->sequence != c->sequence || a->state != UfsStarting || !clock_us(a, &now) ||
        (a->power_srb && now - a->power_since >= a->power_timeout)) return FALSE;
    a->power_touched = TRUE;
    if (start_step(a, &now)) return TRUE;
    if (a->state == UfsPrepared) {
        c->prepared = TRUE;
        c->have_parameters = a->link.parameter_count == UfsPaParameterCount;
        if (c->have_parameters) RtlCopyMemory(c->parameters, a->link.parameter, sizeof(c->parameters));
    }
    return FALSE;
}

static BOOLEAN fail_restore(PVOID extension, PVOID opaque)
{
    UFS_ADAPTER *a = extension;
    if (a->sequence != *(LONG *)opaque ||
        (a->state != UfsStarting && a->state != UfsPrepared && a->state != UfsFault)) return FALSE;
    if (a->power_srb && !a->power_touched) {
        a->state = a->power_operation == UFS_WAKE ? UfsAsleep : UfsStopped;
    } else if (a->power_srb && clock_us(a, &a->retire_since)) {
        a->state = UfsRecovering;
    } else {
        a->state = UfsFault;
    }
    a->start_timer = a->state == UfsRecovering;
    a->prepare_only = FALSE;
    finish_power(a, SRB_STATUS_ERROR);
    if (a->start_timer) StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
    return FALSE;
}

static BOOLEAN retry_restore(PVOID extension, PVOID opaque)
{
    UFS_ADAPTER *a = extension;
    ULONGLONG now;
    if (a->sequence != *(LONG *)opaque || a->state != UfsStarting || !a->start_timer) return FALSE;
    if (!clock_us(a, &now)) return fail_restore(a, opaque);
    if (!a->worker_since) a->worker_since = now;
    if (a->power_srb ? now - a->power_since >= a->power_timeout : now - a->worker_since >= RETIRE_TIMEOUT_US)
        return fail_restore(a, opaque);
    StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
    return TRUE;
}

static BOOLEAN publish_restore(PVOID extension, PVOID opaque)
{
    UFS_ADAPTER *a = extension;
    struct restore_context *c = opaque;
    if (a->sequence != c->sequence || a->state != UfsPrepared) return FALSE;
    if (c->activate) {
        if (!UfsIoStart(&a->io)) return fail_restore(a, &c->sequence);
        a->state = UfsOnline;
    }
    a->prepare_only = a->start_timer = FALSE;
    finish_power(a, SRB_STATUS_SUCCESS);
    return TRUE;
}

/* The worker holds the platform references while it can touch the provider.
 * Its sequence prevents an old D0 from starting I/O after reset or removal. */
static BOOLEAN restore_passive(UFS_ADAPTER *a, LONG sequence, BOOLEAN disconnected)
{
    struct restore_context c = { .sequence = sequence, .activate = !disconnected };
    if (sequence != a->sequence || a->state != UfsStarting) return FALSE;
    if (!a->link_power_begin || a->link_power_begin(a->link_power_context)) {
        while (disconnected ? restore_step(a, &c) : StorPortSynchronizeAccess(a, restore_step, &c)) {
            if (StorPortDelayExecution(a, START_POLL_US) != STOR_STATUS_SUCCESS) break;
        }
    }
    if (!c.prepared || a->sequence != sequence) {
        return disconnected ? fail_restore(a, &sequence) : StorPortSynchronizeAccess(a, fail_restore, &sequence);
    }
    if (a->link_power_ready)
        a->link_power_ready(a->link_power_context, c.have_parameters ? c.parameters : NULL);
    return disconnected ? publish_restore(a, &c) : StorPortSynchronizeAccess(a, publish_restore, &c);
}

static VOID restore_worker(PVOID extension, PVOID context, PVOID worker)
{
    UFS_ADAPTER *a = extension;
    UNREFERENCED_PARAMETER(worker);
    (void)restore_passive(a, (LONG)(ULONG_PTR)context, FALSE);
    InterlockedExchange(&a->platform.worker_active, 0);
}

/* Storport calls this before RestartAdapter, with the interrupt disconnected. */
static BOOLEAN prepare_restart(UFS_ADAPTER *a)
{
    if (a->state == UfsOnline || (a->state == UfsPrepared && !a->prepare_only)) return TRUE;
    if ((a->state != UfsStopped && a->state != UfsAsleep) || a->power_srb || a->start_timer ||
        a->platform.worker_active || !a->io.registers || a->platform.phy.phase != UFS_PLATFORM_READY ||
        StorPortReadRegisterUlong(a, (PULONG)(a->io.registers + HCI_INTERRUPT_ENABLE))) return FALSE;
    a->power_operation = a->state == UfsAsleep ? UFS_WAKE : UFS_DISCOVER;
    InterlockedExchange(&a->media_off, 0);
    a->state = UfsStarting;
    a->prepare_only = TRUE;
    a->power_touched = FALSE;
    a->retire_since = 0;
    return restore_passive(a, InterlockedIncrement(&a->sequence), TRUE);
}

static SCSI_ADAPTER_CONTROL_STATUS adapter_control(PVOID extension, SCSI_ADAPTER_CONTROL_TYPE type, PVOID parameters)
{
    UFS_ADAPTER *a = extension;
    if (type == ScsiQuerySupportedControlTypes) {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list = parameters;
        const SCSI_ADAPTER_CONTROL_TYPE ops[] = { ScsiQuerySupportedControlTypes, ScsiStopAdapter,
                                                   ScsiRestartAdapter, ScsiAdapterSurpriseRemoval };
        for (unsigned i = 0; i < RTL_NUMBER_OF(ops); ++i)
            if ((ULONG)ops[i] < list->MaxControlType) list->SupportedTypeList[ops[i]] = TRUE;
        if (a->platform.extension && (ULONG)ScsiSetRunningConfig < list->MaxControlType)
            list->SupportedTypeList[ScsiSetRunningConfig] = TRUE;
        return ScsiAdapterControlSuccess;
    }
    if (type == ScsiSetRunningConfig && a->platform.extension)
        return prepare_restart(a) ? ScsiAdapterControlSuccess : ScsiAdapterControlUnsuccessful;
    if (type == ScsiStopAdapter) {
        if (a->state == UfsStopped || a->state == UfsAsleep || a->state == UfsAbsent) return ScsiAdapterControlSuccess;
        /* The board cannot flush or drop RPMh votes at DIRQL. A preceding
         * POWER or PnP stop SRB must already have finished with hardware. */
        if (a->platform.extension) return ScsiAdapterControlUnsuccessful;
        if (a->io.managing && !UfsLinkStop(&a->io, &a->link)) return ScsiAdapterControlUnsuccessful;
        if (!UfsIoStop(&a->io)) return ScsiAdapterControlUnsuccessful;
        if (a->io.registers) {
            PULONG ie = (PULONG)(a->io.registers + HCI_INTERRUPT_ENABLE);
            StorPortWriteRegisterUlong(a, ie, 0);
            if (StorPortReadRegisterUlong(a, ie)) return ScsiAdapterControlUnsuccessful;
            PULONG hce = (PULONG)(a->io.registers + HCI_ENABLE);
            StorPortWriteRegisterUlong(a, hce, 0);
            if (StorPortReadRegisterUlong(a, hce) & 1) return ScsiAdapterControlUnsuccessful;
        }
        if (a->start_timer) StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)0);
        a->start_timer = FALSE;
        a->state = UfsStopped;
        return ScsiAdapterControlSuccess;
    }
    if (type == ScsiRestartAdapter && a->platform.extension) {
        if (a->state == UfsPrepared && !a->prepare_only && UfsIoStart(&a->io)) a->state = UfsOnline;
        return a->state == UfsOnline ? ScsiAdapterControlSuccess : ScsiAdapterControlUnsuccessful;
    }
    if (type == ScsiRestartAdapter && a->state == UfsStopped && a->io.registers) {
        if (a->needs_reset && !UfsIoReset(&a->io)) return ScsiAdapterControlUnsuccessful;
        a->needs_reset = FALSE;
        schedule_restart(a);
        return ScsiAdapterControlSuccess;
    }
    if (type == ScsiAdapterSurpriseRemoval) {
        /* No register is safe to touch after surprise removal. Storport and
         * PnP retire the bus mappings; an attempted peripheral reset here
         * could instead hit the next device occupying the same PCI address.
         */
        StorPortSynchronizeAccess(a, surprise_removed, NULL);
        return ScsiAdapterControlSuccess;
    }
    return ScsiAdapterControlUnsuccessful;
}

NTSTATUS UfsMiniportRegister(PDRIVER_OBJECT driver, PUNICODE_STRING registry, INTERFACE_TYPE bus,
                             PHW_FIND_ADAPTER discover, ULONG ranges)
{
    if (!discover || !ranges) return STATUS_INVALID_PARAMETER;
    HW_INITIALIZATION_DATA init = {0};
    init.HwInitializationDataSize = sizeof(init);
    init.AdapterInterfaceType = bus;
    init.DeviceExtensionSize = sizeof(UFS_ADAPTER);
    init.NumberOfAccessRanges = ranges;
    init.HwFindAdapter = discover;
    init.HwInitialize = initialize;
    init.HwBuildIo = UfsIoBuildRequest;
    init.HwStartIo = start_io;
    init.HwInterrupt = interrupt;
    init.HwResetBus = reset_bus;
    init.HwAdapterControl = adapter_control;
    init.NeedPhysicalAddresses = init.TaggedQueuing = init.MultipleRequestPerLu = init.AutoRequestSense = TRUE;
    init.SrbTypeFlags = SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK | SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK;
    init.AddressTypeFlags = ADDRESS_TYPE_FLAG_BTL8;
    return (NTSTATUS)StorPortInitialize(driver, registry, &init, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    return UfsMiniportRegister(driver, registry, PCIBus, find_pci, 1);
}
