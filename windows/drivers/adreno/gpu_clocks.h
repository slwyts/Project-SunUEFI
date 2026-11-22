/* SPDX-License-Identifier: MIT */
#pragma once
#include "cx_power.h"
#include "../gcc/fixed_pll.h"

typedef struct ADRENO_GPU_CLOCKS ADRENO_GPU_CLOCKS;
typedef struct {
    ADRENO_GPU_CLOCKS *controller;
    ADRENO_CX_REFERENCE cx;
    ULONG held;
} ADRENO_CLOCK_REFERENCE;

enum adreno_clock_client { AdrenoGmuClocks, AdrenoRenderBusClocks, AdrenoSmmuClocks };

/* GPUCC owns its register resource. GCC lends its already mapped resource
 * and the same APSS vote lock used by the other clients of GCC's 0x52000.
 * The sole consumer of that word's GPU GPLL0-main bit is GPUCC. Never map
 * GCC from the render PDO. Keep both providers in D0 while taking clocks.
 * The platform has taken over the GPU from firmware. SetRailsAndBus must
 * hold the board's XO, CX/MX and interconnect votes before touching GPUCC,
 * including PLL calibration at 0x44 * 19.2 MHz. */
struct ADRENO_GPU_CLOCKS {
    ADRENO_CX_DOMAIN cx;
    KMUTEX lock;
    volatile ULONG *gpucc, *axi, *memnoc, *gpll_vote;
    SM8750_GCC_PLLS *gcc;
    ULONG users[12], dirty, partial, forced;
    BOOLEAN park_gmu, parent_held;
};

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoPrepareGpuClocks(ADRENO_GPU_CLOCKS *clocks, PCM_PARTIAL_RESOURCE_DESCRIPTOR gpucc, SM8750_GCC_PLLS *gcc);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoReleaseGpuClocks(ADRENO_GPU_CLOCKS *clocks);

/* Zero-initialize a separate reference per client. Releasing after a
 * failed enable retires any pending hardware operation before its parents.
 * The renderer and GMU both consume AHB and memory NoC. The SMMU has its
 * own branch. Each of the three clients also holds the same physical CX
 * until its last clock has been released. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoSetGpuClocks(ADRENO_GPU_CLOCKS *clocks, ADRENO_CLOCK_REFERENCE *reference,
                           enum adreno_clock_client client, BOOLEAN enable);
