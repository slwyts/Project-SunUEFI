/* SPDX-License-Identifier: BSD-2-Clause */
#include <ntddk.h>
#include "button_gpio.h"

static GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION query_info;
static GPIO_CLIENT_ENABLE_INTERRUPT enable;
static GPIO_CLIENT_DISABLE_INTERRUPT disable;
static GPIO_CLIENT_UNMASK_INTERRUPT unmask;
static GPIO_CLIENT_MASK_INTERRUPTS mask;
static GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS query_enabled;
static GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS query_active;
static GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS clear_active;
static GPIO_CLIENT_RECONFIGURE_INTERRUPT reconfigure;

static NTSTATUS nt_status(enum spmi_status status)
{
    switch (status) {
    case SPMI_OK: return STATUS_SUCCESS;
    case SPMI_INVALID: return STATUS_INVALID_PARAMETER;
    case SPMI_NOT_FOUND: return STATUS_NOT_FOUND;
    case SPMI_NOT_OWNER: case SPMI_DENIED: return STATUS_ACCESS_DENIED;
    case SPMI_BUSY: return STATUS_DEVICE_BUSY;
    case SPMI_TIMEOUT: return STATUS_IO_TIMEOUT;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

NTSTATUS SpmiButtonGpioPrepare(SPMI_BUTTON_GPIO *g, SPMI_PMIC_BUS *b)
{
    if (!g || !b || !b->arbiter.count || g->connected || g->cleanup || g->volume_up.active || g->volume_up.dirty)
        return STATUS_INVALID_DEVICE_STATE;
    /* PON has two separate IRQs; PM8550 GPIO6 uses its input IRQ at bit 0.
     * The array index is shared with the corresponding GpioInt descriptor. */
    static const struct { USHORT ppid; UCHAR bit; } pins[PianoButtonCount] = {
        [PianoPower] = {0x013, 7}, [PianoVolumeUp] = {0x18d, 0}, [PianoVolumeDown] = {0x013, 6}
    };
    SPMI_BUTTON_GPIO map = {0};
    map.bus = b;
    enum spmi_status status;
    ExAcquireFastMutex(&b->lock);
    status = spmi_irq_open(&b->arbiter, pins[PianoPower].ppid, pins[PianoPower].bit, &map.line[PianoPower]);
    if (status == SPMI_OK) {
        map.line[PianoVolumeDown] = map.line[PianoPower];
        map.line[PianoVolumeDown].bit = pins[PianoVolumeDown].bit;
        status = spmi_irq_open(&b->arbiter, pins[PianoVolumeUp].ppid, pins[PianoVolumeUp].bit, &map.line[PianoVolumeUp]);
    }
    ExReleaseFastMutex(&b->lock);
    if (status == SPMI_OK) *g = map;
    return nt_status(status);
}

static NTSTATUS query_info(PVOID context, PCLIENT_CONTROLLER_BASIC_INFORMATION info)
{
    UNREFERENCED_PARAMETER(context);
    RtlZeroMemory(info, sizeof(*info));
    info->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    info->Size = sizeof(*info);
    info->TotalPins = info->NumberOfPinsPerBank = PianoButtonCount;
    /* PMIC access is serial. All GpioClx callbacks must be at PASSIVE_LEVEL.
     * ActiveBoth emulation also delivers an already-held button on connect. */
    info->Flags.EmulateActiveBoth = TRUE;
    info->Flags.EmulateDebouncing = TRUE;
    return STATUS_SUCCESS;
}

static enum spmi_status set_trigger(SPMI_BUTTON_GPIO *g, unsigned pin, KINTERRUPT_MODE mode,
                                    KINTERRUPT_POLARITY polarity)
{
    enum spmi_trigger how;
    if (mode != Latched && mode != LevelSensitive) return SPMI_INVALID;
    if (polarity == InterruptActiveHigh) how = mode == Latched ? SPMI_RISING : SPMI_HIGH;
    else if (polarity == InterruptActiveLow) how = mode == Latched ? SPMI_FALLING : SPMI_LOW;
    else return SPMI_INVALID; /* GpioClx owns ActiveBoth/initial polarity. */
    return spmi_irq_configure(&g->bus->arbiter, &g->line[pin], how);
}

static enum spmi_status access_bus(SPMI_BUTTON_GPIO *g)
{
    /* A timed-out transaction belongs to the caller that issued it. Do not
     * treat its completed response as the current read/write/ack. */
    if (g->bus->arbiter.pending) {
        (void)spmi_v7_retire(&g->bus->arbiter);
        if (g->bus->arbiter.pending) return SPMI_BUSY;
    }
    for (unsigned i = 0; i < PianoButtonCount; ++i) {
        ULONG64 bit = 1ULL << i;
        if (!(g->cleanup & bit)) continue;
        enum spmi_status s = spmi_irq_mask(&g->bus->arbiter, &g->line[i]);
        if (s == SPMI_OK && i == PianoVolumeUp)
            s = spmi_gpio_input_stop(&g->bus->arbiter, &g->line[i].peripheral, &g->volume_up);
        if (s != SPMI_OK) return s;
        g->cleanup &= ~bit;
    }
    return SPMI_OK;
}

NTSTATUS SpmiButtonGpioDrain(SPMI_BUTTON_GPIO *g)
{
    if (!g || !g->bus) return STATUS_INVALID_DEVICE_STATE;
    SPMI_PMIC_BUS *b = g->bus;
    ExAcquireFastMutex(&b->lock);
    g->cleanup |= g->connected;
    g->connected = 0;
    enum spmi_status s = access_bus(g);
    if (s == SPMI_OK) g->bus = NULL;
    ExReleaseFastMutex(&b->lock);
    return nt_status(s);
}

static NTSTATUS enable(PVOID context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    if (!g->bus || p->BankId || p->PinNumber >= PianoButtonCount) return STATUS_INVALID_PARAMETER;
    if (p->PullConfiguration != GPIO_PIN_PULL_CONFIGURATION_DEFAULT &&
        !(p->PinNumber == PianoVolumeUp && p->PullConfiguration == GPIO_PIN_PULL_CONFIGURATION_PULLUP))
        return STATUS_NOT_SUPPORTED;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = access_bus(g);
    if (s == SPMI_OK && p->PinNumber == PianoVolumeUp && !g->volume_up.active)
        s = spmi_gpio_input_start(&g->bus->arbiter, &g->line[PianoVolumeUp].peripheral, &g->volume_up, 1, 0);
    if (s == SPMI_OK) s = set_trigger(g, p->PinNumber, p->InterruptMode, p->Polarity);
    if (s == SPMI_OK) {
        ULONG64 bit = 1ULL << p->PinNumber;
        s = spmi_irq_enable(&g->bus->arbiter, &g->line[p->PinNumber]);
        if (s == SPMI_OK) g->connected |= bit;
        else {
            /* EN_SET may already have reached the peripheral. Keep the line
             * until its mask write completes, not just until the API returns. */
            g->cleanup |= bit;
            (void)access_bus(g);
        }
    }
    if (s != SPMI_OK && p->PinNumber == PianoVolumeUp && !(g->connected & (1ULL << PianoVolumeUp)) &&
        (g->volume_up.active || g->volume_up.dirty)) {
        g->cleanup |= 1ULL << PianoVolumeUp;
        (void)access_bus(g);
    }
    ExReleaseFastMutex(&g->bus->lock);
    return nt_status(s);
}

static NTSTATUS disable(PVOID context, PGPIO_DISABLE_INTERRUPT_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    if (!g->bus || p->BankId || p->PinNumber >= PianoButtonCount) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = access_bus(g);
    if (s == SPMI_OK) s = spmi_irq_mask(&g->bus->arbiter, &g->line[p->PinNumber]);
    if (s == SPMI_OK && p->PinNumber == PianoVolumeUp)
        s = spmi_gpio_input_stop(&g->bus->arbiter, &g->line[p->PinNumber].peripheral, &g->volume_up);
    if (s == SPMI_OK) g->connected &= ~(1ULL << p->PinNumber);
    ExReleaseFastMutex(&g->bus->lock);
    return nt_status(s);
}

static NTSTATUS unmask(PVOID context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    if (!g->bus || p->BankId || p->PinNumber >= PianoButtonCount) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = access_bus(g);
    if (s == SPMI_OK && !(g->connected & (1ULL << p->PinNumber))) s = SPMI_INVALID;
    if (s == SPMI_OK) s = spmi_irq_unmask(&g->bus->arbiter, &g->line[p->PinNumber]);
    ExReleaseFastMutex(&g->bus->lock);
    return nt_status(s);
}

static NTSTATUS reconfigure(PVOID context, PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    if (!g->bus || p->BankId || p->PinNumber >= PianoButtonCount) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = access_bus(g);
    if (s == SPMI_OK) s = set_trigger(g, p->PinNumber, p->InterruptMode, p->Polarity);
    ExReleaseFastMutex(&g->bus->lock);
    return nt_status(s);
}

static NTSTATUS apply_mask(SPMI_BUTTON_GPIO *g, unsigned bank, ULONG64 requested, PULONG64 failed,
                           enum spmi_status (*apply)(struct spmi_arbiter *, const struct spmi_irq *))
{
    *failed = requested;
    if (!g->bus || bank || requested >> PianoButtonCount) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = STATUS_SUCCESS;
    ExAcquireFastMutex(&g->bus->lock);
    for (unsigned i = 0; i < PianoButtonCount; ++i) {
        ULONG64 bit = 1ULL << i;
        if (!(requested & bit)) continue;
        enum spmi_status s = access_bus(g);
        if (s == SPMI_OK && !(g->connected & bit)) s = SPMI_INVALID;
        if (s == SPMI_OK) s = apply(&g->bus->arbiter, &g->line[i]);
        if (s == SPMI_OK) *failed &= ~bit;
        else if (NT_SUCCESS(status)) status = nt_status(s);
    }
    ExReleaseFastMutex(&g->bus->lock);
    return status;
}

static NTSTATUS mask(PVOID context, PGPIO_MASK_INTERRUPT_PARAMETERS p)
{
    return apply_mask(context, p->BankId, p->PinMask, &p->FailedMask, spmi_irq_mask);
}

static NTSTATUS clear_active(PVOID context, PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    return apply_mask(context, p->BankId, p->ClearActiveMask, &p->FailedClearMask, spmi_irq_ack);
}

static NTSTATUS query_active(PVOID context, PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    p->ActiveMask = 0;
    if (!g->bus || p->BankId) return STATUS_INVALID_PARAMETER;
    ULONG64 active = 0;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = spmi_irq_active_bank(&g->bus->arbiter, g->line, PianoButtonCount,
                                              p->EnabledMask & g->connected, &active);
    ExReleaseFastMutex(&g->bus->lock);
    if (s == SPMI_OK) p->ActiveMask = active;
    return nt_status(s);
}

static NTSTATUS query_enabled(PVOID context, PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS p)
{
    SPMI_BUTTON_GPIO *g = context;
    p->EnabledMask = 0;
    if (!g->bus || p->BankId) return STATUS_INVALID_PARAMETER;
    ULONG64 mask = 0;
    ExAcquireFastMutex(&g->bus->lock);
    enum spmi_status s = access_bus(g);
    for (unsigned i = 0; i < PianoButtonCount && s == SPMI_OK; ++i) {
        if (!(g->connected & (1ULL << i))) continue;
        unsigned enabled;
        s = spmi_irq_enabled(&g->bus->arbiter, &g->line[i], &enabled);
        if (s == SPMI_OK && enabled) mask |= 1ULL << i;
    }
    ExReleaseFastMutex(&g->bus->lock);
    if (s == SPMI_OK) p->EnabledMask = mask;
    return nt_status(s);
}

VOID SpmiButtonGpioCallbacks(PGPIO_CLIENT_REGISTRATION_PACKET p)
{
    p->ControllerContextSize = sizeof(SPMI_BUTTON_GPIO);
    p->CLIENT_QueryControllerBasicInformation = query_info;
    p->CLIENT_EnableInterrupt = enable;
    p->CLIENT_DisableInterrupt = disable;
    p->CLIENT_MaskInterrupts = mask;
    p->CLIENT_UnmaskInterrupt = unmask;
    p->CLIENT_ReconfigureInterrupt = reconfigure;
    p->CLIENT_QueryEnabledInterrupts = query_enabled;
    p->CLIENT_QueryActiveInterrupts = query_active;
    p->CLIENT_ClearActiveInterrupts = clear_active;
}
