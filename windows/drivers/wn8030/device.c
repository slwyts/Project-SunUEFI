/* SPDX-License-Identifier: BSD-2-Clause */
#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#include "input.h"

/* GPIO connections in _CRS: reset, sleep. Both are electrically active low.
 * ACPI owns the I/O and digital regulators; this driver owns only the pins
 * and bus resources given to this device by the PnP manager. */
enum { pin_reset, pin_sleep, pin_count, keepalive_ms = 12000 };
enum cover_phase { cover_asleep, cover_starting, cover_ready };

struct device {
    HANDLE pins[pin_count];
    WN8030_INPUT *input;
    WDFTIMER keepalive;
    KMUTEX lock;
    KEVENT wake_irq;
    ULONGLONG irq_time;
    enum cover_phase phase;
    BOOLEAN cold, attached;
};
typedef struct device COVER_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(COVER_DEVICE, cover_device)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD device_add;
static EVT_WDF_DEVICE_PREPARE_HARDWARE prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE release;
static EVT_WDF_DEVICE_D0_ENTRY power_on;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED wake;
static EVT_WDF_DEVICE_D0_EXIT sleep;
static EVT_WDF_INTERRUPT_ISR handle_irq;
static EVT_WDF_TIMER keepalive;

static void acquire(struct device *d)
{
    (void)KeWaitForSingleObject(&d->lock, Executive, KernelMode, FALSE, NULL);
}

static void delay_ms(LONG ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)ms * 10000;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static NTSTATUS open_pin(const CM_PARTIAL_RESOURCE_DESCRIPTOR *r, HANDLE *pin)
{
    WCHAR path[RESOURCE_HUB_PATH_CHARS];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    RtlInitEmptyUnicodeString(&name, path, sizeof(path));
    NTSTATUS status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, r->u.Connection.IdLowPart,
                                                       r->u.Connection.IdHighPart);
    if (!NT_SUCCESS(status)) return status;
    InitializeObjectAttributes(&attr, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwOpenFile(pin, GENERIC_WRITE | SYNCHRONIZE, &attr, &io, 0,
                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS set_pin(HANDLE pin, UCHAR high)
{
    IO_STATUS_BLOCK io;
    return ZwDeviceIoControlFile(pin, NULL, NULL, NULL, &io, IOCTL_GPIO_WRITE_PINS,
                                 &high, sizeof(high), NULL, 0);
}

static NTSTATUS reset_mcu(struct device *d)
{
    NTSTATUS status = set_pin(d->pins[pin_sleep], 0);
    if (NT_SUCCESS(status)) status = set_pin(d->pins[pin_reset], 0);
    if (NT_SUCCESS(status)) {
        delay_ms(2);
        status = set_pin(d->pins[pin_reset], 1);
        if (NT_SUCCESS(status)) delay_ms(500);
    }
    return status;
}

static void close_resources(struct device *d)
{
    if (d->input) {
        Wn8030InputDelete(d->input);
        d->input = NULL;
    }
    for (unsigned int i = 0; i < pin_count; ++i) {
        if (d->pins[i]) {
            ZwClose(d->pins[i]);
            d->pins[i] = NULL;
        }
    }
}

static NTSTATUS prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    struct device *d = cover_device(device);
    UNREFERENCED_PARAMETER(raw);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR i2c = NULL, irq = NULL;
    ULONG pins = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (r->Type == CmResourceTypeInterrupt) {
            if (irq || r->ShareDisposition == CmResourceShareShared ||
                (r->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
            }
            irq = r;
        } else if (r->Type == CmResourceTypeConnection) {
            if (r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
                if (i2c) { status = STATUS_DEVICE_CONFIGURATION_ERROR; break; }
                i2c = r;
            } else if (r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
                       r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
                if (pins == pin_count) { status = STATUS_DEVICE_CONFIGURATION_ERROR; break; }
                status = open_pin(r, &d->pins[pins++]);
                if (!NT_SUCCESS(status)) break;
            }
        }
    }
    if (NT_SUCCESS(status) && (!i2c || !irq || pins != pin_count))
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) status = Wn8030InputCreate(WdfDeviceWdmGetDeviceObject(device), i2c, &d->input);
    /* KMDF calls ReleaseHardware even after a partial PrepareHardware. */
    d->cold = TRUE;
    return status;
}

static NTSTATUS release(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    struct device *d = cover_device(device);
    (void)WdfTimerStop(d->keepalive, TRUE);
    close_resources(d);
    return STATUS_SUCCESS;
}

/* Called with d->lock held. Do not postpone an active timer on each key press. */
static void set_attached(struct device *d, BOOLEAN attached)
{
    if (attached == d->attached) return;
    d->attached = attached;
    if (attached)
        (void)WdfTimerStart(d->keepalive, WDF_REL_TIMEOUT_IN_MS(keepalive_ms));
    else
        (void)WdfTimerStop(d->keepalive, FALSE);
}

