// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/MtcVendor.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Protocol/MonotonicCounter.h>
#include <Protocol/PianoNvFile.h>

// Use the same nonvolatile variable, encoding and attributes as
// MdeModulePkg/Universal/MonotonicCounterRuntimeDxe.
#define MTC_ATTRIBUTES (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

STATIC EFI_GUID mNvFileGuid = PIANO_NV_FILE_PROTOCOL_GUID;
STATIC UINT32 mHigh, mLow, mPending;
STATIC BOOLEAN mReady, mCarry, mUpdating;

// Called without a raised TPL. mUpdating excludes another counter
// caller while SetVariable and file I/O can dispatch callbacks.
STATIC EFI_STATUS ReserveHigh(UINT32 *Reserved) {
  PIANO_NV_FILE_PROTOCOL *Files;
  EFI_STATUS Status = gBS->LocateProtocol(&mNvFileGuid, NULL, (VOID **)&Files);
  if (Status != EFI_SUCCESS || Files->Revision < PIANO_NV_FILE_REVISION || !Files->Sync || Files->Frozen)
    return EFI_DEVICE_ERROR;

  UINT32 Stored = 0, Attributes = 0;
  UINTN Size = sizeof(Stored);
  Status = gRT->GetVariable(MTC_VARIABLE_NAME, &gMtcVendorGuid, &Attributes, &Size, &Stored);
  if (Status == EFI_NOT_FOUND) {
    if (mReady) return EFI_DEVICE_ERROR;
    Stored = 0;
  } else if (Status != EFI_SUCCESS || Size != sizeof(Stored) || Attributes != MTC_ATTRIBUTES) {
    return EFI_DEVICE_ERROR;
  }
  if (mPending == 0) {
    if ((mReady && Stored != mHigh) || Stored == MAX_UINT32) return EFI_DEVICE_ERROR;
    mPending = Stored + 1;
  } else if (Stored != mPending && Stored != mPending - 1) {
    return EFI_DEVICE_ERROR;
  }
  if (Stored != mPending) {
    Status = gRT->SetVariable(MTC_VARIABLE_NAME, &gMtcVendorGuid, MTC_ATTRIBUTES, sizeof(mPending), &mPending);
    if (Status != EFI_SUCCESS) return EFI_DEVICE_ERROR;
  }
  // A failed commit may already have written one copy. Retry this same
  // value; never return a value that another boot could also issue.
  if (Files->Sync() != EFI_SUCCESS) return EFI_DEVICE_ERROR;
  *Reserved = mPending;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Next(BOOLEAN HighOnly, UINT64 *Count) {
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if (mUpdating) {
    gBS->RestoreTPL(Tpl);
    return EFI_DEVICE_ERROR;
  }
  if (HighOnly || !mReady || mCarry) {
    if (Tpl > TPL_CALLBACK) {
      gBS->RestoreTPL(Tpl);
      return EFI_DEVICE_ERROR;
    }
    mUpdating = TRUE;
    gBS->RestoreTPL(Tpl);
    UINT32 Reserved;
    EFI_STATUS Status = ReserveHigh(&Reserved);
    Tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    mUpdating = FALSE;
    if (Status != EFI_SUCCESS) {
      gBS->RestoreTPL(Tpl);
      return Status;
    }
    mHigh = Reserved;
    mLow = mPending = 0;
    mCarry = FALSE;
    mReady = TRUE;
  }
  if (HighOnly) {
    *Count = mHigh;
  } else {
    *Count = ((UINT64)mHigh << 32) | mLow;
    if (mLow == MAX_UINT32) mCarry = TRUE;
    else ++mLow;
  }
  gBS->RestoreTPL(Tpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI GetNextCount(OUT UINT64 *Count) {
  if (EfiAtRuntime()) return EFI_UNSUPPORTED;
  if (Count == NULL) return EFI_INVALID_PARAMETER;
  return Next(FALSE, Count);
}

STATIC EFI_STATUS EFIAPI GetNextHigh(OUT UINT32 *High) {
  if (EfiAtRuntime()) return EFI_UNSUPPORTED;
  if (High == NULL) return EFI_INVALID_PARAMETER;
  UINT64 Value;
  EFI_STATUS Status = Next(TRUE, &Value);
  if (Status == EFI_SUCCESS) *High = (UINT32)Value;
  return Status;
}

EFI_STATUS EFIAPI MonotonicCounterInitialize(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_HANDLE Handle = NULL;
  (VOID)Image;
  (VOID)SystemTable;
  gBS->GetNextMonotonicCount = GetNextCount;
  gRT->GetNextHighMonotonicCount = GetNextHigh;
  return gBS->InstallMultipleProtocolInterfaces(&Handle, &gEfiMonotonicCounterArchProtocolGuid, NULL, NULL);
}
