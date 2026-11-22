/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

typedef void SM8750_CLOCK_TARGET_REMOVED(PVOID context);

typedef struct {
    PDEVICE_OBJECT pdo;
    PVOID notification;
    volatile LONG removing;
    /* Optional callback for remove-complete. Queue cleanup; do not wait. */
    SM8750_CLOCK_TARGET_REMOVED *removed;
    PVOID context;
} SM8750_CLOCK_TARGET;

/* The watcher and interface reference have separate lifetimes. A further
 * query can reuse the watch on the same PDO or replace it after the caller
 * has released the old interface. Serialize queries and closes. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Sm8750QueryClockInterface(const GUID *category, const GUID *interface_id,
                                   PINTERFACE result, USHORT size, PDRIVER_OBJECT driver, SM8750_CLOCK_TARGET *target);
NTSTATUS Sm8750ClockTargetStatus(SM8750_CLOCK_TARGET *target);
/* Cancel notifications and release the PDO, retaining callback configuration. */
_IRQL_requires_(PASSIVE_LEVEL)
void Sm8750CloseClockTarget(SM8750_CLOCK_TARGET *target);
