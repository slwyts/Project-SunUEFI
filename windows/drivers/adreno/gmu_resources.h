/* SPDX-License-Identifier: MIT */
#pragma once
#include "firmware.h"
#include <dispmprt.h>

typedef struct {
    volatile ULONG *words;
    ULONG bytes;
} ADRENO_REGISTER_WINDOW;

/* Gen8 register numbers are GPU-relative words; the resource begins at
 * GPU word 0x1b000 and ends before GPUCC. */
#define ADRENO_GMU_BASE_WORD 0x1b000u
#define ADRENO_GMU_ITCM_WORD (0x1b400u - ADRENO_GMU_BASE_WORD)

/* Only map the complete translated resource collected by the PnP path.
 * The caller must hold device power while accessing registers. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoMapRegisterWindow(const DXGKRNL_INTERFACE *dxgk,
                                 PCM_PARTIAL_RESOURCE_DESCRIPTOR memory, ADRENO_REGISTER_WINDOW *registers);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoUnmapRegisterWindow(const DXGKRNL_INTERFACE *dxgk, ADRENO_REGISTER_WINDOW *registers);
