/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>

typedef struct WN8030_INPUT WN8030_INPUT;

/* Use the WN8030 I2C connection from the PDO's translated PnP resources.
 * The supplied FDO must have the inbox Vhf.sys lower filter attached. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030InputCreate(PDEVICE_OBJECT fdo, const CM_PARTIAL_RESOURCE_DESCRIPTOR *connection,
                          WN8030_INPUT **result);

/* D0 starts with the MCU awake. Cold starts need the RAM image; a retained
 * resume is confirmed separately by the controller's data-ready IRQ. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030InputWake(WN8030_INPUT *input, BOOLEAN cold, BOOLEAN *attached);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030InputSleep(WN8030_INPUT *input);
/* Timestamp from the data-ready interrupt, in 100 ns units. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030InputRead(WN8030_INPUT *input, ULONGLONG interrupt_time, BOOLEAN *attached);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030CoverKeepAlive(WN8030_INPUT *input);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Wn8030SetBacklight(WN8030_INPUT *input, UCHAR brightness);

/* Quiesce interrupts and other callers first. Releases held inputs, finishes
 * pending HID requests and closes the resource connection. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID Wn8030InputDelete(WN8030_INPUT *input);
