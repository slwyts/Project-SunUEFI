/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

typedef struct {
    PDEVICE_OBJECT pdo;
    PVOID notification;
    volatile LONG removing;
} SM8750_CLOCK_TARGET;

/* The PnP registration watches the same file on which the interface is
 * queried. Stop watching before reusing its storage or releasing the PDO. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Sm8750QueryClockInterface(const GUID *category, const GUID *interface_id,
                                   PINTERFACE result, USHORT size, PDRIVER_OBJECT driver, SM8750_CLOCK_TARGET *target);
NTSTATUS Sm8750ClockTargetStatus(SM8750_CLOCK_TARGET *target);
void Sm8750CloseClockTarget(SM8750_CLOCK_TARGET *target);
