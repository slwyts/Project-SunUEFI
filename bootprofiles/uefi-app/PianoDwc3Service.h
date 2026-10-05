// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoOwnedSmmu.h"
#include "PianoFastboot.h"
#ifndef PIANO_USB_SERVICE
#define PIANO_USB_SERVICE 0
#endif
#if PIANO_USB_SERVICE != 0 && PIANO_USB_SERVICE != 1
#error PIANO_USB_SERVICE must be exactly 0 or 1
#endif
typedef struct {
  VOID *Context;
  // Monotonic CPU-only clock. Never allocate, call BS or read an owner device.
  UINT64 (*NowUs)(VOID *Context);
  // Optional immutable callbacks, copied by Controller Start. The backing
  // Context must outlive the service; public partition reads use the shared
  // UFS/USB SMMU guard rather than directly calling these callbacks.
  CONST PIANO_FB_STORAGE *Storage;
  // Optional APP-only immutable snapshot reemit. CPU/RAM only: no MMIO,
  // allocation, BS, device access or recursive service work. Copied at Start.
  // Exact success is required before oem ramlog freezes the RAM console.
  EFI_STATUS (*BeforeRamlog)(VOID *Context);
} PIANO_DWC3_SERVICE_CONFIG;
typedef enum {
  PianoUsbServiceOff=0,PianoUsbServiceListening,PianoUsbServiceStopRequested,
  PianoUsbServiceRetained,PianoUsbServiceExited
} PIANO_USB_SERVICE_PHASE;
typedef enum {
  PianoUsbServiceActionNone=0,PianoUsbServiceActionContinue,PianoUsbServiceActionReboot,
  PianoUsbServiceActionBoot,PianoUsbServiceActionFault
} PIANO_USB_SERVICE_ACTION;
typedef struct {
  UINT32 Revision;
  PIANO_USB_SERVICE_PHASE Phase;
  PIANO_USB_SERVICE_ACTION Action;
  EFI_STATUS LastStatus;
  BOOLEAN Started,Configured,WorkPending,Busy,Retained,ServicesLost;
  BOOLEAN BulkActive; // APP snapshot: frames/EP3 in-flight/download, never idle EP2
  BOOLEAN DeviceHalted,DmaFreed;
  UINT32 DmaBuffersFreed,QueuedEvents;
  UINT64 OutBytes,InBytes;
} PIANO_DWC3_SERVICE_STATUS;
// APP-only start/worker/stop. Start requires the actual owned USB context.
EFI_STATUS PianoDwc3ServiceStart(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device,
  CONST PIANO_DWC3_SERVICE_CONFIG *Config);
// Notification-safe: bounded ring/cache/MMIO and raw event queue only.
EFI_STATUS PianoDwc3ServicePollBounded(UINTN MaxEvents);
EFI_STATUS PianoDwc3ServicePumpApp(UINT32 Reason,UINTN BudgetUs);
EFI_STATUS PianoDwc3ServiceStop(EFI_STATUS Reason);
EFI_STATUS PianoDwc3ServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status);
// CPU-only, most recent actual Device cleanup; clocks filled by Controller.
EFI_STATUS PianoDwc3GetRetireEvidence(PIANO_SMMU_USB_RETIRE_EVIDENCE *Evidence);
// CPU/MMIO-only last-resort EBS fence. It retains resources; never full close.
EFI_STATUS PianoDwc3ServiceFenceExit(VOID);
