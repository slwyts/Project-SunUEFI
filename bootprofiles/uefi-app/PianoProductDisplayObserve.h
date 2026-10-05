// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_PRODUCT_DISPLAY_OBSERVE_H
#define PIANO_PRODUCT_DISPLAY_OBSERVE_H
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#define PIANO_DISPLAY_OBSERVE_REVISION 1U
#define PIANO_DISPLAY_OBSERVE_PHASES 8U
#define PIANO_DISPLAY_OBSERVE_GOPS 16U
#define PIANO_DISPLAY_OBSERVE_PHASE_BYTES 32U
typedef BOOLEAN (EFIAPI *PIANO_PRODUCT_DISPLAY_ALIVE)(VOID);
typedef struct {
  EFI_STATUS InterfaceStatus,ModeStatus;
  UINTN Handle,Interface,BltPc,ModePointer,InfoPointer;
  UINT64 FrameBufferBase,FrameBufferSize;
  UINT32 Mode,MaxMode,Width,Height,PixelFormat,PixelsPerScanLine;
  BOOLEAN Preferred,ConOut;
} PIANO_PRODUCT_DISPLAY_GOP;
typedef struct {
  CHAR8 Phase[PIANO_DISPLAY_OBSERVE_PHASE_BYTES];
  EFI_STATUS Status,PreferredStatus,ConOutStatus,EnumerationStatus,FreeStatus;
  UINTN PreferredInterface,ConOutInterface,HandleCount,RecordedCount;
  BOOLEAN Retained,ServicesLost;
  PIANO_PRODUCT_DISPLAY_GOP Gop[PIANO_DISPLAY_OBSERVE_GOPS];
} PIANO_PRODUCT_DISPLAY_SNAPSHOT;
typedef struct {
  UINT32 Revision,Count;
  BOOLEAN Retained,ServicesLost;
  UINTN RetainedHandleBuffer; // opaque diagnostic address, never dereferenced
  PIANO_PRODUCT_DISPLAY_SNAPSHOT Snapshot[PIANO_DISPLAY_OBSERVE_PHASES];
} PIANO_PRODUCT_DISPLAY_REPORT;
/* Bounded protocol/CPU metadata observation only. No GOP method, framebuffer,
 * MMIO, AT, GCD, mapping, allocation ownership or display readiness is granted. */
EFI_STATUS PianoProductDisplayObserve(CONST CHAR8 *Phase,PIANO_PRODUCT_DISPLAY_ALIVE Alive);
/* Reemit immutable CPU snapshots; never re-enumerates/dereferences providers. */
EFI_STATUS PianoProductDisplayReemit(PIANO_PRODUCT_DISPLAY_ALIVE Alive);
BOOLEAN PianoProductDisplayRetained(VOID);
CONST PIANO_PRODUCT_DISPLAY_REPORT *PianoProductDisplayGetReport(VOID);
#endif
