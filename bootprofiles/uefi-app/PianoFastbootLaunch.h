// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastbootBoot.h"
#include <Protocol/LoadedImage.h>
// Take moves exclusive ownership out of the download state. No callback may
// recycle it during shutdown. BorrowView loans stable CPU memory only, never
// a DMA mapping; it may cover a future 1GiB arena without another full copy.
typedef struct {
  VOID *Context;
  UINT64 Bytes;
  EFI_STATUS (*Take)(VOID *Context,VOID **Owner);
  EFI_STATUS (*Read)(VOID *Context,VOID *Owner,UINT64 Offset,UINTN Bytes,VOID *Buffer);
  EFI_STATUS (*BorrowView)(VOID *Context,VOID *Owner,PIANO_BOOT_RANGE Range,CONST VOID **View,VOID **Loan);
  EFI_STATUS (*Unborrow)(VOID *Context,VOID *Owner,VOID *Loan);
  EFI_STATUS (*Restore)(VOID *Context,VOID *Owner); // Moves unchanged ownership to caller, not a restarted USB service.
  EFI_STATUS (*ZeroRelease)(VOID *Context,VOID *Owner); // Must zero then consume/free; exact success is the acknowledgement.
} PIANO_LAUNCH_BLOB;
typedef struct {
  VOID *Context;
  EFI_BOOT_SERVICES *Services;
  EFI_HANDLE ParentImage;
  UINT64 MaxImageBytes;
  UINT64 MaxSourceBytes; // Explicit independent budget; does not advertise transport capacity.
  EFI_STATUS (*ShutdownAll)(VOID *Context); // Typed all-owner shutdown, not halt-only.
  BOOLEAN (*BootServicesAlive)(VOID *Context); // CPU/runtime-safe check, no BS call.
  VOID (*FailStop)(VOID *Context,EFI_STATUS Status); // Runtime-only, never returns; CpuDeadLoop is fallback.
  BOOLEAN RestoreOnFailure;
  BOOLEAN AllowKnownV4CliHeaderQuirk;
} PIANO_LAUNCH_ENV;
typedef enum {PianoLaunchIdle,PianoLaunchOwned,PianoLaunchParsed,PianoLaunchShutdown,
  PianoLaunchBorrowed,PianoLaunchLoaded,PianoLaunchStarted,PianoLaunchReturned,
  PianoLaunchReleased,PianoLaunchRetained,PianoLaunchServicesLost} PIANO_LAUNCH_PHASE;
typedef struct {
  EFI_STATUS Status,CleanupStatus,ImageExitStatus;
  BOOLEAN ShutdownSucceeded,StartInvoked,AppReturned,ImageUnloaded;
  BOOLEAN BlobRestored,BlobZeroReleased,OptionsRestored,ExitSignalSeen,ResourcesRetained;
} PIANO_LAUNCH_RESULT;
// Must be zero-initialized and have driver lifetime, not an ephemeral stack address. A failed cleanup
// can retain the EBS event/source/image/options, so it must not be overwritten.
typedef struct {
  UINT32 Signature;
  BOOLEAN Busy,LostServices,UnknownOwnership;
  PIANO_LAUNCH_PHASE Phase;
  PIANO_LAUNCH_BLOB Blob;
  PIANO_LAUNCH_ENV Env;
  PIANO_BOOT_IMAGE Parsed;
  PIANO_LAUNCH_RESULT Result;
  VOID *Owner,*Loan;
  CONST VOID *View;
  EFI_HANDLE Image;
  EFI_EVENT ExitEvent;
  VOID *OptionsCopy,*OriginalOptions;
  UINT32 OptionsBytes,OriginalOptionsBytes;
  UINTN ExitDataBytes;
  CHAR16 *ExitData;
  BOOLEAN OptionsInstalled;
  BOOLEAN ImageIdentityKnown;
  VOID *LoadedProtocolIdentity,*ImageBaseIdentity;
  UINT64 ImageSizeIdentity;
} PIANO_FASTBOOT_LAUNCH;
EFI_STATUS PianoFastbootLaunchInit(PIANO_FASTBOOT_LAUNCH *State);
EFI_STATUS PianoFastbootLaunchRun(PIANO_FASTBOOT_LAUNCH *State,CONST PIANO_LAUNCH_ENV *Environment,
                               CONST PIANO_LAUNCH_BLOB *Blob,CONST VOID *LoadOptions,UINT32 LoadOptionsBytes);
