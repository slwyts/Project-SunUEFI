// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDwc3Service.h"
typedef struct {
  UINT32 Revision;
  EFI_STATUS Status,DeviceStatus,OwnedCloseStatus,TimerCloseStatus;
  BOOLEAN Attempted,Clean,Retained,DeviceHalted,DmaFreed,DomainFreed,ClocksReleased;
  UINT32 DmaBuffersFreed,ClockReleaseMask;
} PIANO_USB_SERVICE_RETIRE_REPORT;
// One persistent real USB0 owner. UI app return never invokes Stop.
EFI_STATUS PianoUsbControllerServiceStart(CONST VOID *Fdt,CONST PIANO_DWC3_SERVICE_CONFIG *Config);
EFI_STATUS PianoUsbControllerServicePumpApp(UINT32 Reason,UINTN BudgetUs);
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status);
// Full APP cleanup; a failed/unknown stage returns retained and prevents retry.
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report);
// The action in GetStatus is consumed by the product all-owner manager. These
// service APIs never ResetSystem or load/start an EFI image themselves.
