// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoNvJournal.h"
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>
typedef struct {
  UINT32 Signature;
  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL Protocol;
  PIANO_NV_JOURNAL *Journal;
  EFI_FVB_ATTRIBUTES_2 Attributes;
  EFI_PHYSICAL_ADDRESS PhysicalBase;
  BOOLEAN Runtime,Busy,VirtualConverted,ConversionFailed;
} PIANO_NV_FVB;
// Blank standard system-NV FV and authenticated variable store, plus erased
// FTW working/spare. Pure memory formatter; never commits/provisions hardware.
EFI_STATUS PianoNvFvbBuildBlank(VOID *Buffer,UINTN Bytes);
EFI_STATUS PianoNvFvbInitialize(PIANO_NV_FVB *,PIANO_NV_JOURNAL *);
VOID PianoNvFvbFenceRuntime(PIANO_NV_FVB *);
// Called from runtime driver's VA event with the real ConvertPointer function.
// Translates only runtime memory/code pointers; the retired boot-only IO is
// explicitly cleared rather than converted or reused after EBS.
EFI_STATUS PianoNvFvbConvertVirtual(PIANO_NV_FVB *,EFI_CONVERT_POINTER);
// Pure standard-entry guard: NV creation/update/delete at runtime unsupported.
EFI_STATUS PianoNvRuntimeWriteCheck(BOOLEAN Runtime,UINT32 Attributes,BOOLEAN ExistingNv);
