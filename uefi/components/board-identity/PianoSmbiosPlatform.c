// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmbiosPlatform.h"
#include <Guid/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SmbiosStringTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Smbios.h>

STATIC EFI_STATUS
AddRecord (EFI_SMBIOS_PROTOCOL *Smbios, EFI_HANDLE Image, SMBIOS_STRUCTURE *Header,
           CONST CHAR8 **Values, UINTN Count, EFI_SMBIOS_HANDLE *Handle)
{
  STRING_TABLE Strings;
  EFI_STATUS Status = StringTableInitialize (&Strings, Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  SMBIOS_STRUCTURE *Record = NULL;
  for (UINTN I = 0; I < Count; ++I) {
    Status = StringTableAddString (&Strings, Values[I], NULL);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }
  Record = AllocateSmbiosRecord (Header->Length, &Strings);
  if (Record == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  CopyMem (Record, Header, Header->Length);
  Status = StringTablePublishStringSet (
    &Strings, (CHAR8 *)Record + Header->Length, StringTableGetStringSetSize (&Strings)
    );
  if (!EFI_ERROR (Status)) {
    *Handle = SMBIOS_HANDLE_PI_RESERVED;
    Status = Smbios->Add (Smbios, Image, Handle, Record);
  }
Done:
  if (Record != NULL) {
    FreePool (Record);
  }
  StringTableFree (&Strings);
  return Status;
}

STATIC EFI_STATUS
AddFirmware (EFI_SMBIOS_PROTOCOL *Smbios, EFI_HANDLE Image, EFI_SMBIOS_HANDLE *Handle)
{
  SMBIOS_TABLE_TYPE0 Bios = {
    .Hdr = { SMBIOS_TYPE_BIOS_INFORMATION, sizeof (SMBIOS_TABLE_TYPE0), 0 },
    .Vendor = 1, .BiosVersion = 2, .BiosReleaseDate = 3,
    .BiosCharacteristics.SelectableBootIsSupported = 1,
    .BIOSCharacteristicsExtensionBytes = { 0, BIT3 }, // UEFI firmware
    .SystemBiosMajorRelease = 0xff, .SystemBiosMinorRelease = 0xff,
    .EmbeddedControllerFirmwareMajorRelease = 0xff, .EmbeddedControllerFirmwareMinorRelease = 0xff
  };
  CONST UINT32 Bytes = FixedPcdGet32 (PcdFdSize);
  if (Bytes < SIZE_16MB) {
    Bios.BiosSize = (UINT8)((Bytes - 1) / SIZE_64KB);
  } else {
    Bios.BiosSize = 0xff;
    Bios.ExtendedBiosSize.Size = (UINT16)(Bytes / SIZE_1MB);
  }
  VOID *Acpi;
  if (!EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, &Acpi)) && Acpi != NULL) {
    Bios.BIOSCharacteristicsExtensionBytes[0] = BIT0;
  }
  CHAR8 Vendor[FixedPcdGetSize (PcdFirmwareVendor) / sizeof (CHAR16)];
  CHAR8 Version[FixedPcdGetSize (PcdFirmwareVersionString) / sizeof (CHAR16)];
  CHAR8 Date[sizeof ("MM/DD/YYYY")];
  CONST CHAR8 *Stamp = __DATE__;
  CONST CHAR8 Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  UINTN Month = 0;
  while (Month < 12 && CompareMem (Stamp, Months + Month * 3, 3) != 0) {
    ++Month;
  }
  // The C build date honours SOURCE_DATE_EPOCH in the product LLVM toolchain.
  AsciiSPrint (Date, sizeof (Date), "%02u/%02u/%a", (UINT32)Month + 1,
               (UINT32)AsciiStrDecimalToUintn (Stamp + 4), Stamp + 7);
  AsciiSPrint (Vendor, sizeof (Vendor), "%s", FixedPcdGetPtr (PcdFirmwareVendor));
  AsciiSPrint (Version, sizeof (Version), "%s", FixedPcdGetPtr (PcdFirmwareVersionString));
  CONST CHAR8 *Strings[] = { Vendor, Version, Date };
  return AddRecord (Smbios, Image, &Bios.Hdr, Strings, ARRAY_SIZE (Strings), Handle);
}

EFI_STATUS
PianoSmbiosInstallPlatform (EFI_HANDLE Image, CONST VOID *FactoryTree)
{
  EFI_SMBIOS_PROTOCOL *Smbios;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  // The validated ABL tree may carry a system serial, but a SoC identifier
  // is not a manufacturer-assigned system UUID or a tablet's retail SKU.
  INT32 Length;
  CONST CHAR8 *Serial = FdtGetProp (FactoryTree, 0, "serial-number", &Length);
  if (Serial == NULL || Length <= 1 || AsciiStrnLenS (Serial, (UINTN)Length) != (UINTN)Length - 1) {
    Serial = NULL;
  }
  CONST CHAR8 *Maker = FixedPcdGetPtr (PcdSmbiosSystemManufacturer);
  CONST CHAR8 *SystemStrings[] = { Maker, FixedPcdGetPtr (PcdSmbiosSystemModel), Serial };
  SMBIOS_TABLE_TYPE1 System = {
    .Hdr = { SMBIOS_TYPE_SYSTEM_INFORMATION, sizeof (SMBIOS_TABLE_TYPE1), 0 },
    .Manufacturer = 1, .ProductName = 2, .SerialNumber = Serial != NULL ? 3 : 0,
    .WakeUpType = SystemWakeupTypeUnknown
  };
  SMBIOS_TABLE_TYPE3 Chassis = {
    // No contained elements. The following byte is the absent chassis SKU.
    .Hdr = { SMBIOS_TYPE_SYSTEM_ENCLOSURE, OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements) + 1, 0 },
    .Manufacturer = 1, .Type = MiscChassisTablet,
    .BootupState = ChassisStateUnknown, .PowerSupplyState = ChassisStateUnknown,
    .ThermalState = ChassisStateUnknown, .SecurityStatus = ChassisSecurityStatusUnknown
  };
  CONST CHAR8 *BoardStrings[] = { Maker, FixedPcdGetPtr (PcdSmbiosSystemBoardModel) };
  SMBIOS_TABLE_TYPE2 Board = {
    .Hdr = { SMBIOS_TYPE_BASEBOARD_INFORMATION, OFFSET_OF (SMBIOS_TABLE_TYPE2, ContainedObjectHandles), 0 },
    .Manufacturer = 1, .ProductName = 2, .FeatureFlag.Motherboard = 1, .BoardType = BaseBoardTypeMotherBoard
  };
  EFI_SMBIOS_HANDLE Handles[4];
  UINTN Added = 0;
  Status = AddFirmware (Smbios, Image, &Handles[Added]);
  if (!EFI_ERROR (Status)) {
    Status = AddRecord (Smbios, Image, &System.Hdr, SystemStrings, Serial != NULL ? 3 : 2, &Handles[++Added]);
  }
  if (!EFI_ERROR (Status)) {
    Status = AddRecord (Smbios, Image, &Chassis.Hdr, &Maker, 1, &Handles[++Added]);
  }
  if (!EFI_ERROR (Status)) {
    Board.ChassisHandle = Handles[Added];
    Status = AddRecord (Smbios, Image, &Board.Hdr, BoardStrings, ARRAY_SIZE (BoardStrings), &Handles[++Added]);
  }
  if (EFI_ERROR (Status)) {
    while (Added != 0) {
      Smbios->Remove (Smbios, Handles[--Added]);
    }
  }
  return Status;
}
