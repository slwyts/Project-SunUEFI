// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../uefi-app/PianoFastbootLaunch.h"
#include <Protocol/SimpleFileSystem.h>
#define PIANO_BOOT_FILE_LOW_BUDGET (64ULL*1024*1024)
#define PIANO_BOOT_FILE_PATH_CHARS 260U
typedef struct {
  VOID *Context;EFI_BOOT_SERVICES *Services;
  BOOLEAN(*BootServicesAlive)(VOID *); // real caller CPU-only EBS/lifetime fence
} PIANO_BOOT_FILE_ENV;
typedef struct {
  EFI_HANDLE FileSystem;
  CONST CHAR16 *AbsolutePath; // fixed configured path, never directory enumeration
  UINT64 MaxBytes;
  CONST UINT8 *ExpectedSha256; // optional trusted32-byte pin; copied at entry
} PIANO_BOOT_FILE_SPEC;
typedef struct {
  UINT32 Signature;PIANO_BOOT_FILE_ENV Env;
  BOOLEAN Busy,Ready,Taken,Consumed,Retained,ServicesLost,CloseAttempted,ReleaseAttempted;
  EFI_STATUS Status,Cleanup,FileClose,RootClose,Release;
  EFI_HANDLE FileSystem;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL *Root,*File;EFI_EVENT Exit;
  EFI_FILE_CLOSE CloseRoot,CloseFile;
  EFI_FILE_READ ReadFile;EFI_FILE_SET_POSITION SeekFile;EFI_FILE_GET_POSITION PositionFile;EFI_FILE_GET_INFO InfoFile;
  UINT8 *Data;UINTN Bytes;UINT8 Sha256[32],ExpectedSha256[32];BOOLEAN HasExpected;
  CHAR16 Path[PIANO_BOOT_FILE_PATH_CHARS];
  UINT64 Info[512],InitialInfo[512],ShaContext[512];UINTN InfoBytes;
  VOID *Owner,*Loan;UINT64 Chunks;
} PIANO_BOOT_FILE_SOURCE;
// Exactly one CPU allocation, full incremental SHA, files closed before Ready.
// State is zeroed producer-lifetime storage; retained states are never retried.
EFI_STATUS PianoBootFileLoad(PIANO_BOOT_FILE_SOURCE *,CONST PIANO_BOOT_FILE_ENV *,CONST PIANO_BOOT_FILE_SPEC *,
  PIANO_BOOT_SOURCE *Reader,PIANO_LAUNCH_BLOB *Blob);
// Export same immutable snapshot after a successful Load, no filesystem I/O.
EFI_STATUS PianoBootFileExport(PIANO_BOOT_FILE_SOURCE *,PIANO_BOOT_SOURCE *,PIANO_LAUNCH_BLOB *);
// Release untaken snapshot; refuses outstanding Owner/Loan. Blob owner uses
// ZeroRelease after Unborrow, or Restore to return unchanged source to caller.
EFI_STATUS PianoBootFileDispose(PIANO_BOOT_FILE_SOURCE *);
// Same env and explicit paths, sum(MaxBytes)<=Budget<=64MiB. No Start/EBS.
EFI_STATUS PianoBootFileLoadBundle(PIANO_BOOT_FILE_SOURCE *,UINTN Count,CONST PIANO_BOOT_FILE_ENV *,
  CONST PIANO_BOOT_FILE_SPEC *,UINT64 Budget,PIANO_BOOT_SOURCE *,PIANO_LAUNCH_BLOB *);
