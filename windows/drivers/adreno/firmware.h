/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

#define ADRENO_GMU_TCM_BYTES 0x4000u

typedef struct {
    void *data;
    ULONG bytes;
} ADRENO_IMAGE;

typedef struct {
    ADRENO_IMAGE gmu;
    ADRENO_IMAGE sqe;
    ADRENO_IMAGE aqe;
} ADRENO_FIRMWARE;

/* Call at PASSIVE_LEVEL from a system thread. IRP_MN_START_DEVICE should pass
 * the miniport's DriverObject, not the one belonging to the parent bus.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoReadFirmware(PDRIVER_OBJECT driver, ADRENO_FIRMWARE *firmware);

_IRQL_requires_max_(APC_LEVEL)
void AdrenoFreeFirmware(ADRENO_FIRMWARE *firmware);

/* The caller owns the powered GMU's translated TCM mapping and the GMU
 * instruction cache allocation, and keeps reset asserted until this returns.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoUploadGmu(const ADRENO_IMAGE *image, volatile ULONG *tcm,
                         PMDL cache);

/* Write SQE/AQE data into the caller's mapped firmware buffer. */
_IRQL_requires_max_(APC_LEVEL)
NTSTATUS AdrenoCopyMicrocode(const ADRENO_IMAGE *image, PMDL destination, SIZE_T *copied);
