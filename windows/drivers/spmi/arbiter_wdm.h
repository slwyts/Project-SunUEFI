/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include "interrupt.h"

typedef struct {
    struct spmi_arbiter arbiter;
    PUCHAR window[SPMI_REGIONS];
    FAST_MUTEX lock;
} SPMI_PMIC_BUS;

/* One parent owns the primary bus, not one mapping per PMIC child. Register
 * users hold lock across compound operations. After surprise removal they
 * must no longer be dispatched, even if register mappings still exist. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SpmiPmicPrepareBus(SPMI_PMIC_BUS *, PCM_PARTIAL_RESOURCE_LIST raw,
                            PCM_PARTIAL_RESOURCE_LIST translated);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SpmiPmicReleaseBus(SPMI_PMIC_BUS *, BOOLEAN surprise);
