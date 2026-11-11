// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoProductOwners.h"
#include <Protocol/LoadedImage.h>

// This is the fixed, reserved Kernel-window loader, not full-DDR EFI authority.
// Only the resident Core's acknowledged USB download session produces it.
typedef struct {
  UINT32 Revision;UINT64 Epoch,Kernel,KernelBytes,KernelSpan,Dtb,DtbBytes,Initrd,InitrdBytes;
  EFI_HANDLE Image;CONST EFI_LOADED_IMAGE_PROTOCOL *Identity;VOID *Token;
  EFI_HANDLE ExitImage;CONST EFI_LOADED_IMAGE_PROTOCOL *ExitIdentity;
  UINT8 KernelSha[32],InitrdSha[32],DtbSha[32];
} PIANO_RAW_LINUX_REPORT;
EFI_STATUS PianoRawLinuxRegister(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable);
VOID PianoRawLinuxReplayStable(VOID);
// Core retains payload ownership; ExitImage is the actual current EFI image.
EFI_STATUS PianoRawLinuxPrepare(PIANO_PRODUCT_OWNERS *,EFI_HANDLE CoreImage,EFI_HANDLE ExitImage,CONST PIANO_RAW_LINUX_REPORT **);
// CPU-only identity/ownership check, including the actual complete owner ledger.
BOOLEAN PianoRawLinuxPrepared(CONST PIANO_RAW_LINUX_REPORT *,CONST PIANO_PRODUCT_OWNERS *,EFI_HANDLE);
// Called only after LateHandoffArmRaw accepts the prepared report. Uses the
// standard EBS call (including its product hook), then never returns on success.
EFI_STATUS PianoRawLinuxEnter(CONST PIANO_RAW_LINUX_REPORT *);
