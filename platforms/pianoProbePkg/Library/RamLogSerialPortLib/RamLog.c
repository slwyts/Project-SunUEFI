// SPDX-License-Identifier: BSD-2-Clause-Patent
// Append to the existing no-ECC Linux ramoops console ring in reserved RAM.
// piano DT: reg 0xA3500000/4 MiB, console-size 2 MiB, pmsg-size 2 MiB.
#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct {
  volatile UINT32 Signature;
  volatile UINT32 Start;
  volatile UINT32 Size;
  UINT8 Data[];
} RAM_CONSOLE;

VOID RamLogWrite (IN CONST UINT8 *Buffer, IN UINTN Length)
{
  RAM_CONSOLE *Ring = (RAM_CONSOLE *)(UINTN)0xA3500000;
  CONST UINT32 Capacity = 0x200000 - 12;
  UINT32 Start, Size;
  UINTN First;
  BOOLEAN Interrupts;
  if (Buffer == NULL || Length == 0) { return; }
  // Refuse a mismatched layout instead of initializing or erasing existing RAM.
  if (Ring->Signature != 0x43474244 || Ring->Start >= Capacity || Ring->Size > Capacity) { return; }
  if (Length > Capacity) { Buffer += Length - Capacity; Length = Capacity; }
  Interrupts = ArmGetInterruptState ();
  if (Interrupts) { ArmDisableInterrupts (); }
  Start = Ring->Start;
  Size = Ring->Size;
  First = MIN (Length, (UINTN)(Capacity - Start));
  CopyMem (Ring->Data + Start, Buffer, First);
  WriteBackInvalidateDataCacheRange (Ring->Data + Start, First);
  if (Length > First) {
    CopyMem (Ring->Data, Buffer + First, Length - First);
    WriteBackInvalidateDataCacheRange (Ring->Data, Length - First);
  }
  Ring->Start = (UINT32)((Start + Length) % Capacity);
  Ring->Size = (UINT32)MIN ((UINTN)Capacity, (UINTN)Size + Length);
  WriteBackInvalidateDataCacheRange (Ring, 12);
  if (Interrupts) { ArmEnableInterrupts (); }
}
