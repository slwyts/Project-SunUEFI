// SPDX-License-Identifier: BSD-2-Clause-Patent
// Protocol methods without installing handles or touching hardware/BS.
#ifndef PIANO_POGO_INPUT_H
#define PIANO_POGO_INPUT_H
#include "PianoPogoReport.h"
#define PIANO_POGO_NOTIFIES 8

typedef VOID (*PIANO_POGO_SIGNAL)(VOID *Context, EFI_EVENT Event);
typedef struct {
  BOOLEAN Used;
  EFI_KEY_DATA Match;
  EFI_KEY_NOTIFY_FUNCTION Function;
} PIANO_POGO_NOTIFY;

typedef struct {
  PIANO_POGO_INPUT Core;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL Text;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL TextEx;
  EFI_SIMPLE_POINTER_PROTOCOL Pointer;
  EFI_SIMPLE_POINTER_MODE PointerMode;
  EFI_ABSOLUTE_POINTER_PROTOCOL Absolute;
  EFI_ABSOLUTE_POINTER_MODE AbsoluteMode;
  PIANO_POGO_NOTIFY Notifies[PIANO_POGO_NOTIFIES];
  PIANO_POGO_SIGNAL Signal;
  VOID *SignalContext;
  BOOLEAN Feeding;
} PIANO_POGO_ADAPTER;

EFI_STATUS PianoPogoInitializeAdapter(IN OUT PIANO_POGO_ADAPTER *Adapter,
                                     IN CONST EFI_SIMPLE_POINTER_MODE *VerifiedPointerMode,
                                     IN EFI_EVENT KeyEvent, IN EFI_EVENT KeyExEvent,
                                     IN EFI_EVENT PointerEvent, IN EFI_EVENT AbsoluteEvent,
                                     IN PIANO_POGO_SIGNAL Signal OPTIONAL, IN VOID *SignalContext OPTIONAL);
EFI_STATUS PianoPogoFeedAdapter(IN OUT PIANO_POGO_ADAPTER *Adapter, IN CONST UINT8 *Frame, IN UINTN Bytes);
VOID PianoPogoSignalReady(IN OUT PIANO_POGO_ADAPTER *Adapter);
#endif