static VOID keepalive(WDFTIMER timer)
{
    struct device *d = cover_device((WDFDEVICE)WdfTimerGetParentObject(timer));
    acquire(d);
    if (d->phase == cover_ready && d->attached) {
        (void)Wn8030CoverKeepAlive(d->input);
        (void)WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(keepalive_ms));
    }
    KeReleaseMutex(&d->lock, FALSE);
}

static BOOLEAN handle_irq(WDFINTERRUPT interrupt, ULONG id)
{
    UNREFERENCED_PARAMETER(id);
    ULONGLONG stamp = KeQueryInterruptTime();
    struct device *d = cover_device(WdfInterruptGetDevice(interrupt));
    acquire(d);
    if (d->phase == cover_starting) {
        d->irq_time = stamp;
        KeSetEvent(&d->wake_irq, IO_NO_INCREMENT, FALSE);
    } else if (d->phase == cover_ready) {
        BOOLEAN attached;
        (void)Wn8030InputRead(d->input, stamp, &attached);
        set_attached(d, attached);
    }
    KeReleaseMutex(&d->lock, FALSE);
    return TRUE; /* _CRS gives the device an exclusive data-ready interrupt. */
}

static NTSTATUS power_on(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    struct device *d = cover_device(device);
    if (previous == WdfPowerDeviceD3Final) d->cold = TRUE;
    KeClearEvent(&d->wake_irq);
    d->phase = cover_starting; /* Interrupts are not enabled yet. */
    NTSTATUS status = d->cold ? reset_mcu(d) : STATUS_SUCCESS;
    if (!NT_SUCCESS(status)) {
        d->phase = cover_asleep;
        (void)set_pin(d->pins[pin_sleep], 0);
        (void)set_pin(d->pins[pin_reset], 0);
    }
    return status;
}

static NTSTATUS wake(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    struct device *d = cover_device(device);
    NTSTATUS status = set_pin(d->pins[pin_sleep], 1);
    if (NT_SUCCESS(status) && !d->cold) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -25 * 10000LL;
        /* An IRQ is the stock resume acknowledgement. Never hold the device
         * mutex while waiting for its passive ISR. */
        if (KeWaitForSingleObject(&d->wake_irq, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
            d->cold = TRUE;
            status = reset_mcu(d);
            if (NT_SUCCESS(status)) status = set_pin(d->pins[pin_sleep], 1);
        }
    }
    if (NT_SUCCESS(status) && d->cold) delay_ms(50);
    acquire(d);
    BOOLEAN attached = FALSE;
    if (NT_SUCCESS(status)) status = Wn8030InputWake(d->input, d->cold, &attached);
    if (NT_SUCCESS(status)) {
        d->phase = cover_ready;
        d->cold = FALSE;
        /* Piano may combine the wake indication with the first report. */
        if (KeReadStateEvent(&d->wake_irq)) (void)Wn8030InputRead(d->input, d->irq_time, &attached);
        set_attached(d, attached);
    } else {
        d->phase = cover_asleep;
        d->cold = TRUE;
        (void)set_pin(d->pins[pin_sleep], 0);
        (void)set_pin(d->pins[pin_reset], 0);
    }
    KeReleaseMutex(&d->lock, FALSE);
    return status;
}

static NTSTATUS sleep(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    struct device *d = cover_device(device);
    acquire(d);
    d->phase = cover_asleep; /* Framework has disabled the IRQ. */
    d->attached = FALSE;
    NTSTATUS status = Wn8030InputSleep(d->input);
    KeReleaseMutex(&d->lock, FALSE);
    (void)WdfTimerStop(d->keepalive, TRUE);
    NTSTATUS pin = set_pin(d->pins[pin_sleep], 0);
    if (NT_SUCCESS(status)) status = pin;
    if (target == WdfPowerDeviceD3Final) {
        pin = set_pin(d->pins[pin_reset], 0);
        if (NT_SUCCESS(status)) status = pin;
        d->cold = TRUE;
    }
    delay_ms(10);
    return status;
}

static NTSTATUS device_add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = prepare;
    callbacks.EvtDeviceReleaseHardware = release;
    callbacks.EvtDeviceD0Entry = power_on;
    callbacks.EvtDeviceD0EntryPostInterruptsEnabled = wake;
    callbacks.EvtDeviceD0Exit = sleep;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, COVER_DEVICE);
    attr.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) return status;
    struct device *d = cover_device(device);
    KeInitializeMutex(&d->lock, 0);
    KeInitializeEvent(&d->wake_irq, NotificationEvent, FALSE);

    /* The one exclusive GPIO IRQ follows the device across resource
     * rebalancing. Its passive ISR can read a complete frame over I2C. */
    WDF_INTERRUPT_CONFIG irq_config;
    WDFINTERRUPT irq;
    WDF_INTERRUPT_CONFIG_INIT(&irq_config, handle_irq, NULL);
    irq_config.PassiveHandling = TRUE;
    irq_config.ShareVector = WdfFalse;
    status = WdfInterruptCreate(device, &irq_config, WDF_NO_OBJECT_ATTRIBUTES, &irq);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG config;
    WDF_TIMER_CONFIG_INIT(&config, keepalive);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&config, &attr, &d->keepalive);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, device_add);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
