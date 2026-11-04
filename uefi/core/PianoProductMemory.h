// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoCpuInput.h"

// Saved validation progress plus the existing SEC GUID HOBs. Reading this
// report does not capture a map, touch hardware, or grant memory authority.
// An index is MAX_UINTN when the saved stage does not concern a table row.
typedef struct {
  CONST CHAR8 *CaptureStage, *DtbStage;
  EFI_STATUS CaptureStatus, DtbStatus;
  UINTN CaptureIndex, DtbIndex;
  EFI_STATUS ColdHobStatus, ColdStatus, ColdPublishStatus;
  UINT32 ColdNativeCount, ColdCount, ColdRamCount, ColdReservedCount;
  UINT64 ColdEpoch, ColdDramBytes, ColdInputFingerprint;
  EFI_STATUS ObjectsHobStatus, ObjectsValidateStatus;
  EFI_STATUS ObjectsStatus, ObjectsPublishStatus;
  EFI_STATUS ObjectsHandoffStatus, ObjectsDtbStatus;
  UINT32 ObjectsReason, ObjectsCount;
  UINT64 ObjectsEpoch;
  BOOLEAN ObjectsPublished, ObjectsCoherent;
  EFI_STATUS EarlyHobStatus, EarlyStatus, EarlyPublishStatus, SmemStatus;
  UINT32 EarlyRecoveredFaults, EarlyRawPayloadBytes;
  UINT32 SmemReason, SmemRamVersion, SmemBankCount, SmemRawEntryCount;
  BOOLEAN EarlyFinished, EarlyPublished, EarlyColdStateVerified;
  BOOLEAN SmemParsed, RawPayloadCoherent;
  BOOLEAN SmemRepeatedMetadataEqual, SmemRepeatedPayloadEqual;
} PIANO_PRODUCT_MEMORY_DIAGNOSTIC;

VOID PianoProductMemoryGetDiagnostic(PIANO_PRODUCT_MEMORY_DIAGNOSTIC *Report);

// Check the cold producer's map against the live EFI allocation ledger. These
// functions allocate no pages and never convert or release occupied memory.
EFI_STATUS PianoProductMemoryCheck(VOID *Context,PIANO_LINUX_MEMORY_PROOF *Proof);
EFI_STATUS PianoProductMemoryValidate(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Proof);
// The final Linux tree must describe RAM/reservations compatible with the map
// admitted before the first MemoryPeim, including FDT reserve-map entries.
EFI_STATUS PianoProductMemoryValidateDtb(CONST VOID *Tree,UINTN Bytes);
