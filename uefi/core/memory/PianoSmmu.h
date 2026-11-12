// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_SMMU_DEVICE_COUNT 4
// ARM SMMUv2 FSR bits10:9 describe the translation format; they are not faults.
// Keep them as captured metadata. Only architectural fault/status bits below
// authorize own-bank W1C clearing or fence a healthy DMA context.
#define PIANO_SMMU_FSR_FORMAT_MASK (BIT10|BIT9)
#define PIANO_SMMU_FSR_FAULT_MASK  (BIT31|BIT30|0x000001FEU)
typedef struct {
  CONST CHAR8 *Name;
  UINT16 Sid,Mask;
  UINT16 StreamIndex;
  UINT8 Type,ContextBank;
  BOOLEAN Present,Enabled;
  UINT32 Smr,S2cr,Sctlr,Cbar,Cba2r,Tcr,Tcr2,Mair0,Mair1,Fsr,Fsynr;
  UINT64 Ttbr0,Ttbr1,Far;
} PIANO_SMMU_DEVICE;
typedef struct {
  BOOLEAN Valid,ExtendedIds;
  UINT32 Id0,Id1,Id2,GlobalControl,GlobalFault;
  UINT32 PageShift,Groups,Banks;
  UINTN Base,Window,ContextBase;
  UINT32 RawSmr[256],RawS2cr[256];
  PIANO_SMMU_DEVICE Device[PIANO_SMMU_DEVICE_COUNT];
} PIANO_SMMU_SNAPSHOT;
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Snapshot);
// Same live register reads and checks, without per-row diagnostic output.
EFI_STATUS PianoSmmuCaptureQuiet(CONST VOID *Fdt,PIANO_SMMU_SNAPSHOT *Snapshot);
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *Snapshot);
BOOLEAN PianoSmmuStreamMatches(UINT32 Smr,UINT32 S2cr,BOOLEAN Extended,UINT16 Sid);
