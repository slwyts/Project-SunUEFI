// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoFastboot.h"
typedef EFI_STATUS (*PIANO_USB_STORAGE_CHECK)(VOID *Context,CONST CHAR8 *Phase,BOOLEAN FullCapture);
#define PIANO_USB_SHUTDOWN_GUID {0x3D43C751,0xC239,0x42D5,{0x95,0x9C,0x75,0x21,0xF0,0x18,0x02,0x40}}
// Cold-reset fence only: bounded MMIO halt/readback, no BS/native calls or
// DMA retirement. Success proves DevCtrlHlt, never a complete owner teardown.
typedef struct {UINT64 Revision;EFI_STATUS (*Halt)(VOID);} PIANO_USB_SHUTDOWN;
// Configure only while stopped. Caller keeps the backend alive through halt.
EFI_STATUS PianoDwc3SetStorageForExperiment(CONST PIANO_FB_STORAGE *Storage);
EFI_STATUS PianoDwc3SetStorageCheckForExperiment(PIANO_USB_STORAGE_CHECK Check,VOID *Context);
EFI_STATUS PianoDwc3CheckStorageForExperiment(CONST CHAR8 *Phase,BOOLEAN FullCapture);
// Returns after USB teardown. Combined caller shuts UFS before acting on reboot.
EFI_STATUS PianoUsbControllerRunWithStorage(CONST VOID *Fdt,
                        CONST PIANO_FB_STORAGE *Storage,BOOLEAN *RebootRequested);
struct PIANO_SMMU_RETIRED_USB_PROOF;
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *Fdt,struct PIANO_SMMU_RETIRED_USB_PROOF *Proof);
// Exact private startup rollback ledger plus a fresh SMMU peer check. These
// APIs reject ordinary unavailable/unsupported USB and uncertain cleanup.
EFI_STATUS PianoUsbControllerGetStartupFailureProof(CONST VOID *Fdt,struct PIANO_SMMU_RETIRED_USB_PROOF *Proof);
EFI_STATUS PianoUsbControllerValidateStartupFailureProof(CONST VOID *Fdt,CONST struct PIANO_SMMU_RETIRED_USB_PROOF *Proof);
