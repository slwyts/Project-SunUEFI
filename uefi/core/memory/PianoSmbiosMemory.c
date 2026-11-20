// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmbiosMemory.h"
#include <Library/BaseLib.h>
#include <Protocol/DDRDetails.h>

// ABL emits the DDR controller's physical base and byte count as big-endian
// cells. DDR ranks, controller addresses and the EFI free list are not DIMMs.
STATIC UINTN
ReadDdrRanges (CONST PIANO_MEMORY_LAYOUT *Layout, PIANO_MEMORY_SPAN *Ranges, UINT64 *Total)
{
  UINTN Count = 0;
  UINT64 Sum = 0;

  for (UINTN I = 0; I < PIANO_MEMORY_LAYOUT_MAX_DDR_REGIONS; ++I) {
    if (!(Layout->DdrRegions & (1U << I))) {
      continue;
    }
    CONST UINT8 *Entry = Layout->DdrRegion[I];
    UINT64 Base = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Entry));
    UINT64 Bytes = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)(Entry + 8)));
    if (!Bytes || Base > MAX_UINT64 - Bytes || Sum > MAX_UINT64 - Bytes) {
      return 0;
    }
    for (UINTN J = 0; J < Count; ++J) {
      if (Base < Ranges[J].Base + Ranges[J].Bytes && Ranges[J].Base < Base + Bytes) {
        return 0;
      }
    }
    Ranges[Count++] = (PIANO_MEMORY_SPAN){ .Base = Base, .Bytes = Bytes };
    Sum += Bytes;
  }

  // Only use the controller map if it accounts for all the RAM which ABL
  // made addressable this boot. A region may cross adjacent controller banks.
  for (UINTN I = 0; I < Layout->RamCount; ++I) {
    UINT64 Pos = Layout->Ram[I].Base;
    UINT64 End = Pos + Layout->Ram[I].Bytes;
    while (Pos < End) {
      UINTN J;
      for (J = 0; J < Count; ++J) {
        if (Pos >= Ranges[J].Base && Pos - Ranges[J].Base < Ranges[J].Bytes) {
          Pos = Ranges[J].Base + Ranges[J].Bytes;
          break;
        }
      }
      if (J == Count) {
        return 0;
      }
    }
  }
  *Total = Sum;
  return Count;
}

STATIC UINT8
MemoryType (CONST PIANO_MEMORY_LAYOUT *Layout)
{
  if (Layout->DdrProperties & BIT2) {
    switch (Layout->DdrDeviceType) {
      case DDR_TYPE_LPDDR4:
      case DDR_TYPE_LPDDR4X:
        return MemoryTypeLpddr4;
      case DDR_TYPE_LPDDR5:
      case DDR_TYPE_LPDDR5X:
        return MemoryTypeLpddr5;
    }
  }
  return MemoryTypeUnknown;
}

