// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
typedef struct {
  UINT32 Signature;
  BOOLEAN Busy,EbsObserved,Retained,StartCalled,StartReturned,AutoUnloaded;
  EFI_STATUS Result,Load,Start,Retire,Cleanup;
  EFI_HANDLE Image;
  EFI_EVENT ExitEvent;
  VOID *LoadedIdentity,*BaseIdentity;
  UINT64 SizeIdentity;
  VOID *OptionsCopy,*OriginalOptions,*ExitData;
  UINT32 OptionsBytes,OriginalOptionsBytes;
  UINTN ExitBytes;
  BOOLEAN OptionsInstalled;
  VOID *SourceOwned,*PathOwned,*HandlesOwned;
} PIANO_FV_APPLICATION;
// Context is zeroed driver-lifetime storage, never a returned stack frame.
// UI app launch preserves shared USB/UFS/input owners. This is not OS handoff.
EFI_STATUS PianoFvApplicationRun(EFI_HANDLE Parent,CONST EFI_GUID *File,
  CONST CHAR16 *Options,UINT64 MaxLoadedBytes,PIANO_FV_APPLICATION *Context);
// Buffer is already validated and held by the payload registry caller. This
// helper neither takes/free that source nor claims it belongs to an FV.
EFI_STATUS PianoApplicationRunBuffer(EFI_HANDLE Parent,CONST VOID *Image,UINTN Bytes,
  CONST CHAR16 *Options,UINT64 MaxLoadedBytes,PIANO_FV_APPLICATION *Context);
