// SPDX-License-Identifier: BSD-2-Clause-Patent
// THP matrix to one contact, ported from the Linux reader
// debian-piano initramfs/touch-view/piano-touch-view.c
#pragma once
#include <Uefi.h>

#define PIANO_TOUCH_MAX_NODES 4096U
#define PIANO_TOUCH_REF_FRAMES 32U
#define PIANO_TOUCH_PROBE_FRAMES 16U
#define PIANO_TOUCH_THRESHOLD 200
#define PIANO_TOUCH_SCREEN_W 3200
#define PIANO_TOUCH_SCREEN_H 2136
#define PIANO_TOUCH_TRACK_DIST 400

typedef struct {
  INT32 RefType;
  UINT32 ProbeCount,ProbeTypes[256],RefCount,Rows,Cols,Nodes;
  BOOLEAN HaveReference;
  INT16 Samples[PIANO_TOUCH_REF_FRAMES][PIANO_TOUCH_MAX_NODES];
  INT32 Reference[PIANO_TOUCH_MAX_NODES],Delta[PIANO_TOUCH_MAX_NODES];
  INT32 MaxDelta;
  // Screen pixels, landscape like Linux
  BOOLEAN Touching;
  INT32 X,Y;
  UINT32 Frames,BadChecksum,Skipped;
  UINT16 LastFrameNo;
} PIANO_TOUCH_MATRIX;

VOID PianoTouchMatrixReset(OUT PIANO_TOUCH_MATRIX *M);
// Payload is the frame after the 257-byte event buffer. EFI_SUCCESS when
// Touching/X/Y were updated, EFI_NOT_READY while learning the reference
EFI_STATUS PianoTouchMatrixFeed(IN OUT PIANO_TOUCH_MATRIX *M,IN CONST UINT8 *Payload,IN UINTN Bytes);
// 1 match, 0 mismatch, -1 inconsistent header fields
INTN PianoTouchPayloadChecksum(IN CONST UINT8 *Payload,IN UINTN Bytes);
