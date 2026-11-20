// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmbiosMemory.h"
#include <Library/BaseLib.h>
#include <assert.h>
#include <string.h>

static unsigned Count, FailAt;
static SMBIOS_TABLE_TYPE16 Array;
static SMBIOS_TABLE_TYPE17 Device;
static SMBIOS_TABLE_TYPE19 Map[4];

UINT64 EFIAPI ReadUnaligned64 (CONST VOID *Ptr) {
  UINT64 Value;
  memcpy(&Value, Ptr, sizeof(Value));
  return Value;
}
UINT64 EFIAPI SwapBytes64 (UINT64 Value) { return __builtin_bswap64(Value); }

static EFI_STATUS EFIAPI Add (const EFI_SMBIOS_PROTOCOL *This, EFI_HANDLE Producer,
                              EFI_SMBIOS_HANDLE *Handle, EFI_SMBIOS_TABLE_HEADER *Record) {
  (void)This; (void)Producer;
  assert(*Handle == SMBIOS_HANDLE_PI_RESERVED);
  if (Count == FailAt) return EFI_OUT_OF_RESOURCES;
  *Handle = (EFI_SMBIOS_HANDLE)(10 + Count);
  if (!Count) {
    assert(Record->Type == 16);
    memcpy(&Array, Record, sizeof(Array));
  } else if (Count == 1) {
    assert(Record->Type == 17);
    memcpy(&Device, Record, sizeof(Device));
    assert(Device.MemoryArrayHandle == 10);
    assert(!strcmp((const char *)Record + Record->Length, "Onboard"));
  } else {
    assert(Record->Type == 19 && Count - 2 < ARRAY_SIZE(Map));
    memcpy(&Map[Count - 2], Record, sizeof(Map[0]));
    assert(Map[Count - 2].MemoryArrayHandle == 10);
  }
  ++Count;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Remove (const EFI_SMBIOS_PROTOCOL *This, EFI_SMBIOS_HANDLE Handle) {
  (void)This;
  assert(Count && Handle == 10 + Count - 1);
  --Count;
  return EFI_SUCCESS;
}

static void Region (UINT8 *Bytes, UINT64 Base, UINT64 Size) {
  UINT64 Cells[] = { SwapBytes64(Base), SwapBytes64(Size) };
  memcpy(Bytes, Cells, sizeof(Cells));
}

int main(void) {
  EFI_SMBIOS_PROTOCOL Protocol = { .Add = Add, .Remove = Remove };
  PIANO_MEMORY_LAYOUT Layout = {
    .RamCount = 2, .Ram = {{SIZE_1GB, SIZE_1GB, 0, 0}, {SIZE_8TB, SIZE_4GB, 0, 0}},
    .DramBytes = SIZE_1GB + SIZE_4GB, .DdrSize = 16, .DdrProperties = BIT0 | BIT2,
    .DdrDeviceType = 8, .DdrRegions = BIT0 | BIT2
  };
  Region(Layout.DdrRegion[0], SIZE_1GB, SIZE_8GB);
  Region(Layout.DdrRegion[2], SIZE_8TB, SIZE_8GB);
  FailAt = MAX_UINT32;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Array.MaximumCapacity == SIZE_16GB / SIZE_1KB);
  assert(Device.Size == 16384 && Device.MemoryType == MemoryTypeLpddr5);
  assert(Map[0].StartingAddress == SIZE_1GB / SIZE_1KB);
  assert(Map[0].EndingAddress == (SIZE_1GB + SIZE_8GB) / SIZE_1KB - 1);
  assert(Map[1].StartingAddress == MAX_UINT32 && Map[1].EndingAddress == MAX_UINT32);
  assert(Map[1].ExtendedStartingAddress == SIZE_8TB && Map[1].ExtendedEndingAddress == SIZE_8TB + SIZE_8GB - 1);

  Count = 0;
  Layout.DdrSize = 24;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Device.Size == 16384);  // A changed size label with no added addresses.
  Count = 0;
  Layout.Ram[1].Bytes = SIZE_16GB;  // The new bank now extends beyond the old controller table.
  Layout.DramBytes = SIZE_1GB + SIZE_16GB;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Device.Size == 24576 && Map[0].EndingAddress == SIZE_2GB / SIZE_1KB - 1);
  assert(Map[1].ExtendedEndingAddress == SIZE_8TB + SIZE_16GB - 1);
  Count = 0;
  Layout.Ram[1].Bytes = SIZE_4GB;
  Layout.DramBytes = SIZE_1GB + SIZE_4GB;
  Layout.DdrSize = 8;  // A stale, larger controller window must not inflate a smaller SKU.
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Device.Size == 8192 && Map[1].ExtendedEndingAddress == SIZE_8TB + SIZE_4GB - 1);
  Count = 0;
  Layout.DdrSize = 12;
  Region(Layout.DdrRegion[2], SIZE_8TB, SIZE_4GB);
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Device.Size == 12288 && Map[0].EndingAddress == (SIZE_1GB + SIZE_8GB) / SIZE_1KB - 1);
  Count = 0;
  Layout.DdrProperties = 0;
  Region(Layout.DdrRegion[2], SIZE_8TB, SIZE_32GB - SIZE_8GB);
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Count == 4);
  assert(Device.Size == 0x7fff && Device.ExtendedSize == 32768 && Device.VolatileSize == SIZE_32GB);
  assert(Device.MemoryType == MemoryTypeUnknown);
  Count = 0;
  Layout.DdrProperties = BIT0;
  Layout.DdrSize = 4;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_SUCCESS && Device.VolatileSize == SIZE_32GB);
  Count = 0;
  FailAt = 3;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_OUT_OF_RESOURCES && Count == 0);
  Layout.DdrRegions = 0;
  assert(PianoSmbiosAddMemory(&Protocol, NULL, &Layout) == EFI_NOT_FOUND && Count == 0);
  return 0;
}
