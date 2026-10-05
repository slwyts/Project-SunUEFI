// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include "Protocol/PianoProductRuntime.h"
#include "PianoFvApplication.h"
#include "PianoUsbService.h"
typedef struct {
  BOOLEAN Initialized,ProtocolInstalled,ServicesLost,Retained,Pumping,Dispatching;
  UINT32 PendingAction,ActiveAction,KeyboardProviders;
  UINT64 Sequence,PumpCalls,AppRuns;
  EFI_STATUS LastPump,LastAction,KeyStatus,PayloadStatus;
  // CPU-only trusted UI reason; parent gives this to the unified owner manager.
  // Never a direct reset from a GUI/pump or a fabricated USB worker action.
  PIANO_USB_SERVICE_ACTION RequestedCoreAction;
  PIANO_DWC3_SERVICE_STATUS Usb;
  PIANO_FV_APPLICATION Application;
} PIANO_BOOT_POLICY_REPORT;
// Shared core starts real USB/UFS/input first; this policy does not create a
// transport, stop it on UI return, or turn an unbound worker into USB-ready.
EFI_STATUS PianoBootPolicyInitialize(EFI_HANDLE Parent);
// Auto SimpleInit from Root's actual APPv1 validated payload registry. Pending
// actions dispatch only at APP after the preceding child cooperatively exits.
EFI_STATUS PianoBootPolicyRun(VOID);
EFI_STATUS PianoBootPolicyDispatchPending(VOID);
// Parent must keep code/state alive while installed, or Stop before returning.
// This stops policy notifications only, never shared device/service owners.
EFI_STATUS PianoBootPolicyStop(VOID);
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID);
