// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDwc3Service.h"
typedef struct {
  UINT32 Revision;
  EFI_STATUS Status,DeviceStatus,OwnedCloseStatus,TimerCloseStatus;
  BOOLEAN Attempted,Clean,Retained,DeviceHalted,DmaFreed,DomainFreed,ClocksReleased;
  UINT32 DmaBuffersFreed,ClockReleaseMask;
  // The host session is retired first; the device fields then come from the
  // device stop made when it started
  BOOLEAN HostWasActive;
  EFI_STATUS HostStopStatus;
  UINT32 HostSessions;
} PIANO_USB_SERVICE_RETIRE_REPORT;
// One persistent real USB0 owner. UI app return never invokes Stop.
EFI_STATUS PianoUsbControllerServiceStart(CONST VOID *Fdt,CONST PIANO_DWC3_SERVICE_CONFIG *Config);
EFI_STATUS PianoUsbControllerServicePumpApp(UINT32 Reason,UINTN BudgetUs);
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status);
// Full APP cleanup; a failed/unknown stage returns retained and prevents retry.
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report);
// APP, before an exit is latched. EFI_NOT_READY: a USB device is still in use,
// nothing changed and the exit can be retried after unplugging
EFI_STATUS PianoUsbControllerServiceLeaveHost(VOID);
// The host role is busy or failed; handoff is refused for USB
BOOLEAN PianoUsbControllerHostBlocked(VOID);
// The action in GetStatus is consumed by the product all-owner manager. These
// service APIs never ResetSystem or load/start an EFI image themselves.
