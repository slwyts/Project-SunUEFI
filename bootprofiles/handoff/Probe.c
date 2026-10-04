// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>

STATIC UINT64 ReadFdtInteger (IN CONST VOID *Fdt, IN INT32 Node, IN CONST CHAR8 *Name)
{
  INT32 Length;
  CONST UINT8 *Data = FdtGetProp (Fdt, Node, Name, &Length);
  UINT64 Value = 0;
  if (Data == NULL || (Length != 4 && Length != 8)) { return 0; }
  for (INT32 Index = 0; Index < Length; ++Index) { Value = (Value << 8) | Data[Index]; }
  return Value;
}

VOID PrintBootHandoff (VOID)
{
  CONST volatile UINT64 *Record = (CONST volatile UINT64 *)(UINTN)0xA7FFF000;
  EFI_MEMORY_REGION_DESCRIPTOR *Map;
  UINT8 Count;
  UINT64 Address;
  UINT64 Available = 0;
  CONST VOID *Fdt;
  INT32 Node;
  UINT64 Start;
  UINT64 End;

  Print (L"\r\nSunUEFI Linux handoff probe (RAM reads only)\r\n");
  if (Record[0] != 0x534E554546494448ULL) {
    Print (L"BootShim handoff record is absent.\r\n"); return;
  }
  Address = Record[1];
  Print (L"Bootloader EL%u, x0/DTB=0x%lx\r\n", (UINT32)(Record[2] >> 2), Address);
  DEBUG ((DEBUG_WARN, "SUNUEFI_HANDOFF EL%u DTB=0x%lx\n", (UINT32)(Record[2] >> 2), Address));
  GetMemoryMap (&Map, &Count);
  // Read only mapped, known RAM regions. Never dereference arbitrary MMIO.
  for (UINT8 Index = 0; Index < Count; ++Index) {
    EFI_MEMORY_REGION_DESCRIPTOR *Region = &Map[Index];
    if (AsciiStrCmp (Region->Name, "Kernel") && AsciiStrCmp (Region->Name, "DXE_Heap") &&
        AsciiStrCmp (Region->Name, "XBL_DT") && AsciiStrCmp (Region->Name, "UEFI_RESV")) { continue; }
    if (Address >= Region->Address && Address - Region->Address < Region->Length) {
      Available = Region->Length - (Address - Region->Address); break;
    }
  }
  if (Address == 0 || (Address & 7) || Available < 40) {
    Print (L"DTB pointer is outside the allowed mapped RAM regions; not read.\r\n"); return;
  }
  Fdt = (CONST VOID *)(UINTN)Address;
  if (FdtCheckHeader (Fdt) != 0 || FdtTotalSize (Fdt) > Available || FdtTotalSize (Fdt) > 0x200000) {
    Print (L"DTB header/size is invalid; not traversed.\r\n"); return;
  }
  Print (L"Valid runtime DTB, size=%u bytes\r\n", FdtTotalSize (Fdt));
  DEBUG ((DEBUG_WARN, "SUNUEFI_DTB_VALID size=%u\n", FdtTotalSize (Fdt)));
  Node = FdtPathOffset (Fdt, "/chosen");
  if (Node < 0) { Print (L"Runtime DTB has no chosen node.\r\n"); return; }
  Start = ReadFdtInteger (Fdt, Node, "linux,initrd-start");
  End = ReadFdtInteger (Fdt, Node, "linux,initrd-end");
  Print (L"Runtime initrd: 0x%lx .. 0x%lx\r\n", Start, End);
  DEBUG ((DEBUG_WARN, "SUNUEFI_INITRD start=0x%lx end=0x%lx size=%lu\n", Start, End, End > Start ? End - Start : 0));
  if (End > Start) { Print (L"Runtime initrd size: %lu bytes\r\n", End - Start); }
}
