// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/EFIClock.h>
#include <Protocol/LoadedImage.h>
#define PIANO_DISPLAY_CLOCK_LEASE_REVISION 1U
typedef struct {
  EFI_STATUS Status,EndStatus;
  UINT32 Reads,Pages;
  BOOLEAN Retained,ServicesLost;
  UINT32 Ahb[2],HfAxi[2]; // protected exact GCC127004 /127008 double reads
} PIANO_DISPLAY_CLOCK_LEASE_GCC;
#define PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_REVISION 1U
// Observation of the immediately preceding actual Reader call. No ownership
// permission is carried here; only first-text refusal before a guard begins
// can be cleaned by Lease. Unknown/stale/partial evidence remains retained.
typedef struct {
  UINT32 Revision;UINT64 Sequence,Address;UINTN Bytes;
  EFI_STATUS Status,MapStatus,EndStatus;
  BOOLEAN Busy,Retained,ServicesLost;
  UINT32 Sessions,GuardReads;
  BOOLEAN GuardActive,GuardSyncOwned,GuardSErrorOwned,GuardFatal,GuardRetained,GuardServicesLost;
} PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE;
typedef struct {
  VOID *Context;EFI_BOOT_SERVICES *Services;
  BOOLEAN (*BootServicesAlive)(VOID *);
  // Root actual bounded CPU reader: only verified native loaded data/text and
  // native low-heap owner spans. Exact SUCCESS copies all bytes; no remapping.
  EFI_STATUS (*ReadCpu)(VOID *,UINT64 Address,UINTN Bytes,VOID *Destination);
  // Reuse actual PianoGuardedRead Begin/4reads/End and report its exact result.
  // No native Clock call inside this callback; no MMIO writes or fake ready.
  EFI_STATUS (*ReadGcc)(VOID *,PIANO_DISPLAY_CLOCK_LEASE_GCC *);
  // Optional actual Reader report getter. Missing evidence never relaxes a
  // failed read. Exact SUCCESS must describe this call's address/bytes/status.
  EFI_STATUS (*GetReadFailureEvidence)(VOID *,UINT64,UINTN,EFI_STATUS,PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE *);
} PIANO_DISPLAY_CLOCK_LEASE_ENV;
typedef struct {
  UINT64 Global,Client,Module,Node,ClientRef;
  UINT32 Provider,Index,ModuleCount,ClockCount,GlobalFlags,NodeFlags;
  // native counter classes: ordinary[0], bit9-selected alternate[1]. Not two reads.
  UINT16 Total[2],PerClient[2];UINT8 ClientFlags;
  UINT32 MatchingSnapshots; // two full independent decoded snapshots compared equal
} PIANO_DISPLAY_CLOCK_LEASE_REFS;
#define PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF_REVISION 1U
// One real gcc_disp_ahb clock reference, not a rail/domain/MMCX or DPU lease.
// Native queries and its own short GCC guard complete before this is returned.
typedef struct {
  UINT32 Revision;UINT64 Token;VOID *LeaseContext;
  UINTN ClockId;UINT64 NativeBase;
  EFI_STATUS Status,Identity,CounterStatus,IsEnabled,GccStatus;
  BOOLEAN EnabledObserved;UINT32 OwnedReferences;
  PIANO_DISPLAY_CLOCK_LEASE_REFS Refs;
  PIANO_DISPLAY_CLOCK_LEASE_GCC Gcc;
} PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF;
typedef struct {
  UINT32 Revision;EFI_STATUS Status,Identity,Before,GetId,Enable,IsOn,After,Disable,Cleanup,CounterStatus,ReleaseReadbackStatus;
  UINT32 OwnedReferences;
  BOOLEAN Busy,Held,Retained,ServicesLost,AcquireAttempted,ReleaseAttempted,Released;
  UINTN ClockId;UINT64 NativeBase;
  PIANO_DISPLAY_CLOCK_LEASE_REFS Baseline,Acquired,ReleaseBefore,Retired;
  PIANO_DISPLAY_CLOCK_LEASE_GCC BeforeGcc,AfterGcc,ReleaseGcc;
  // Enabled is the mandatory native BOOL output. On is diagnostic: HWCG can
  // legitimately leave the CBCR top nibble8 while bit0 and our refs are held.
  EFI_STATUS IsEnabled;
  BOOLEAN EnabledObserved,OnObserved;
  EFI_STATUS ReadFailureEvidenceStatus;
  BOOLEAN CleanSourceRefusal;
  PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE ReadFailureEvidence;
} PIANO_DISPLAY_CLOCK_LEASE_REPORT;
typedef struct {
  UINT32 Signature;PIANO_DISPLAY_CLOCK_LEASE_ENV Env;
  PIANO_DISPLAY_CLOCK_LEASE_REPORT Report;
  EFI_CLOCK_PROTOCOL *Clock;EFI_HANDLE NativeImage;
  EFI_LOADED_IMAGE_PROTOCOL *ImageIdentity;VOID *ImageBase;UINT64 ImageSize;
  EFI_EVENT Exit;VOID *PinnedCopy;UINTN PinnedBytes;
  UINT64 ReadCpuCalls,BorrowToken;
  UINT8 LiveText[256];
} PIANO_DISPLAY_CLOCK_LEASE;
// Zeroed driver-lifetime state. Exactly one gcc_disp_ahb native reference, no
// rate/reset/MDP/GDSC operations. Existing product owners must explicitly bind
// this participant before an OS transition; this module never retires them.
EFI_STATUS PianoDisplayClockLeaseAcquire(PIANO_DISPLAY_CLOCK_LEASE *,CONST PIANO_DISPLAY_CLOCK_LEASE_ENV *);
// Strict one attempt. Exact native ref decrement + identity + guard evidence
// releases only the reference held here; never claims global hardware off.
EFI_STATUS PianoDisplayClockLeaseRelease(PIANO_DISPLAY_CLOCK_LEASE *);
// APP only, one live borrow at a time. Driver-lifetime tokens never repeat.
// These do not acquire/release a native ref or grant any memory/domain access.
EFI_STATUS PianoDisplayClockLeaseBorrowHeld(PIANO_DISPLAY_CLOCK_LEASE *,PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *);
EFI_STATUS PianoDisplayClockLeaseValidateHeld(PIANO_DISPLAY_CLOCK_LEASE *,UINT64 Token,PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *);
// Call only after the caller's guarded read has exactly ended. Fresh validation
// is mandatory on return; failure revokes the token and retains the clock owner.
EFI_STATUS PianoDisplayClockLeaseReturnHeld(PIANO_DISPLAY_CLOCK_LEASE *,UINT64 Token,PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *);
