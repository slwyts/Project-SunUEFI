// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDma.h"
#include "PianoSmmu.h"
#include "PianoIoPageTable.h"
// Observation only. These records never authorize a changed peer/unused slot.
typedef struct {
  BOOLEAN Valid,LiveRead;
  CONST CHAR8 *Reason;
  UINT32 Index;
  EFI_STATUS CaptureStatus;
  UINT32 BeforeSmr,BeforeS2cr,AfterSmr,AfterS2cr;
  UINT32 LiveSmr[2],LiveS2cr[2];
} PIANO_SMMU_CLOSE_DIAGNOSTIC;
typedef struct {
  VOID *Domain,*Api;
  BOOLEAN Attached,Verified,ExitRetained;
  PIANO_DMA_BUFFER TableMemory;
  PIANO_IO_PAGE_TABLE PageTable;
  PIANO_SMMU_SNAPSHOT Before,After;
  UINT8 Used[PIANO_IOVA_BYTES/4096];
  struct {BOOLEAN Used;UINT32 First,Pages;} Mapping[64];
  CONST VOID *Fdt;
  CONST CHAR8 *ResourceName;
  UINT8 DeviceIndex;
  BOOLEAN BaselineSaved;
  UINT8 BaselineSlotCount;
  UINT16 BaselineSlots[7],DiagnosticOwnedSlot;
  PIANO_SMMU_CLOSE_DIAGNOSTIC CloseDiagnostic;
} PIANO_OWNED_SMMU;
EFI_STATUS PianoOwnedSmmuOpen(CONST VOID *Fdt,PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device);
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device);
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *Context);
// Re-emits saved baseline/last rejection without MMIO or native HAL calls;
// safe after clocks close. Full body CRC and its mirror have the same sequence.
VOID PianoOwnedSmmuReport(CONST PIANO_OWNED_SMMU *Context);
// No native HAL, allocation, detach or free; the owned hardware table remains
// Reserved until a conforming EFI-map consumer takes over/reprograms the SMMU.
EFI_STATUS PianoOwnedSmmuRetainForExit(PIANO_OWNED_SMMU *Context);
EFI_STATUS PianoOwnedSmmuMemoryExperiment(CONST VOID *Fdt);
