/** @file
  Read the PMK8550 calendar before the OS takes ownership of SPMI.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

STATIC volatile UINT32 mReading;
STATIC BOOLEAN mOutstanding;

// SPMI EXT_READL on the EE0 observer channel. The writes to its command
// register request a read; no transfer is sent to a PMIC write channel.
STATIC EFI_STATUS
ReadObserver (UINTN Channel, UINT8 Offset, UINT8 Length, UINT32 *Data)
{
  // A timeout must not overwrite a read which the arbiter still owns.
  if (mOutstanding && (MmioRead32 (Channel + 8) & BIT0) == 0) return EFI_DEVICE_ERROR;
  mOutstanding = TRUE;
  MmioWrite32 (Channel, (1U << 27) | ((UINT32)Offset << 4) | (Length - 1));
  for (UINTN Us = 0; Us < 1000; ++Us) {
    UINT32 Status = MmioRead32 (Channel + 8);
    if ((Status & BIT0) != 0) {
      mOutstanding = FALSE;
      if ((Status & (BIT1 | BIT2 | BIT3)) != 0) return EFI_DEVICE_ERROR;
      *Data = MmioRead32 (Channel + 0x18);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }
  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
ReadRtcSeconds (UINTN Channel, UINT32 *Seconds)
{
  UINT32 Control, Low;
  EFI_STATUS Status = ReadObserver (Channel, 0x46, 1, &Control);
  if (EFI_ERROR (Status) || (Control & BIT7) == 0) return EFI_DEVICE_ERROR;

  Status = ReadObserver (Channel, 0x48, 4, Seconds);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadObserver (Channel, 0x48, 1, &Low);
  if (EFI_ERROR (Status)) return Status;
  // A carry out of the low byte can tear a four-byte RTC read.
  if ((UINT8)Low < (UINT8)*Seconds) return ReadObserver (Channel, 0x48, 4, Seconds);
  return EFI_SUCCESS;
}

STATIC EFI_RT_PROPERTIES_TABLE  mRuntimeProperties = {
  EFI_RT_PROPERTIES_TABLE_VERSION,
  sizeof (EFI_RT_PROPERTIES_TABLE),
  EFI_RT_SUPPORTED_GET_VARIABLE |
  EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
  EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
  EFI_RT_SUPPORTED_CONVERT_POINTER |
  EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
  EFI_RT_SUPPORTED_RESET_SYSTEM |
  EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
};

EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities OPTIONAL
  )
{
  UINT32 Raw;
  if (Time == NULL) return EFI_INVALID_PARAMETER;
  if (EfiAtRuntime ()) return EFI_UNSUPPORTED;
  UINT32 Channel = PcdGet32 (PcdPianoRtcObserverBase);
  if (Channel == 0) return EFI_UNSUPPORTED;
  UINT32 Offset = PcdGet32 (PcdPianoRtcOffsetSeconds);
  if (InterlockedCompareExchange32 (&mReading, 0, 1) != 0) return EFI_DEVICE_ERROR;
  EFI_STATUS Status = ReadRtcSeconds (Channel, &Raw);
  InterlockedCompareExchange32 (&mReading, 1, 0);
  if (EFI_ERROR (Status)) return Status;

  *Time = (EFI_TIME){ .TimeZone = 0 };
  EpochToEfiTime ((UINTN)Raw + (UINTN)Offset, Time);
  if (Capabilities != NULL) {
    Capabilities->Resolution = 1;
    // No board calibration is available; report the widest representable tolerance.
    Capabilities->Accuracy = MAX_UINT32;
    Capabilities->SetsToZero = FALSE;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  return (Time == NULL) ? EFI_INVALID_PARAMETER : EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  if ((Enabled == NULL) || (Pending == NULL) || (Time == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN   Enabled,
  IN EFI_TIME  *Time OPTIONAL
  )
{
  return (Enabled && (Time == NULL)) ? EFI_INVALID_PARAMETER : EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_RT_PROPERTIES_TABLE  *Properties;
  EFI_STATUS               Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid, (VOID **)&Properties);
  if (!EFI_ERROR (Status)) {
    if ((Properties->Version != EFI_RT_PROPERTIES_TABLE_VERSION) ||
        (Properties->Length < sizeof (*Properties)))
    {
      return EFI_COMPROMISED_DATA;
    }

    mRuntimeProperties.RuntimeServicesSupported = Properties->RuntimeServicesSupported;
  }

  // After ExitBootServices the OS owns SPMI and UFS. The capsule driver
  // publishes the architectural protocol but does not process capsules.
  mRuntimeProperties.RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_TIME |
                                                  EFI_RT_SUPPORTED_SET_TIME |
                                                  EFI_RT_SUPPORTED_GET_WAKEUP_TIME |
                                                  EFI_RT_SUPPORTED_SET_WAKEUP_TIME |
                                                  EFI_RT_SUPPORTED_SET_VARIABLE |
                                                  EFI_RT_SUPPORTED_UPDATE_CAPSULE |
                                                  EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES);
  return gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, &mRuntimeProperties);
}
