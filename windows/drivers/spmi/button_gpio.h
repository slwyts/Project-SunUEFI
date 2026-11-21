/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <wdf.h>
#include <gpioclx.h>
#include "arbiter_wdm.h"
#include "gpio_input.h"

enum piano_button_pin { PianoPower, PianoVolumeUp, PianoVolumeDown, PianoButtonCount };

/* GpioClx supplies this controller context. The SPMI parent owns the shared
 * interrupt and bus, and assigns the dedicated button pins before use. */
typedef struct {
    SPMI_PMIC_BUS *bus;
    struct spmi_irq line[PianoButtonCount];
    struct spmi_gpio_input volume_up;
    ULONG64 connected, cleanup;
} SPMI_BUTTON_GPIO;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SpmiButtonGpioPrepare(SPMI_BUTTON_GPIO *, SPMI_PMIC_BUS *);
/* Drain before returning the shared bus on a normal device stop. An error
 * keeps the bus reference and the exact lines still owned for a later retry. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SpmiButtonGpioDrain(SPMI_BUTTON_GPIO *);
VOID SpmiButtonGpioCallbacks(PGPIO_CLIENT_REGISTRATION_PACKET);
