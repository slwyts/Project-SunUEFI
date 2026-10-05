// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_FASTBOOT_MAX_DOWNLOAD  (64U * 1024U * 1024U)
#define PIANO_FASTBOOT_MAX_FETCH 65536U
#define PIANO_FASTBOOT_PARTITION_NAME 36U
#ifndef PIANO_USB_SERVICE
#define PIANO_USB_SERVICE 0
#endif
#if PIANO_USB_SERVICE != 0 && PIANO_USB_SERVICE != 1
#error PIANO_USB_SERVICE must be exactly 0 or 1
#endif
#ifndef PIANO_USB_RAM_BOOT
#define PIANO_USB_RAM_BOOT 0
#endif
#if PIANO_USB_RAM_BOOT != 0 && PIANO_USB_RAM_BOOT != 1
#error PIANO_USB_RAM_BOOT must be exactly 0 or 1
#endif
typedef struct {
  CHAR8 Name[PIANO_FASTBOOT_PARTITION_NAME+1];
  UINT64 Bytes;
  UINT32 BlockSize;
  BOOLEAN ReadOnly;
  VOID *Token;
} PIANO_FB_PARTITION_INFO;
typedef struct {
  VOID *Context;
  EFI_STATUS (*Ready)(VOID *Context);
  EFI_STATUS (*Info)(VOID *Context,CONST CHAR8 *ExactName,PIANO_FB_PARTITION_INFO *Info);
  // Lba is relative to the resolved partition. Bytes is a whole block count.
  // Backend handles its DMA/IoAlign and rejects changed media or stale tokens.
  EFI_STATUS (*ReadBlocks)(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer);
} PIANO_FB_STORAGE;
typedef EFI_STATUS (*PIANO_FB_SEND)(VOID *Context, CONST VOID *Data, UINTN Bytes);
typedef EFI_STATUS (*PIANO_FB_LOG)(VOID *Context, PIANO_FB_SEND Send);
typedef struct PIANO_FASTBOOT PIANO_FASTBOOT;
typedef struct {
  UINT64 Offset,Bytes;
  UINT32 ImageBytes;
  BOOLEAN Wrapped,KnownV4CliHeaderQuirk;
} PIANO_FB_BOOT_VIEW;
typedef struct {
  BOOLEAN AckCompleted,QueueEmpty,DeviceHalted,DmaFreed,DispatchFrozen;
  UINT32 AckBytes,DmaBuffersFreed;
} PIANO_FB_BOOT_PROOF;
typedef struct {
  VOID *Context;
  UINT64 MaxImageBytes;
  BOOLEAN AllowKnownV4CliHeaderQuirk;
  EFI_STATUS (*Ready)(VOID *Context);
  // Mandatory caller policy (e.g. first-test fixture SHA allowlist). Executed
  // after bounded structural parse/budget, before an OKAY is queued.
  EFI_STATUS (*Validate)(VOID *Context,CONST PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View);
  // Called only after the proof is populated. Move exclusive source ownership
  // to driver-lifetime storage; never LoadImage/StartImage from this callback.
  EFI_STATUS (*TakeAfterAck)(VOID *Context,PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View,VOID **Token);
} PIANO_FB_BOOT;
typedef struct {
  VOID *Context,*Token;
  PIANO_FB_BOOT_VIEW View;
  PIANO_FB_BOOT_PROOF Proof;
  EFI_STATUS Status;
  BOOLEAN Taken,Retained;
} PIANO_FB_BOOT_ACTION;
typedef EFI_STATUS (*PIANO_FB_QUERY)(VOID *Context, CONST CHAR8 *Name, CHAR8 Value[60]);
typedef EFI_STATUS (*PIANO_FB_DIAGNOSTIC)(VOID *Context, PIANO_FASTBOOT *State, CONST CHAR8 *Command);
struct PIANO_FASTBOOT {
  VOID *Context;
  PIANO_FB_SEND Send;
  PIANO_FB_LOG Log;
  UINT8 *Download;
  UINT8 *Upload;
  UINTN UploadBytes;
  BOOLEAN UploadBorrowed;
  PIANO_FB_QUERY Query;
  PIANO_FB_DIAGNOSTIC Diagnostic;
  PIANO_FB_STORAGE Storage;
  PIANO_FB_BOOT Boot;
  PIANO_FB_BOOT_VIEW BootView;
  PIANO_FB_BOOT_PROOF BootProof;
  UINT8 *BootValidatedDownload;
  UINTN BootValidatedBytes;
  BOOLEAN BootPreparing,BootPending,BootTransferFrozen;
  UINTN Expected, Received;
  BOOLEAN Receiving, Complete, RebootRequested, ExitRequested;
};

EFI_STATUS PianoFastbootInit(PIANO_FASTBOOT *State, VOID *Context,
                            PIANO_FB_SEND Send, PIANO_FB_LOG Log);
EFI_STATUS PianoFastbootPacket(PIANO_FASTBOOT *State, CONST VOID *Data, UINTN Bytes);
VOID PianoFastbootReset(PIANO_FASTBOOT *State);
// Send must copy each frame before returning; it must never DMA this CPU pool.
EFI_STATUS PianoFastbootStageCopy(PIANO_FASTBOOT *State, CONST VOID *Data, UINTN Bytes);
// NULL unregisters. Merely registering callbacks does not advertise readiness.
EFI_STATUS PianoFastbootSetStorage(PIANO_FASTBOOT *State,CONST PIANO_FB_STORAGE *Storage);
EFI_STATUS PianoFastbootSetBoot(PIANO_FASTBOOT *State,CONST PIANO_FB_BOOT *Boot);
// Device run API: copies callbacks; caller keeps Context/returned Token alive.
EFI_STATUS PianoDwc3SetBootForExperiment(CONST PIANO_FB_BOOT *Boot);
// One-shot result after run; success proves a Take, not Controller/all-owner
// cleanup or execution. An error result can carry a retained partial Token.
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action);
EFI_STATUS PianoStartUsbDebug(VOID);
VOID PianoStopUsbDebug(VOID);
