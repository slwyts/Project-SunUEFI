/** @file
  Report unsupported firmware time services without inventing a wall clock.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/RealTimeClockLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_RT_PROPERTIES_TABLE  mRuntimeProperties = {
  EFI_RT_PROPERTIES_TABLE_VERSION,
  sizeof (EFI_RT_PROPERTIES_TABLE),
  EFI_RT_SUPPORTED_GET_VARIABLE |
  EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
  EFI_RT_SUPPORTED_SET_VARIABLE |
  EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
  EFI_RT_SUPPORTED_CONVERT_POINTER |
  EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
  EFI_RT_SUPPORTED_RESET_SYSTEM |
  EFI_RT_SUPPORTED_UPDATE_CAPSULE |
  EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES |
  EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
};

EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities OPTIONAL
  )
{
  return (Time == NULL) ? EFI_INVALID_PARAMETER : EFI_UNSUPPORTED;
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

  Status = EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid, (VOID **)&Properties);
  if (!EFI_ERROR (Status)) {
    if ((Properties->Version != EFI_RT_PROPERTIES_TABLE_VERSION) ||
        (Properties->Length < sizeof (*Properties)))
    {
      return EFI_COMPROMISED_DATA;
    }

    mRuntimeProperties.RuntimeServicesSupported = Properties->RuntimeServicesSupported;
  }

  // Preserve other services' capability bits; only the four time calls are absent.
  mRuntimeProperties.RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_TIME |
                                                  EFI_RT_SUPPORTED_SET_TIME |
                                                  EFI_RT_SUPPORTED_GET_WAKEUP_TIME |
                                                  EFI_RT_SUPPORTED_SET_WAKEUP_TIME);
  return gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, &mRuntimeProperties);
}
