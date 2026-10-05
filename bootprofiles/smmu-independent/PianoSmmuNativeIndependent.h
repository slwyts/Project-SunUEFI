// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoSmmu.h"
#include "PianoIoPageTable.h"
#define PIANO_SMMU_INDEPENDENT_SIGNATURE SIGNATURE_32('S','I','N','D')
typedef enum {PianoIndependentUfs=0,PianoIndependentUsb=1} PIANO_SMMU_INDEPENDENT_MASTER;
typedef struct {
  VOID *Context;
  EFI_STATUS (*Read32)(VOID *,UINTN Address,UINT32 *Value);
  EFI_STATUS (*Write32)(VOID *,UINTN Address,UINT32 Value);
  EFI_STATUS (*Fence)(VOID *);
  EFI_STATUS (*Clean)(VOID *,CONST VOID *Memory,UINTN Bytes);
  EFI_STATUS (*NowUs)(VOID *,UINT64 *Value);
  EFI_STATUS (*Pause)(VOID *);
} PIANO_SMMU_INDEPENDENT_OPS;
typedef struct {UINT32 Cbar,Cba2r,Sctlr,Actlr,Tcr2,Ttbr0Low,Ttbr0High,Ttbr1Low,Ttbr1High,Tcr,Mair0,Mair1,Fsr,FarLow,FarHigh,Fsynr;} PIANO_SMMU_INDEPENDENT_BANK;
typedef struct {
  UINT32 Global,Id0,Id1,Id2,GlobalFault,Smr[127],S2cr[127];
  PIANO_SMMU_INDEPENDENT_BANK Banks[83];
} PIANO_SMMU_INDEPENDENT_SNAPSHOT;
typedef struct {
  UINT32 Signature;
  BOOLEAN Attached,Retained,Closed,RegisterConfigurationVerified,TablesUnreachable;
  PIANO_SMMU_INDEPENDENT_MASTER Master;
  UINT16 Sid,Slot;UINT8 Bank;
  UINT32 WritesAttempted,Syncs;
  EFI_STATUS Status;
  PIANO_SMMU_INDEPENDENT_OPS Ops;
  PIANO_IO_PAGE_TABLE *PageTable;PIANO_DMA_BUFFER *Tables;
  PIANO_SMMU_INDEPENDENT_SNAPSHOT Before,Expected;
} PIANO_SMMU_INDEPENDENT;
// Prototype uses injected status-returning register/cache operations only.
// Caller supplies exclusive driver-lifetime state and existing shared PT/DMA.
// No HAL, allocator, global reset, master enable or product integration exists.
EFI_STATUS PianoSmmuIndependentOpen(PIANO_SMMU_INDEPENDENT *,PIANO_SMMU_INDEPENDENT_MASTER,
  CONST PIANO_SMMU_INDEPENDENT_OPS *,PIANO_IO_PAGE_TABLE *,PIANO_DMA_BUFFER *);
EFI_STATUS PianoSmmuIndependentSync(PIANO_SMMU_INDEPENDENT *);
EFI_STATUS PianoSmmuIndependentClose(PIANO_SMMU_INDEPENDENT *);
