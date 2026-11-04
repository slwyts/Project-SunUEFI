// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoCpuInput.h"

// Check the cold producer's map against the live EFI allocation ledger. These
// functions allocate no pages and never convert or release occupied memory.
EFI_STATUS PianoProductMemoryCheck(VOID *Context,PIANO_LINUX_MEMORY_PROOF *Proof);
EFI_STATUS PianoProductMemoryValidate(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Proof);
// The final Linux tree must describe RAM/reservations compatible with the map
// admitted before the first MemoryPeim, including FDT reserve-map entries.
EFI_STATUS PianoProductMemoryValidateDtb(CONST VOID *Tree,UINTN Bytes);
