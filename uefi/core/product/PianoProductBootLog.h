// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

typedef BOOLEAN (EFIAPI *PIANO_PRODUCT_BOOT_LOG_ALIVE)(VOID);

// A display-only sink for actual initialization results. The caller owns the
// ExitBootServices fence and elapsed-time measurement. No console state changes.
EFI_STATUS PianoProductBootLogInitialize(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                                        PIANO_PRODUCT_BOOT_LOG_ALIVE Alive);
// Rows cover the visible startup stages. Earlier SMEM capture stays in the
// firmware log. Failures show their EFI_STATUS; painting does not gate startup.
EFI_STATUS PianoProductBootLogStage(CONST CHAR8 *Name,EFI_STATUS Status,
                                   UINT64 ElapsedMs);
// Human-readable boot/exit progress and recovery instructions through the same GOP.
// A failure page retains the last stage and elapsed time when its status matches.
EFI_STATUS PianoProductBootLogMessage(CONST CHAR16 *Title,CONST CHAR16 *Detail,
                                     EFI_STATUS Status,BOOLEAN Failed);
// Explicit diagnostic builds only: draw through the same GOP used by the
// product boot log. No mode change, hardware write or protocol replacement.
EFI_STATUS PianoProductDisplayCheckpoint(CONST CHAR8 *Name,UINT32 Stage);
VOID PianoProductDisplayCheckpointReplay(VOID);
