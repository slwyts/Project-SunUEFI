// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <Uefi.h>
typedef struct {
  BOOLEAN Started,Returned,Clean,Retained;
  EFI_STATUS TimerCancel,TimerClose,Disconnect,Uninstall,WaitClose,Status;
} PIANO_KEYS_RETIRE_REPORT;
EFI_STATUS PianoStopKeysForProduct(PIANO_KEYS_RETIRE_REPORT *Report);
