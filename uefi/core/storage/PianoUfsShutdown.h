// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_UFS_SHUTDOWN_GUID {0x3D43C751,0xC239,0x42D5,{0x95,0x9C,0x75,0x21,0xF0,0x18,0x02,0x60}}
typedef struct {UINT64 Revision;EFI_STATUS (*Halt)(VOID);} PIANO_UFS_SHUTDOWN;
// Cold-reset-only two-phase retirement. Prepare leaves DMA/domain/clocks owned
// but fully halted so another owner (USB) can retire between these calls.
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID);
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID);
EFI_STATUS PianoUfsStopClocksForReset(VOID);
typedef struct {
  BOOLEAN Started,Prepared,Returned,Clean,Failed,TplHeld;
  EFI_STATUS Result,Disconnect,Halt,Bases,Dma,Domain,Protocols,Clocks;
  UINTN Disconnected,DmaFreed,ProtocolsRemoved;
  UINT32 TransferDoorbell,TaskDoorbell,TransferRun,TaskRun,Interrupt;
} PIANO_UFS_RESET_REPORT;
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID);
// Explicitly bind the successfully retired shared USB owner before UFS
// retirement. A fresh SMMU capture and exact baseline/identity proof are needed.
struct PIANO_SMMU_RETIRED_USB_PROOF;
EFI_STATUS PianoUfsAcceptRetiredUsb(CONST struct PIANO_SMMU_RETIRED_USB_PROOF *Proof);
