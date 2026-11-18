// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFvApplication.h"

typedef enum {
  PianoBootSystems,
  PianoBootAutomatic,
  PianoBootSelected
} PIANO_BOOT_SELECTION;

typedef struct {
  PIANO_BOOT_SELECTION Selection;
  UINT16 Number;
  EFI_STATUS (*PrepareSystem)(VOID);
  BOOLEAN (*CanTryNextSystem)(EFI_STATUS Status);
  BOOLEAN SystemBlocked,StartedMenu,StartedSystem;
} PIANO_BOOT_DISPATCH;

EFI_STATUS PianoRefreshBootOptions(VOID);
BOOLEAN PianoBootOptionIsSimpleInit(CONST EFI_DEVICE_PATH_PROTOCOL *Path);
EFI_STATUS PianoStartBootOption(EFI_HANDLE Parent,EFI_HANDLE *Device,
                                PIANO_FV_APPLICATION *Application,PIANO_BOOT_DISPATCH *Dispatch);
