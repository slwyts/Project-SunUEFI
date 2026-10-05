// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_FASTBOOT_MAX_DOWNLOAD  (64U * 1024U * 1024U)
#define PIANO_FASTBOOT_MAX_FETCH 65536U
#define PIANO_FASTBOOT_PARTITION_NAME 36U
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
EFI_STATUS PianoStartUsbDebug(VOID);
VOID PianoStopUsbDebug(VOID);
