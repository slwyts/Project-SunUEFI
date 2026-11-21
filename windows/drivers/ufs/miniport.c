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
static STOR_SYNCHRONIZED_ACCESS start_step;
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
        if (UfsLinkBegin(&a->io, &a->link, now) == UFS_LINK_OK) {
            a->link.read_parameters = (uint8_t)(a->link_power_ready != NULL && !a->start_timer);
            return TRUE;
        }
    } else {
        enum ufs_link_phase phase = UfsLinkPoll(&a->link, now);
        if (phase != UFS_LINK_READY && phase != UFS_LINK_FAILED) return TRUE;
        if (!UfsLinkStop(&a->io, &a->link)) {
            if (!a->retire_since) a->retire_since = now;
            if (now - a->retire_since < RETIRE_TIMEOUT_US) return TRUE;
            a->needs_reset = TRUE;
        } else if (phase == UFS_LINK_READY && UfsIoStart(&a->io)) {
            a->state = UfsOnline;
            return FALSE;
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

static BOOLEAN start_io(PVOID extension, PSCSI_REQUEST_BLOCK srb)
{
    UFS_ADAPTER *a = extension;
    if (a->state == UfsOnline) return UfsIoStartRequest(&a->io, srb);
    SrbSetSrbStatus(srb, a->state == UfsStarting ? SRB_STATUS_BUSY : SRB_STATUS_NO_HBA);
    if (SrbGetSrbFunction(srb) == SRB_FUNCTION_EXECUTE_SCSI) SrbSetDataTransferLength(srb, 0);
    StorPortNotification(RequestComplete, a, srb);
    return TRUE;
}

static BOOLEAN interrupt(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    return a->state == UfsOnline && UfsIoInterrupt(&a->io) != 0;
}

static VOID schedule_restart(UFS_ADAPTER *a)
{
    a->state = UfsStarting;
    a->start_timer = TRUE;
    a->retire_since = 0;
    StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
}

static VOID poll_restart(PVOID extension)
{
    UFS_ADAPTER *a = extension;
    ULONGLONG now;
    if (!a->start_timer || a->state != UfsStarting) return;
    if (!clock_us(a, &now)) { a->state = UfsFault; a->start_timer = FALSE; return; }
    if (StorPortSynchronizeAccess(a, start_step, &now)) {
        StorPortNotification(RequestTimerCall, a, poll_restart, (ULONG)START_POLL_US);
    } else {
        a->start_timer = FALSE;
    }
}

static BOOLEAN synchronized_reset(PVOID extension, PVOID unused)
{
    UFS_ADAPTER *a = extension;
    UNREFERENCED_PARAMETER(unused);
    if ((a->state != UfsOnline && a->state != UfsFault && a->state != UfsStarting) || !a->io.registers)
        return FALSE;
    a->state = UfsFault;
    if (a->io.managing && !UfsLinkStop(&a->io, &a->link)) { a->needs_reset = TRUE; return FALSE; }
    a->needs_reset = TRUE;
    if (!UfsIoReset(&a->io)) return FALSE;
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
    a->start_timer = FALSE;
    return TRUE;
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
        return ScsiAdapterControlSuccess;
    }
    if (type == ScsiStopAdapter) {
        if (a->state == UfsStopped || a->state == UfsAbsent) return ScsiAdapterControlSuccess;
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
    if (type == ScsiRestartAdapter && a->state == UfsStopped && a->io.registers &&
        (!a->platform.extension || a->platform.phy.phase == UFS_PLATFORM_READY)) {
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
