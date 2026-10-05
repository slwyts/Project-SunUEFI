// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastboot.h"
typedef struct {
  EFI_STATUS Result,BeforeSnapshot,AfterSnapshot;
  BOOLEAN Attempted,Returned,Clean,Retained,UfsAbsent,UsbAbsent;
  BOOLEAN DeviceHalted,DmaFreed,DomainFreed,ClocksReleased;
  BOOLEAN OtherStreamsStable;
  UINT16 OwnedStreamIndex;
  UINT32 ClockReleaseMask;
} PIANO_USB_BOOT_RETIRE_REPORT;
// Isolated USB-only experiment. No UFS owner/stream may exist before/after.
// Does not execute the returned source. Caller retains Context/Token storage.
EFI_STATUS PianoUsbControllerRunForRamBoot(CONST VOID *Fdt,CONST PIANO_FB_BOOT *Boot,
  PIANO_FB_BOOT_ACTION *Action,PIANO_USB_BOOT_RETIRE_REPORT *Report,BOOLEAN *RebootRequested);
