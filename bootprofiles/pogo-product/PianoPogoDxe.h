// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../uefi-app/PianoPogoInput.h"
#include <Protocol/DevicePath.h>
typedef struct {EFI_STATUS Status;BOOLEAN Quiet,Clean,Retained;} PIANO_POGO_BACKEND_STOP;
typedef struct {
  UINT32 Revision;VOID *Context;
  // Real platform readiness: current exclusive SE6/MCU/FW/GPIO/clock/PIO proof.
  // Source-only parser existence or fabricated reports must never satisfy it.
  EFI_STATUS(EFIAPI *Ready)(VOID *);
  EFI_STATUS(EFIAPI *DataReady)(VOID *,BOOLEAN *);
  UINT64(EFIAPI *NowUs)(VOID *);
  EFI_STATUS(EFIAPI *Read68)(VOID *,UINT64,UINT8[PIANO_POGO_FRAME_BYTES],UINTN *);
  EFI_STATUS(EFIAPI *Stop)(VOID *,PIANO_POGO_BACKEND_STOP *);
} PIANO_POGO_DXE_BACKEND;
typedef struct {
  BOOLEAN Started,Published,Connected,Stopping,Stopped,Busy,ServicesLost,Retained,WorkPending;
  BOOLEAN InstallAttempted,EventsUnknown,StopAttempted;
  EFI_STATUS Status,Ready,Read,Connect,Disconnect,Uninstall,BackendStop,Events;
  UINT32 Reads,Frames,Notifications;UINT64 LastTime;
} PIANO_POGO_DXE_REPORT;
typedef struct {EFI_KEY_DATA Match;EFI_KEY_NOTIFY_FUNCTION Function;VOID *Token;} PIANO_POGO_DXE_NOTIFY;
typedef struct {
  UINT32 Signature;PIANO_POGO_DXE_BACKEND Backend;PIANO_POGO_DXE_REPORT Report;
  PIANO_POGO_ADAPTER Adapter;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL OriginalText;EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL OriginalEx;
  EFI_SIMPLE_POINTER_PROTOCOL OriginalPointer;EFI_ABSOLUTE_POINTER_PROTOCOL OriginalAbsolute;
  EFI_HANDLE Handle;EFI_EVENT Wait[4],Timer,Exit;
  struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} Path;
  PIANO_POGO_DXE_NOTIFY Notify[PIANO_POGO_NOTIFIES];UINTN NotifySequence;
  PIANO_POGO_BACKEND_STOP StopReport;
} PIANO_POGO_DXE;
// New zeroed caller/driver lifetime storage. No backend Read/Ready in callbacks.
EFI_STATUS PianoPogoDxeStart(PIANO_POGO_DXE *,CONST PIANO_POGO_DXE_BACKEND *,CONST EFI_SIMPLE_POINTER_MODE *);
// Shared product APP pump calls this; it alone reads actual transport reports.
EFI_STATUS PianoPogoDxePump(PIANO_POGO_DXE *);
EFI_STATUS PianoPogoDxeStop(PIANO_POGO_DXE *);
CONST PIANO_POGO_DXE_REPORT *PianoPogoDxeReport(CONST PIANO_POGO_DXE *);
