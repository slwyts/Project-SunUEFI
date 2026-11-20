/* SPDX-License-Identifier: MIT */
#pragma once
#include "firmware.h"
#include <dispmprt.h>

typedef struct {
    void *mapping;
} ADRENO_GMU_TCM;

/* Pass the translated GMU "gmu" memory resource for Adreno 830. The caller
 * must hold device power while accessing it; mapping alone never powers or
 * resets the processor.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoMapGmuTcm(const DXGKRNL_INTERFACE *dxgk,
                         PCM_PARTIAL_RESOURCE_DESCRIPTOR memory, ADRENO_GMU_TCM *tcm);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoUnmapGmuTcm(const DXGKRNL_INTERFACE *dxgk, ADRENO_GMU_TCM *tcm);
