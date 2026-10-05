// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

typedef BOOLEAN (EFIAPI *PIANO_SPLASH_ALIVE)(VOID);
typedef struct {
  UINT32 Width,Height,LogoX,LogoY,LogoSize,WordmarkX,WordmarkY,HintX,HintY,KeysY;
  UINT32 WordmarkScale,HintScale,KeysScale,DrawCalls;
  EFI_STATUS Status;
} PIANO_SPLASH_REPORT;

// Uses the real GOP Blt interface only. No frame-buffer pointer, allocation,
// text-console reconfiguration, timer, keyboard claim or BootLogo2 dependency.
EFI_STATUS PianoProductDrawSplash(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                                 PIANO_SPLASH_ALIVE Alive,PIANO_SPLASH_REPORT *Report);
