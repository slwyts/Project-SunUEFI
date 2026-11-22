// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <PiDxe.h>
#include <IndustryStandard/PeImage.h>
#include <Library/BaseLib.h>
#include "FirmwareVolume.h"

typedef struct {
  CONST EFI_IMAGE_SECTION_HEADER *Sections;
  UINTN Count;
  UINTN Base;
  UINTN Entry;
} XIP_IMAGE;

STATIC BOOLEAN
Contains (UINTN Begin, UINTN Size, UINTN Address, UINTN Bytes)
{
  return Address >= Begin && Address - Begin <= Size && Bytes <= Size - (Address - Begin);
}

STATIC EFI_STATUS
ReadImage (UINT8 Type, CONST UINT8 *Data, UINTN Size, XIP_IMAGE *Image)
{
  UINTN Sections;
  if (Type == EFI_SECTION_TE) {
    CONST EFI_TE_IMAGE_HEADER *Te = (CONST VOID *)Data;
    if (Size < sizeof (*Te) || Te->Signature != EFI_TE_IMAGE_HEADER_SIGNATURE ||
        Te->Machine != IMAGE_FILE_MACHINE_ARM64 || Te->StrippedSize < sizeof (*Te) ||
        (UINTN)Data < Te->StrippedSize - sizeof (*Te)) {
      return EFI_LOAD_ERROR;
    }
    Image->Base = (UINTN)Data - (Te->StrippedSize - sizeof (*Te));
    if (Te->ImageBase != Image->Base) {
      return EFI_LOAD_ERROR;
    }
    Image->Count = Te->NumberOfSections;
    Image->Entry = Te->AddressOfEntryPoint;
    Sections = sizeof (*Te);
  } else {
    CONST EFI_IMAGE_DOS_HEADER *Dos = (CONST VOID *)Data;
    CONST EFI_IMAGE_NT_HEADERS64 *Pe;
    UINTN Offset = 0;
    if (Size >= sizeof (*Dos) && Dos->e_magic == EFI_IMAGE_DOS_SIGNATURE) {
      Offset = Dos->e_lfanew;
    }
    if (!Contains (0, Size, Offset, sizeof (*Pe))) {
      return EFI_LOAD_ERROR;
    }
    Pe = (CONST VOID *)(Data + Offset);
    if (Pe->Signature != EFI_IMAGE_NT_SIGNATURE || Pe->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64 ||
        Pe->OptionalHeader.Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        Pe->FileHeader.SizeOfOptionalHeader < sizeof (Pe->OptionalHeader) ||
        Pe->OptionalHeader.ImageBase != (UINTN)Data) {
      return EFI_LOAD_ERROR;
    }
    Image->Base = (UINTN)Data;
    Image->Count = Pe->FileHeader.NumberOfSections;
    Image->Entry = Pe->OptionalHeader.AddressOfEntryPoint;
    Sections = Offset + OFFSET_OF (EFI_IMAGE_NT_HEADERS64, OptionalHeader) + Pe->FileHeader.SizeOfOptionalHeader;
  }
  if (!Image->Count || !Contains (0, Size, Sections, Image->Count * sizeof (*Image->Sections))) {
    return EFI_LOAD_ERROR;
  }
  Image->Sections = (CONST VOID *)(Data + Sections);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ProtectImage (CONST XIP_IMAGE *Image, UINTN Window, UINTN WindowSize, UINTN Data, UINTN Bytes,
              EFI_MEMORY_ATTRIBUTE_PROTOCOL *Memory)
{
  UINTN Previous = Image->Base;
  BOOLEAN HasEntry = FALSE;
  for (UINTN Index = 0; Index < Image->Count; ++Index) {
    CONST EFI_IMAGE_SECTION_HEADER *Section = &Image->Sections[Index];
    UINTN Length = MAX (Section->Misc.VirtualSize, Section->SizeOfRawData);
    if (!Length) {
      continue;
    }
    UINTN Start = Image->Base + Section->VirtualAddress;
    if (Start < Image->Base || Start < Previous || Start > MAX_UINTN - Length) {
      return EFI_LOAD_ERROR;
    }
    Previous = Start + Length;
    if ((Section->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    // XIP requires the initialized bytes at their virtual offset. A page
    // must not also contain data belonging to a writable or adjacent section.
    if ((Section->Characteristics & EFI_IMAGE_SCN_MEM_WRITE) != 0 || (Start & EFI_PAGE_MASK) ||
        Section->PointerToRawData != Section->VirtualAddress || Length != Section->SizeOfRawData ||
        Start < (UINTN)(Image->Sections + Image->Count) || Previous > MAX_UINTN - EFI_PAGE_MASK ||
        !Contains (Data, Bytes, Start, ALIGN_VALUE (Length, EFI_PAGE_SIZE)) ||
        !Contains (Window, WindowSize, Start, ALIGN_VALUE (Length, EFI_PAGE_SIZE))) {
      return EFI_LOAD_ERROR;
    }
    Previous = ALIGN_VALUE (Previous, EFI_PAGE_SIZE);
    if (Image->Entry >= Section->VirtualAddress && Image->Entry - Section->VirtualAddress < Length) {
      HasEntry = TRUE;
    }
  }
  if (!HasEntry) {
    return EFI_LOAD_ERROR;
  }
  for (UINTN Index = 0; Index < Image->Count; ++Index) {
    CONST EFI_IMAGE_SECTION_HEADER *Section = &Image->Sections[Index];
    if (!(Section->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) || !Section->SizeOfRawData) {
      continue;
    }
    UINTN Start = Image->Base + Section->VirtualAddress;
    UINTN Length = ALIGN_VALUE ((UINTN)Section->SizeOfRawData, EFI_PAGE_SIZE);
    UINT64 Attributes;
    // Protect against writes before allowing instruction fetch.
    EFI_STATUS Status = Memory->SetMemoryAttributes (Memory, Start, Length, EFI_MEMORY_RO);
    if (!EFI_ERROR (Status)) {
      Status = Memory->ClearMemoryAttributes (Memory, Start, Length, EFI_MEMORY_XP);
    }
    if (!EFI_ERROR (Status)) {
      Status = Memory->GetMemoryAttributes (Memory, Start, Length, &Attributes);
    }
    if (EFI_ERROR (Status) || Attributes != EFI_MEMORY_RO) {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
FindCode (CONST UINT8 *File, UINTN Size, UINTN First, UINTN Window, UINTN WindowSize, EFI_MEMORY_ATTRIBUTE_PROTOCOL *Memory)
{
  for (UINTN Pos = First; Contains (0, Size, Pos, sizeof (EFI_COMMON_SECTION_HEADER));) {
    CONST EFI_COMMON_SECTION_HEADER *Section = (CONST VOID *)(File + Pos);
    UINTN Head = sizeof (*Section);
    UINTN Length = SECTION_SIZE (Section);
    if (IS_SECTION2 (Section)) {
      Head = sizeof (EFI_COMMON_SECTION_HEADER2);
      if (!Contains (0, Size, Pos, Head)) {
        return EFI_VOLUME_CORRUPTED;
      }
      Length = ((CONST EFI_COMMON_SECTION_HEADER2 *)Section)->ExtendedSize;
    }
    if (Length < Head || Length > Size - Pos) {
      return EFI_VOLUME_CORRUPTED;
    }
    if (Section->Type == EFI_SECTION_TE || Section->Type == EFI_SECTION_PE32) {
      CONST UINT8 *Data = File + Pos + Head;
      XIP_IMAGE Image;
      EFI_STATUS Status = ReadImage (Section->Type, Data, Length - Head, &Image);
      return EFI_ERROR (Status) ? Status : ProtectImage (&Image, Window, WindowSize, (UINTN)Data, Length - Head, Memory);
    }
    Pos += ALIGN_VALUE (Length, 4);
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
BootFirmwareProtectSec (EFI_PHYSICAL_ADDRESS Base, UINTN Size, EFI_MEMORY_ATTRIBUTE_PROTOCOL *Memory)
{
  CONST EFI_FIRMWARE_VOLUME_HEADER *Fv = (CONST VOID *)(UINTN)Base;
  if (!Memory || !Memory->SetMemoryAttributes || !Memory->ClearMemoryAttributes || !Memory->GetMemoryAttributes) {
    return EFI_INVALID_PARAMETER;
  }
  if (Base & EFI_PAGE_MASK || Size < sizeof (*Fv) || Size > MAX_UINTN - Base ||
      Fv->Signature != EFI_FVH_SIGNATURE || Fv->HeaderLength < sizeof (*Fv) || Fv->HeaderLength > Size ||
      (Fv->HeaderLength & 1) || CalculateSum16 ((CONST UINT16 *)Fv, Fv->HeaderLength) != 0 ||
      Fv->FvLength < Fv->HeaderLength) {
    return EFI_VOLUME_CORRUPTED;
  }
  UINTN Pos = Fv->HeaderLength;
  Size = MIN (Size, Fv->FvLength);
  if (Fv->ExtHeaderOffset) {
    UINTN Ext = Fv->ExtHeaderOffset;
    CONST EFI_FIRMWARE_VOLUME_EXT_HEADER *Header = (CONST VOID *)(Base + Ext);
    if (!Contains (0, Size, Ext, sizeof (*Header)) || Header->ExtHeaderSize < sizeof (*Header) ||
        Header->ExtHeaderSize > Size - Ext) {
      return EFI_VOLUME_CORRUPTED;
    }
    Pos = Ext + Header->ExtHeaderSize;
  }
  Pos = ALIGN_VALUE (Pos, 8);
  while (Contains (0, Size, Pos, sizeof (EFI_FFS_FILE_HEADER))) {
    CONST EFI_FFS_FILE_HEADER *File = (CONST VOID *)(Base + Pos);
    UINTN Header = sizeof (*File);
    UINTN Length = FFS_FILE_SIZE (File);
    if (IS_FFS_FILE2 (File)) {
      Header = sizeof (EFI_FFS_FILE_HEADER2);
      if (!Contains (0, Size, Pos, Header)) {
        return EFI_VOLUME_CORRUPTED;
      }
      Length = ((CONST EFI_FFS_FILE_HEADER2 *)File)->ExtendedSize;
    }
    if (Length < Header || Length > Size - Pos) {
      return EFI_NOT_FOUND;
    }
    UINT8 State = File->State ^ ((Fv->Attributes & EFI_FVB2_ERASE_POLARITY) ? 0xff : 0);
    if (File->Type == EFI_FV_FILETYPE_SECURITY_CORE && (State & EFI_FILE_DATA_VALID) &&
        !(State & (EFI_FILE_DELETED | EFI_FILE_HEADER_INVALID))) {
      if ((UINT8)(CalculateSum8 ((CONST UINT8 *)File, Header) - File->State - File->IntegrityCheck.Checksum.File) != 0) {
        return EFI_VOLUME_CORRUPTED;
      }
      return FindCode ((CONST UINT8 *)File, Length, Header, Base, Size, Memory);
    }
    Pos += ALIGN_VALUE (Length, 8);
  }
  return EFI_NOT_FOUND;
}
