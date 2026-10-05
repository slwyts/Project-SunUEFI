// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

typedef BOOLEAN (EFIAPI *PIANO_PRODUCT_BOOT_LOG_ALIVE)(VOID);

// A display-only sink for actual initialization results. The caller owns the
// ExitBootServices fence and elapsed-time measurement. No console state changes.
EFI_STATUS PianoProductBootLogInitialize(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                                        PIANO_PRODUCT_BOOT_LOG_ALIVE Alive);
// Fixed rows: DISPLAY, PAYLOAD, SMEM, INPUT, UFS, USB, MENU. Non-success results
// except NOT_STARTED show their complete native EFI_STATUS value. A display failure never becomes a
// service result; the caller continues to own all existing startup decisions.
EFI_STATUS PianoProductBootLogStage(CONST CHAR8 *Name,EFI_STATUS Status,
                                   UINT64 ElapsedMs);
