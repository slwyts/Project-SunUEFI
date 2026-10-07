// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastboot.h"
// Captures through the actual GOP protocol. A successful return has staged an
// independent, complete BMP; metadata is zero on any failure.
EFI_STATUS PianoFastbootCaptureScreen(PIANO_FASTBOOT *State,UINT32 *Width,
                                     UINT32 *Height,UINT32 *Bytes,UINT32 *Crc32);