EFI_STATUS
PianoSmbiosAddMemory (EFI_SMBIOS_PROTOCOL *Smbios, EFI_HANDLE Producer, CONST PIANO_MEMORY_LAYOUT *Layout)
{
  PIANO_MEMORY_SPAN Ddr[PIANO_MEMORY_LAYOUT_MAX_DDR_REGIONS];
  CONST PIANO_MEMORY_SPAN *Ranges = Ddr;
  UINT64 Total = 0;
  UINTN Count = ReadDdrRanges (Layout, Ddr, &Total);
  UINT64 Installed = (Layout->DdrProperties & BIT0) ? (UINT64)Layout->DdrSize * SIZE_1GB : 0;

  if (Installed < Layout->DramBytes) {
    Installed = 0;  // An old capacity value cannot cap a larger live map.
  }
  if (Total % SIZE_1MB) {
    Count = 0;  // Not a complete installed capacity representable by Type 17.
  }
  if (Count && (!Installed || Total <= Installed)) {
    // A larger size label alone is not evidence of additional DDR. These
    // physical banks already account for all AP-visible addresses.
    Installed = Total;
  } else if (Count) {
    // The controller list includes capacity the populated device disclaims.
    // Keep the current device size and report only the confirmed AP ranges.
    Count = 0;
  }
  if (!Count) {
    // No complete controller map. Report the actual AP-addressable ranges,
    // never fill a hole from a capacity or infer soldered capacity by rounding.
    Ranges = Layout->Ram;
    Count = Layout->RamCount;
  }
  if (!Installed || Installed % SIZE_1MB ||
      Installed / SIZE_1MB > MAX_INT32 || !Count) {
    return EFI_NOT_FOUND;
  }

  struct { SMBIOS_TABLE_TYPE16 Table; CHAR8 End[2]; } Array = {
    .Table = {
      .Hdr = { SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY, sizeof (SMBIOS_TABLE_TYPE16), 0 },
      .Location = MemoryArrayLocationSystemBoard, .Use = MemoryArrayUseSystemMemory,
      .MemoryErrorCorrection = MemoryErrorCorrectionUnknown,
      .MemoryErrorInformationHandle = 0xfffe, .NumberOfMemoryDevices = 1
    }
  };
  struct { SMBIOS_TABLE_TYPE17 Table; CHAR8 Strings[sizeof ("Onboard") + 1]; } Device = {
    .Table = {
      .Hdr = { SMBIOS_TYPE_MEMORY_DEVICE, sizeof (SMBIOS_TABLE_TYPE17), 0 },
      .MemoryErrorInformationHandle = 0xfffe, .TotalWidth = MAX_UINT16, .DataWidth = MAX_UINT16,
      .FormFactor = MemoryFormFactorRowOfChips, .DeviceLocator = 1,
      .MemoryTechnology = MemoryTechnologyDram, .TypeDetail = { .Synchronous = 1 },
      .MemoryOperatingModeCapability.Bits.VolatileMemory = 1
    },
    .Strings = "Onboard"
  };
  Array.Table.MaximumCapacity = Installed < SIZE_2TB ? (UINT32)(Installed / SIZE_1KB) : BIT31;
  Array.Table.ExtendedMaximumCapacity = Installed < SIZE_2TB ? 0 : Installed;
  UINT32 MiB = (UINT32)(Installed / SIZE_1MB);
  Device.Table.Size = MiB < 0x7fff ? (UINT16)MiB : 0x7fff;
  Device.Table.ExtendedSize = MiB < 0x7fff ? 0 : MiB;
  Device.Table.VolatileSize = Installed;
  Device.Table.MemoryType = MemoryType (Layout);

  EFI_SMBIOS_HANDLE Handles[2 + PIANO_MEMORY_LAYOUT_MAX_RAM];
  UINTN Added = 0;
  Handles[0] = SMBIOS_HANDLE_PI_RESERVED;
  EFI_STATUS Status = Smbios->Add (Smbios, Producer, &Handles[0], &Array.Table.Hdr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  ++Added;
  Device.Table.MemoryArrayHandle = Handles[0];
  Handles[Added] = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, Producer, &Handles[Added], &Device.Table.Hdr);
  if (!EFI_ERROR (Status)) {
    ++Added;
    for (UINTN I = 0; I < Count; ++I) {
      UINT64 Base = Ranges[I].Base;
      UINT64 Last = Base + Ranges[I].Bytes - 1;
      struct { SMBIOS_TABLE_TYPE19 Table; CHAR8 End[2]; } Map = {
        .Table = {
          .Hdr = { SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS, sizeof (SMBIOS_TABLE_TYPE19), 0 },
          .MemoryArrayHandle = Handles[0], .PartitionWidth = 1
        }
      };
      if (Last / SIZE_1KB < MAX_UINT32 && !(Base % SIZE_1KB) && Last % SIZE_1KB == SIZE_1KB - 1) {
        Map.Table.StartingAddress = (UINT32)(Base / SIZE_1KB);
        Map.Table.EndingAddress = (UINT32)(Last / SIZE_1KB);
      } else {
        Map.Table.StartingAddress = Map.Table.EndingAddress = MAX_UINT32;
        Map.Table.ExtendedStartingAddress = Base;
        Map.Table.ExtendedEndingAddress = Last;
      }
      Handles[Added] = SMBIOS_HANDLE_PI_RESERVED;
      Status = Smbios->Add (Smbios, Producer, &Handles[Added], &Map.Table.Hdr);
      if (EFI_ERROR (Status)) {
        break;
      }
      ++Added;
    }
  }
  if (EFI_ERROR (Status)) {
    while (Added) {
      Smbios->Remove (Smbios, Handles[--Added]);
    }
  }
  return Status;
}
