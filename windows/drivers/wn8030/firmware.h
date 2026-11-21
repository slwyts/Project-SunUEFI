/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>

/* spb is the synchronous PnP resource connection for the controller.
 * Use after cold reset and wake, before starting input interrupts. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030BootFirmware(PDRIVER_OBJECT driver, HANDLE spb);
