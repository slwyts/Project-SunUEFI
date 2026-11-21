/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include "scmi_channel.h"

/* One instance in the PnP device owning CPUCP TX channel 0. All protocols
 * using that slot must share this instance and its passive-level lock. */
typedef struct {
    KMUTEX Lock;
    PULONG Shmem, Doorbell;
    struct scmi_channel Channel;
    volatile LONG Removed;
    BOOLEAN Online;
} PIANO_CPUCP_TX;

VOID PianoCpucpInitialize(PIANO_CPUCP_TX *tx);
NTSTATUS PianoCpucpStart(PIANO_CPUCP_TX *tx, const CM_PARTIAL_RESOURCE_DESCRIPTOR *shmem,
                         const CM_PARTIAL_RESOURCE_DESCRIPTOR *doorbell);
NTSTATUS PianoCpucpStop(PIANO_CPUCP_TX *tx, BOOLEAN surprise);
VOID PianoCpucpRelease(PIANO_CPUCP_TX *tx);
int PianoCpucpMessage(void *context, unsigned protocol, unsigned command, const uint32_t *args,
                      size_t nargs, uint32_t *result, size_t capacity, size_t *count);
