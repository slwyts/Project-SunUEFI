// SPDX-License-Identifier: BSD-2-Clause-Patent
// Expose PI's required architectural protocol, with no capsule processing.
#include <Uefi.h>
#include <Protocol/Capsule.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC EFI_STATUS EFIAPI UnsupportedUpdateCapsule (
  IN EFI_CAPSULE_HEADER **Headers, IN UINTN Count, IN EFI_PHYSICAL_ADDRESS ScatterGatherList)
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI UnsupportedQueryCapsule (
  IN EFI_CAPSULE_HEADER **Headers, IN UINTN Count,
  OUT UINT64 *MaximumCapsuleSize, OUT EFI_RESET_TYPE *ResetType)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI CapsuleArchNullEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE Handle = NULL;
  gRT->UpdateCapsule = UnsupportedUpdateCapsule;
  gRT->QueryCapsuleCapabilities = UnsupportedQueryCapsule;
  return gBS->InstallProtocolInterface (&Handle, &gEfiCapsuleArchProtocolGuid,
                                        EFI_NATIVE_INTERFACE, NULL);
}
