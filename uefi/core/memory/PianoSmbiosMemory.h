// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoMemoryLayout.h"
#include <Protocol/Smbios.h>

EFI_STATUS PianoSmbiosAddMemory (
  EFI_SMBIOS_PROTOCOL *Smbios, EFI_HANDLE Producer, CONST PIANO_MEMORY_LAYOUT *Layout
  );
