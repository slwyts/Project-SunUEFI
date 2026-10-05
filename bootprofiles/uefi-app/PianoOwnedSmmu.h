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
// Observed execution ledger, not permission to accept a changed peer slot.
// Any failed/unknown stage is sticky until a new OpenResource zeroes context.
typedef struct {
  BOOLEAN DetachAttempted,DetachedCaptureAttempted,DestroyAttempted,TableFreeAttempted;
  BOOLEAN Uncertain,ExactClose;
  UINT32 DetachCode,DestroyCode;
  EFI_STATUS DetachedCaptureStatus,TableFreeStatus;
} PIANO_SMMU_CLOSE_LEDGER;
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
  BOOLEAN OwnedIdentitySaved;
  EFI_PHYSICAL_ADDRESS OwnedTablePhysical;
  PIANO_SMMU_SNAPSHOT AttachedSnapshot;
  PIANO_SMMU_CLOSE_LEDGER CloseLedger;
  CONST struct PIANO_SMMU_RETIRED_USB_CONTRACT *RetiredUsbContract;
} PIANO_OWNED_SMMU;
#define PIANO_SMMU_USB_RETIRE_REVISION 1U
// Supplied by the trusted device/controller ledger after the complete USB
// shutdown, not by a host request or by inference from an invalid SMR alone.
typedef struct {
  UINT32 Revision;
  EFI_STATUS DeviceCleanupStatus,ControllerCleanupStatus;
  BOOLEAN DeviceHalted,DmaFreed,ClocksReleased,GdscReleased;
  UINT32 DmaBuffersFreed,ClockReleaseMask;
} PIANO_SMMU_USB_RETIRE_EVIDENCE;
typedef struct PIANO_SMMU_RETIRED_USB_PROOF {
  UINT32 Revision;
  BOOLEAN Valid;
  CONST PIANO_OWNED_SMMU *UsbContext;
  PIANO_SMMU_USB_RETIRE_EVIDENCE Execution;
  UINT16 Slot;
  UINT8 ContextBank;
  EFI_PHYSICAL_ADDRESS TablePhysical;
  PIANO_SMMU_SNAPSHOT UsbBaseline,Attached,Retired;
} PIANO_SMMU_RETIRED_USB_PROOF;
typedef struct PIANO_SMMU_RETIRED_USB_CONTRACT {
  UINT32 Revision;
  BOOLEAN Valid;
  CONST PIANO_OWNED_SMMU *Owner,*Peer;
  UINT16 PeerSlot,OwnerSlot;
  UINT32 BaselineSmr,BaselineS2cr,RetiredSmr,RetiredS2cr;
  EFI_PHYSICAL_ADDRESS PeerTablePhysical;
  PIANO_SMMU_DEVICE OwnerIdentity;
  PIANO_SMMU_SNAPSHOT OwnerBaseline,Authorized;
} PIANO_SMMU_RETIRED_USB_CONTRACT;
// Pure validation/copy only: no MMIO, native HAL, detach, free or normalization.
// Success cannot change the default strict PianoOwnedSmmuClose behavior.
EFI_STATUS PianoOwnedSmmuMakeRetiredUsbProof(CONST PIANO_OWNED_SMMU *Usb,
  CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *Execution,CONST PIANO_SMMU_SNAPSHOT *Current,
  PIANO_SMMU_RETIRED_USB_PROOF *Proof);
EFI_STATUS PianoOwnedSmmuPrepareRetiredUsbContract(CONST PIANO_OWNED_SMMU *Ufs,
  CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof,CONST PIANO_SMMU_SNAPSHOT *Current,
  PIANO_SMMU_RETIRED_USB_CONTRACT *Contract);
// Checks the explicitly authorized, exact zero/zero peer row after owner
// detach. Every other raw row remains strict except the actual owner's row.
// It returns a decision; it never rewrites Context->Before or hardware.
EFI_STATUS PianoOwnedSmmuCheckRetiredUsbContract(CONST PIANO_OWNED_SMMU *Ufs,
  CONST PIANO_SMMU_RETIRED_USB_CONTRACT *Contract,CONST PIANO_SMMU_SNAPSHOT *Current);
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
