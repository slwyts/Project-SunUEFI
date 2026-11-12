// SPDX-License-Identifier: BSD-2-Clause-Patent
// Load a hash-verified EFI application from temporary Android boot RAM.
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SerialIo.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/BlockIo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/FdtLib.h>
#include <Library/DebugLib.h>

#pragma pack(1)
typedef struct {
  UINT8 Magic[16]; UINT32 Version; UINT32 HeaderBytes;
  UINT64 AppBytes; UINT8 AppHash[32];
} APP_HEADER;
#pragma pack()
STATIC CONST UINT8 mMagic[16] = "SUNUEFI-APPv1";

STATIC BOOLEAN KnownRam (UINT64 Address, UINT64 Bytes) {
  EFI_MEMORY_REGION_DESCRIPTOR *Map; UINT8 Count;
  GetMemoryMap (&Map, &Count);
  for (UINT8 I = 0; I < Count; ++I) {
    if (AsciiStrCmp (Map[I].Name, "Kernel") && AsciiStrCmp (Map[I].Name, "DXE_Heap")) { continue; }
    if (Address >= Map[I].Address && Address - Map[I].Address < Map[I].Length &&
        Bytes <= Map[I].Length - (Address - Map[I].Address)) { return TRUE; }
  }
  return FALSE;
}
STATIC UINT64 Prop (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name) {
  INT32 Len; CONST UINT8 *P = FdtGetProp (Fdt, Node, Name, &Len); UINT64 V = 0;
  if (P == NULL || (Len != 4 && Len != 8)) { return 0; }
  for (INT32 I = 0; I < Len; ++I) { V = (V << 8) | P[I]; }
  return V;
}
STATIC VOID CountProtocol (EFI_GUID *Guid, CONST CHAR8 *Name) {
  EFI_HANDLE *Handles = NULL; UINTN Count = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  DEBUG ((DEBUG_WARN, "SUNUEFI_PROTOCOL %a count=%lu status=%r\n", Name, (UINT64)Count, Status));
  if (Handles != NULL) { FreePool (Handles); }
}
STATIC VOID ProbeGop (VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop; EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = NULL; UINTN Size = 0;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Saved[64], Result[64], Color = {0x33,0x22,0x11,0};
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || Gop->Mode == NULL) { return; }
  Status = Gop->QueryMode (Gop, 0, &Size, &Info);
  DEBUG ((DEBUG_WARN, "SUNUEFI_GOP_QUERY %r\n", Status));
  if (Info != NULL) { FreePool (Info); }
  Status = Gop->QueryMode (Gop, Gop->Mode->MaxMode, &Size, &Info);
  DEBUG ((DEBUG_WARN, "SUNUEFI_GOP_INVALID_MODE %r\n", Status));
  Status = Gop->Blt (Gop, Saved, EfiBltVideoToBltBuffer, 0,0,0,0,8,8,0);
  if (EFI_ERROR (Status)) { DEBUG ((DEBUG_WARN, "SUNUEFI_GOP_READ_FAILED %r\n", Status)); return; }
  Status = Gop->Blt (Gop, &Color, EfiBltVideoFill, 0,0,0,0,8,8,0);
  if (!EFI_ERROR (Status)) { Status = Gop->Blt (Gop, Result, EfiBltVideoToBltBuffer, 0,0,0,0,8,8,0); }
  BOOLEAN Match = !EFI_ERROR (Status);
  for (UINTN I = 0; I < 64 && Match; ++I) {
    Match = Result[I].Red == Color.Red && Result[I].Green == Color.Green && Result[I].Blue == Color.Blue;
  }
  Gop->Blt (Gop, Saved, EfiBltBufferToVideo, 0,0,0,0,8,8,0);
  DEBUG ((DEBUG_WARN, "SUNUEFI_GOP_BLT_ROUNDTRIP match=%u status=%r\n", Match, Status));
}

EFI_STATUS EFIAPI RamAppEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  CONST volatile UINT64 *Record = (CONST volatile UINT64 *)(UINTN)0xA7FFF000;
  CONST VOID *Fdt; UINT64 Start, End; INT32 Chosen; CONST APP_HEADER *Header = NULL;
  UINT8 Hash[32]; EFI_HANDLE App; EFI_STATUS Status;
  DEBUG ((DEBUG_WARN, "SUNUEFI_RAM_APP_LOADER\n"));
  ProbeGop ();
  CountProtocol (&gEfiAbsolutePointerProtocolGuid, "AbsolutePointer");
  CountProtocol (&gEfiSerialIoProtocolGuid, "SerialIo");
  CountProtocol (&gEfiUsb2HcProtocolGuid, "UsbHost");
  CountProtocol (&gEfiUsbIoProtocolGuid, "UsbIo");
  CountProtocol (&gEfiBlockIoProtocolGuid, "BlockIo");
  if (Record[0] != 0x534E554546494448ULL || !KnownRam (Record[1],40)) { return EFI_NOT_FOUND; }
  Fdt = (CONST VOID *)(UINTN)Record[1];
  if (FdtCheckHeader (Fdt) || FdtTotalSize (Fdt)>0x200000 || !KnownRam (Record[1],FdtTotalSize (Fdt))) { return EFI_COMPROMISED_DATA; }
  Chosen = FdtPathOffset (Fdt,"/chosen"); if (Chosen < 0) { return EFI_NOT_FOUND; }
  Start = Prop (Fdt,Chosen,"linux,initrd-start"); End = Prop (Fdt,Chosen,"linux,initrd-end");
  if (End <= Start || End-Start>0x10000000 || !KnownRam (Start,End-Start)) { return EFI_BAD_BUFFER_SIZE; }
  for (UINT64 I=0; I+sizeof (APP_HEADER)<=End-Start; ++I) {
    CONST UINT8 *P=(CONST UINT8 *)(UINTN)(Start+I);
    if (!CompareMem (P,mMagic,sizeof (mMagic))) { Header=(CONST APP_HEADER *)P; break; }
  }
  if (Header == NULL) { return EFI_NOT_FOUND; }
  if (Header->Version != 1 || Header->HeaderBytes != sizeof (*Header) || Header->AppBytes < 4096 ||
      Header->AppBytes > 0x4000000 || Header->AppBytes > End-(UINTN)Header-sizeof (*Header)) { return EFI_COMPROMISED_DATA; }
  CONST VOID *Image=(CONST UINT8 *)Header+sizeof (*Header);
  if (!Sha256HashAll (Image,(UINTN)Header->AppBytes,Hash) || CompareMem (Hash,Header->AppHash,32)) { return EFI_SECURITY_VIOLATION; }
  DEBUG ((DEBUG_WARN, "SUNUEFI_RAM_APP_HASH_OK bytes=%lu\n",Header->AppBytes));
  Status=gBS->LoadImage (FALSE,ImageHandle,NULL,(VOID *)Image,(UINTN)Header->AppBytes,&App);
  DEBUG ((DEBUG_WARN, "SUNUEFI_SIMPLEINIT_LOAD %r\n",Status));
  if (EFI_ERROR (Status)) { return Status; }
  Status=gBS->StartImage (App,NULL,NULL);
  DEBUG ((DEBUG_WARN, "SUNUEFI_SIMPLEINIT_RETURN %r\n",Status));
  gBS->UnloadImage (App); return Status;
}
