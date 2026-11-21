/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

/* There is one physical GPUCC CX GDSC for CX, GMU CX and GPU SMMU. The
 * platform clock driver owns this object and its translated GPUCC mapping.
 * Clients keep the parent rails voted while accessing it. */
typedef struct {
    KMUTEX lock;
    volatile ULONG *control, *status;
    ULONG users;
    BOOLEAN initialized, boot_ref;
} ADRENO_CX_DOMAIN;

typedef struct { ADRENO_CX_DOMAIN *domain; } ADRENO_CX_REFERENCE;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoCxInitialize(ADRENO_CX_DOMAIN *domain, void *gpucc, SIZE_T bytes);
/* Zero-initialize each client's reference. A timeout can leave its vote
 * pending; release the same reference during failure cleanup. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoCxAcquire(ADRENO_CX_DOMAIN *domain, ADRENO_CX_REFERENCE *reference);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoCxRelease(ADRENO_CX_DOMAIN *domain, ADRENO_CX_REFERENCE *reference);

/* Release the pre-PnP AP reference once the platform's CX clients,
 * including the SMMU, have acquired their individual references. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoCxReleaseBootReference(ADRENO_CX_DOMAIN *domain);
