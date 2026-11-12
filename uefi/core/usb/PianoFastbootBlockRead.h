// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastboot.h"
// Enumerates only real, read-only GPT children of the Piano UFS vendor path.
// The provider must support arbitrary CPU alignment (IoAlign 0/1). Its own
// shared-DMA bounce remains responsible for cache and hardware ownership.
// Stop revokes every exported token, including after a subsequent Init.
EFI_STATUS PianoFastbootBlockReadInit(VOID);
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID);
VOID PianoFastbootBlockReadStop(VOID);
