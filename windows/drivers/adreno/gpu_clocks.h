/* SPDX-License-Identifier: MIT */
#pragma once
#include "cx_power.h"
#include "gpucc_interface.h"
#include "../gcc/clock_interface.h"

typedef struct ADRENO_GPU_CLOCKS ADRENO_GPU_CLOCKS;
typedef struct {
    ADRENO_GPU_CLOCKS *controller;
    ADRENO_CX_REFERENCE cx;
    ULONG held;
} ADRENO_CLOCK_REFERENCE;

/* GPUCC owns only its translated register resource. The GCC provider
 * retains its own registers and arbitrates votes shared with storage.
 * The platform has taken over the GPU from firmware. SetRailsAndBus must
 * hold the board's XO, CX/MX and interconnect votes before touching GPUCC,
 * including PLL calibration at 0x44 * 19.2 MHz. */
struct ADRENO_GPU_CLOCKS {
    ADRENO_CX_DOMAIN cx;
    KMUTEX lock;
    volatile ULONG *gpucc;
    SM8750_GCC_GPU_INTERFACE gcc;
    ULONG users[12], dirty, partial, forced;
    BOOLEAN park_gmu, gcc_held[Sm8750GpuClockCount];
};

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoPrepareGpuClocks(ADRENO_GPU_CLOCKS *clocks, PCM_PARTIAL_RESOURCE_DESCRIPTOR gpucc);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoReleaseGpuClocks(ADRENO_GPU_CLOCKS *clocks);
/* The FDO has fenced off all local access and owns reference cleanup.
 * Return foreign votes, without dereferencing inaccessible GPUCC registers. */
_IRQL_requires_(PASSIVE_LEVEL)
void AdrenoForgetGpuClocks(ADRENO_GPU_CLOCKS *clocks);

/* Zero-initialize a separate reference per client. Releasing after a
 * failed enable retires any pending hardware operation before its parents.
 * The renderer and GMU both consume AHB and memory NoC. The SMMU has its
 * own branch. Each of the three clients also holds the same physical CX
 * until its last clock has been released. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoSetGpuClocks(ADRENO_GPU_CLOCKS *clocks, ADRENO_CLOCK_REFERENCE *reference,
                           enum adreno_clock_client client, BOOLEAN enable);
