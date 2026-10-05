// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastbootLaunch.h"
#include "PianoCpuInput.h"
#define PIANO_CPU_IMAGE_LOAN_SIGNATURE SIGNATURE_32('C','L','O','N')
typedef struct {
  UINT32 Signature;
  BOOLEAN Active,Retained,Busy;
  EFI_STATUS Status;
  PIANO_LAUNCH_BLOB Blob;
  VOID *Owner,*SourceLoan,*Token;
  CONST VOID *View;
  PIANO_BOOT_RANGE Range;
  UINTN Sequence;
  UINT8 Sha256[32];
  PIANO_CPU_INPUT_ENV Cpu;PIANO_LINUX_MEMORY_PROOF Memory;BOOLEAN HasCpu;
} PIANO_CPU_IMAGE_LOAN;
// Zero-initialize once, then retain driver lifetime. This object owns only a
// borrow, never a second ownership Take or a second full image allocation.
EFI_STATUS PianoCpuImageLoanInitialize(PIANO_CPU_IMAGE_LOAN *);
// Tokens come from one atomic driver-lifetime monotonic counter across all
// instances. No wrap/reuse; output parameters are written on success only and
// must not alias each other, state, callback descriptor or borrowed source.
EFI_STATUS PianoCpuImageLoanBegin(PIANO_CPU_IMAGE_LOAN *,CONST PIANO_LAUNCH_BLOB *,
  VOID *AlreadyOwned,PIANO_BOOT_RANGE,UINT64 MaxBytes,CONST VOID **View,VOID **Token);
// Large source variant uses the same live memory proof/owner validator as
// FileSource/LinuxSession. NULL Cpu retains64MiB; no new source allocation.
EFI_STATUS PianoCpuImageLoanBeginWithCpu(PIANO_CPU_IMAGE_LOAN *,CONST PIANO_LAUNCH_BLOB *,
  VOID *AlreadyOwned,PIANO_BOOT_RANGE,UINT64 MaxBytes,CONST PIANO_CPU_INPUT_ENV *,CONST VOID **View,VOID **Token);
// CPU-only bounded copies; destination cannot alias source/session storage.
EFI_STATUS PianoCpuImageLoanRead(PIANO_CPU_IMAGE_LOAN *,VOID *Token,UINT64 Offset,UINTN Bytes,VOID *);
EFI_STATUS PianoCpuImageLoanRevalidate(PIANO_CPU_IMAGE_LOAN *,VOID *Token);
// Caller must first retire every consumer of the returned CPU view. This is a
// source-lifetime release, not image cleanup, ACK, DMA or EBS authorization.
EFI_STATUS PianoCpuImageLoanEnd(PIANO_CPU_IMAGE_LOAN *,VOID *Token);
