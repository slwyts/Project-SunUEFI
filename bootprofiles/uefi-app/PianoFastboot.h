// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_FASTBOOT_MAX_DOWNLOAD  (64U * 1024U * 1024U)
typedef EFI_STATUS (*PIANO_FB_SEND)(VOID *Context, CONST VOID *Data, UINTN Bytes);
typedef EFI_STATUS (*PIANO_FB_LOG)(VOID *Context, PIANO_FB_SEND Send);
typedef struct {
  VOID *Context;
  PIANO_FB_SEND Send;
  PIANO_FB_LOG Log;
  UINT8 *Download;
  UINTN Expected, Received;
  BOOLEAN Receiving, Complete, RebootRequested, ExitRequested;
} PIANO_FASTBOOT;

EFI_STATUS PianoFastbootInit(PIANO_FASTBOOT *State, VOID *Context,
                            PIANO_FB_SEND Send, PIANO_FB_LOG Log);
EFI_STATUS PianoFastbootPacket(PIANO_FASTBOOT *State, CONST VOID *Data, UINTN Bytes);
VOID PianoFastbootReset(PIANO_FASTBOOT *State);
EFI_STATUS PianoStartUsbDebug(VOID);
VOID PianoStopUsbDebug(VOID);
