// SPDX-License-Identifier: BSD-2-Clause-Patent
// Piano WN8030 runtime input core. No transport, GPIO, firmware or auth writes.
#ifndef PIANO_POGO_REPORT_H
#define PIANO_POGO_REPORT_H

#include <Uefi.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/AbsolutePointer.h>

#define PIANO_POGO_FRAME_BYTES 68
#define PIANO_POGO_KEY_QUEUE 64
#define PIANO_POGO_CONTROL_QUEUE 16
#define PIANO_POGO_MAX_REPORTS 16
#define PIANO_POGO_TOUCH_MAX_X 3199
#define PIANO_POGO_TOUCH_MAX_Y 2135

typedef enum {
  PianoPogoConsumer,
  PianoPogoAttachment,
  PianoPogoHinge,
  PianoPogoAuthRequired,
  PianoPogoAuthUidAvailable,
  PianoPogoAuthChallengeAvailable,
  PianoPogoVendorUnknown
} PIANO_POGO_CONTROL_KIND;

typedef struct {
  PIANO_POGO_CONTROL_KIND Kind;
  UINT8 Command;
  UINT16 Value;
  INT16 X, Y, Z;
} PIANO_POGO_CONTROL_EVENT;

typedef struct {
  UINT8 Flags, Id;
  UINT16 Pressure, X, Y;
} PIANO_POGO_CONTACT;

typedef struct {
  EFI_KEY_DATA Keys[PIANO_POGO_KEY_QUEUE];
  UINTN KeyHead, KeyCount;
  PIANO_POGO_CONTROL_EVENT Controls[PIANO_POGO_CONTROL_QUEUE];
  UINTN ControlHead, ControlCount;
  UINT8 PreviousKeys[6], Modifiers;
  EFI_KEY_TOGGLE_STATE Toggle;
  UINT16 ConsumerUsage;
  BOOLEAN AttachKnown, Attached;
  EFI_SIMPLE_POINTER_STATE Relative;
  BOOLEAN RelativePending;
  EFI_ABSOLUTE_POINTER_STATE Absolute;
  BOOLEAN AbsolutePending, PrimaryContactKnown;
  UINT8 PrimaryContactId, ContactCount;
  PIANO_POGO_CONTACT Contacts[3];
  UINT32 AcceptedFrames, RejectedFrames, QueueOverflows, DroppedControls;
} PIANO_POGO_INPUT;

VOID PianoPogoReset(IN OUT PIANO_POGO_INPUT *Input);
EFI_STATUS PianoPogoFeedFrame(IN OUT PIANO_POGO_INPUT *Input, IN CONST UINT8 *Frame, IN UINTN Bytes);
EFI_STATUS PianoPogoReadKeyEx(IN OUT PIANO_POGO_INPUT *Input, OUT EFI_KEY_DATA *Key);
EFI_STATUS PianoPogoReadPointer(IN OUT PIANO_POGO_INPUT *Input, OUT EFI_SIMPLE_POINTER_STATE *State);
EFI_STATUS PianoPogoReadAbsolute(IN OUT PIANO_POGO_INPUT *Input, OUT EFI_ABSOLUTE_POINTER_STATE *State);
EFI_STATUS PianoPogoReadControl(IN OUT PIANO_POGO_INPUT *Input, OUT PIANO_POGO_CONTROL_EVENT *Event);
EFI_STATUS PianoPogoSetToggle(IN OUT PIANO_POGO_INPUT *Input, IN EFI_KEY_TOGGLE_STATE Toggle);
VOID PianoPogoGetKeyState(IN CONST PIANO_POGO_INPUT *Input, OUT EFI_KEY_STATE *State);

#endif
