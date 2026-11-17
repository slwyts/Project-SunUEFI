// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_FASTBOOT_MAX_DOWNLOAD  (64U * 1024U * 1024U)
#define PIANO_FASTBOOT_DEFAULT_FETCH_BYTES 65536U
#define PIANO_FASTBOOT_MAX_FETCH_BYTES (256U * 1024U)
#define PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES (1024U * 1024U)
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
  // Lba is relative to the resolved partition. Bytes is block aligned and
  // no larger than PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES.
  // Backend handles its DMA/IoAlign and rejects changed media or stale tokens.
  EFI_STATUS (*ReadBlocks)(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer);
  EFI_STATUS (*PrepareWrite)(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info);
  EFI_STATUS (*WriteBlocks)(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,CONST VOID *Buffer);
  EFI_STATUS (*Flush)(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info);
  UINT32 MaxFetchBytes; // Zero selects the 64 KiB default.
} PIANO_FB_STORAGE;
typedef EFI_STATUS (*PIANO_FB_SEND)(VOID *Context, CONST VOID *Data, UINTN Bytes);
typedef EFI_STATUS (*PIANO_FB_LOG)(VOID *Context, PIANO_FB_SEND Send);
typedef struct PIANO_FASTBOOT PIANO_FASTBOOT;
typedef struct PIANO_FB_FLASH PIANO_FB_FLASH;
typedef struct {
  UINT64 Bytes,TotalUs,PrepareUs,WriteUs,ReadUs,FlushUs;
} PIANO_FB_FLASH_TIMING;
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
  BOOLEAN AllowRawLinux; // Opt-in: Android v2 Image/initrd/full-DTB, validated by the registered owner.
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
  UINT64 (*NowUs)(VOID *Context);
  VOID (*ReportFlash)(CONST CHAR8 *Partition,CONST PIANO_FB_FLASH_TIMING *Timing,CONST CHAR8 *Result);
  PIANO_FB_STORAGE Storage;
  PIANO_FB_FLASH *Flash;
  BOOLEAN FlashExecuting,FlashCancelRequested;
  PIANO_FB_BOOT Boot;
  PIANO_FB_BOOT_VIEW BootView;
  PIANO_FB_BOOT_PROOF BootProof;
  UINT8 *BootValidatedDownload;
  UINTN BootValidatedBytes;
  BOOLEAN BootPreparing,BootPending,BootTransferFrozen;
  UINTN Expected, Received;
  BOOLEAN Receiving, Complete, RebootRequested, ExitRequested;
  BOOLEAN DiagnosticsOnly,DiagnosticsRebootBlocked;
};

EFI_STATUS PianoFastbootInit(PIANO_FASTBOOT *State, VOID *Context,
                            PIANO_FB_SEND Send, PIANO_FB_LOG Log);
EFI_STATUS PianoFastbootPacket(PIANO_FASTBOOT *State, CONST VOID *Data, UINTN Bytes);
// Call at application TPL, only after queued replies drain.
// Each step performs at most one bounded write/readback or one final flush.
BOOLEAN PianoFastbootFlashPending(CONST PIANO_FASTBOOT *State);
EFI_STATUS PianoFastbootFlashStep(PIANO_FASTBOOT *State);
// Reset may synchronously flush a cancelled partition write. APP only with storage attached.
VOID PianoFastbootReset(PIANO_FASTBOOT *State);
// One-way restriction for a live error screen. Clears ordinary CPU transfers;
// a prepared or transferred boot payload must retain its existing ownership.
// Ordinary USB resets preserve this restriction. Diagnostic reads and a
// deferred reboot remain available; no new image or UI action is admitted.
EFI_STATUS PianoFastbootRestrictToDiagnostics(PIANO_FASTBOOT *State,BOOLEAN AllowReboot);
// Send must copy each frame before returning; it must never DMA this CPU pool.
EFI_STATUS PianoFastbootStageCopy(PIANO_FASTBOOT *State, CONST VOID *Data, UINTN Bytes);
// NULL unregisters. Merely registering callbacks does not advertise readiness.
EFI_STATUS PianoFastbootSetStorage(PIANO_FASTBOOT *State,CONST PIANO_FB_STORAGE *Storage);
// Backend readiness, not a guarantee that a particular partition is writable.
BOOLEAN PianoFastbootStorageWritable(PIANO_FASTBOOT *State);
EFI_STATUS PianoFastbootSetBoot(PIANO_FASTBOOT *State,CONST PIANO_FB_BOOT *Boot);
// Device run API: copies callbacks; caller keeps Context/returned Token alive.
EFI_STATUS PianoDwc3SetBootForExperiment(CONST PIANO_FB_BOOT *Boot);
// One-shot result after run; success proves a Take, not Controller/all-owner
// cleanup or execution. An error result can carry a retained partial Token.
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action);
EFI_STATUS PianoStartUsbDebug(VOID);
VOID PianoStopUsbDebug(VOID);
