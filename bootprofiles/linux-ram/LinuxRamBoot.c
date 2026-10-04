// SPDX-License-Identifier: BSD-2-Clause-Patent
// Load the captured ARM64 EFI-stub and initramfs exclusively from boot RAM.
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Guid/EventGroup.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/DebugLib.h>
#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>

#pragma pack(1)
typedef struct {
  UINT8 Magic[16];
  UINT32 Version;
  UINT32 HeaderSize;
  UINT64 KernelSize;
  UINT64 InitrdSize;
  UINT8 KernelHash[32];
  UINT8 InitrdHash[32];
} LINUX_RAM_HEADER;
typedef struct {LINUX_RAM_HEADER Base;UINT64 DtbSize;UINT8 DtbHash[32];} LINUX_RAM_HEADER_V2;
typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} INITRD_PATH;
#pragma pack()

STATIC CONST UINT8 mMagic[16] = "SUNUEFI-LINUXv1";
STATIC CONST UINT8 mMagicV2[16] = "SUNUEFI-LINUXv2";
STATIC VOID *mInitrd;
STATIC UINTN mInitrdSize;
STATIC INITRD_PATH mPath = {
  { { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } }, LINUX_EFI_INITRD_MEDIA_GUID },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};
// Board-only parameters from the hash-checked piano cmdline capture. Retain
// HWID-dependent PHY/PMIC selection without the native disk logging arguments.
STATIC CONST CHAR8 mCommandAscii[] =
  "rdinit=/init ro nokaslr efi=novamap console=ttyGS0,115200 loglevel=7 panic=15 "
  "hwid.hwid_value=589824 hwid.project=9 hwid.build_adc=51282 hwid.project_adc=39406";
STATIC CONST CHAR16 mCommand[] =
  L"rdinit=/init ro nokaslr efi=novamap console=ttyGS0,115200 loglevel=7 panic=15 "
  L"hwid.hwid_value=589824 hwid.project=9 hwid.build_adc=51282 hwid.project_adc=39406";

VOID PianoStopFaultRecovery(VOID);
EFI_STATUS PianoStartFaultRecovery(VOID);
STATIC EFI_TEXT_STRING mOriginalOutput;
STATIC BOOLEAN mOutputGuard;
STATIC EFI_STATUS EFIAPI MirrorEfiOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *String) {
  if(String!=NULL && !mOutputGuard) {
    mOutputGuard=TRUE;
    UINTN Offset=0;
    while(Offset<4096 && String[Offset]) {
      CHAR16 Part[128];UINTN Count=0;
      while(Count<ARRAY_SIZE(Part)-1 && Offset<4096 && String[Offset])Part[Count++]=String[Offset++];
      Part[Count]=0;DEBUG((DEBUG_WARN,"SUNUEFI_EFI_STUB_OUTPUT %s\n",Part));
    }
    mOutputGuard=FALSE;
  }
  return mOriginalOutput==NULL?EFI_NOT_READY:mOriginalOutput(This,String);
}
STATIC VOID EFIAPI EfiHandoffMarker(EFI_EVENT Event,VOID *Context) {
  DEBUG((DEBUG_WARN,"SUNUEFI_EFI_HANDOFF_EVENT %a\n",(CONST CHAR8 *)Context));
}

STATIC BOOLEAN KnownRam (UINT64 Address, UINT64 Length)
{
  EFI_MEMORY_REGION_DESCRIPTOR *Map;
  UINT8 Count;
  GetMemoryMap (&Map, &Count);
  for (UINT8 Index = 0; Index < Count; ++Index) {
    EFI_MEMORY_REGION_DESCRIPTOR *R = &Map[Index];
    if (AsciiStrCmp (R->Name, "Kernel") && AsciiStrCmp (R->Name, "DXE_Heap") &&
        AsciiStrCmp (R->Name, "XBL_DT") && AsciiStrCmp (R->Name, "UEFI_RESV")) { continue; }
    if (Address >= R->Address && Address - R->Address < R->Length &&
        Length <= R->Length - (Address - R->Address)) { return TRUE; }
  }
  return FALSE;
}

STATIC UINT64 IntegerProp (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name)
{
  INT32 Length;
  CONST UINT8 *Data = FdtGetProp (Fdt, Node, Name, &Length);
  UINT64 Value = 0;
  if (Data == NULL || (Length != 4 && Length != 8)) { return 0; }
  for (INT32 I = 0; I < Length; ++I) { Value = (Value << 8) | Data[I]; }
  return Value;
}

STATIC EFI_STATUS EFIAPI LoadInitrd (
  EFI_LOAD_FILE2_PROTOCOL *This, EFI_DEVICE_PATH_PROTOCOL *FilePath,
  BOOLEAN BootPolicy, UINTN *BufferSize, VOID *Buffer OPTIONAL)
{
  DEBUG ((DEBUG_WARN, "SUNUEFI_INITRD_LOAD policy=%u buffer=%p size=%lu\n", BootPolicy, Buffer, BufferSize == NULL ? 0 : (UINT64)*BufferSize));
  if (BootPolicy) { return EFI_UNSUPPORTED; }
  if (BufferSize == NULL || FilePath == NULL || FilePath->Type != END_DEVICE_PATH_TYPE ||
      FilePath->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE) { return EFI_INVALID_PARAMETER; }
  if (Buffer == NULL || *BufferSize < mInitrdSize) {
    *BufferSize = mInitrdSize; return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, mInitrd, mInitrdSize);
  *BufferSize = mInitrdSize;
  DEBUG ((DEBUG_WARN, "SUNUEFI_INITRD_DELIVERED bytes=%lu\n", (UINT64)mInitrdSize));
  return EFI_SUCCESS;
}
STATIC EFI_LOAD_FILE2_PROTOCOL mLoad = { LoadInitrd };

#ifdef SUNUEFI_RAW_HANDOFF
// Final handoff stays in registers: no C calls or stack accesses after MMU off.
// The marker appends to the validated ramoops console only after MMU is off.
STATIC VOID __attribute__((naked, noreturn)) RawEnter (
  UINT64 TreeAddress, UINT64 EntryAddress, CONST CHAR8 *Marker, UINT64 MarkerSize)
{
  __asm__ volatile (
    "mov x4, x1\n"
    "msr daifset, #0xf\n"
    "msr cntv_ctl_el0, xzr\n"
    "mrs x9, sctlr_el1\n"
    "mov x10, #5\nbic x9, x9, x10\n"
    "dsb sy\nmsr sctlr_el1, x9\nisb\n"
    "ic iallu\ndsb sy\nisb\n"
    "movz x5, #0xa350, lsl #16\n"
    "ldr w6, [x5]\n"
    "movz w7, #0x4244\nmovk w7, #0x4347, lsl #16\n"
    "cmp w6, w7\nb.ne 4f\n"
    "ldr w6, [x5, #4]\nldr w7, [x5, #8]\n"
    "movz w10, #0xfff4\nmovk w10, #0x1f, lsl #16\n"
    "cmp w6, w10\nb.hs 4f\ncmp w7, w10\nb.hi 4f\n"
    "add x11, x5, #12\n"
    "1: ldrb w9, [x2], #1\nstrb w9, [x11, x6]\n"
    "add w6, w6, #1\ncmp w6, w10\ncsel w6, wzr, w6, eq\n"
    "cmp w7, w10\nb.hs 2f\nadd w7, w7, #1\n"
    "2: subs x3, x3, #1\nb.ne 1b\n"
    "str w6, [x5, #4]\nstr w7, [x5, #8]\ndsb sy\n"
    "4: mov x1, xzr\nmov x2, xzr\nmov x3, xzr\nbr x4\n"
  );
}

STATIC EFI_STATUS RawBoot (EFI_HANDLE ImageHandle, CONST UINT8 *Kernel, UINTN KernelBytes, CONST VOID *Fdt)
{
  UINT64 TextOffset = ReadUnaligned64 ((CONST UINT64 *)(Kernel + 8));
  UINT64 ImageSize = ReadUnaligned64 ((CONST UINT64 *)(Kernel + 16));
  UINT64 Destination = 0xA8000000ULL + TextOffset;
  VOID *Tree = (VOID *)(UINTN)0xB0000000;
  VOID *Ramdisk = (VOID *)(UINTN)0xB0200000;
  UINTN TreeCapacity = FdtTotalSize (Fdt) + 0x10000;
  INT32 Node;
  UINT64 InitrdStart, InitrdEnd;
  EFI_STATUS Status;
  UINTN MapSize = 0, MapKey, DescSize = 0;
  UINT32 DescVersion;
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_LOADED_IMAGE_PROTOCOL *Self;
  UINT64 CurrentEl;
  STATIC CONST CHAR8 BranchMarker[] = "SUNUEFI_RAW_MMU_OFF_BRANCH\n";
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  if (CurrentEl != 4) { return EFI_UNSUPPORTED; }
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status)) { return Status; }
  if (ReadUnaligned32 ((CONST UINT32 *)(Kernel + 56)) != 0x644D5241 || TextOffset > 0x100000 ||
      ImageSize < KernelBytes || ImageSize > 0x8000000 ||
      Destination + ImageSize > (UINTN)Tree ||
      !KnownRam (Destination, ImageSize) ||
      TreeCapacity > 0x200000 || !KnownRam ((UINTN)Tree, TreeCapacity) ||
      mInitrdSize > 0x1000000 || !KnownRam ((UINTN)Ramdisk, mInitrdSize)) { return EFI_COMPROMISED_DATA; }
  // All destinations are in the original native "Kernel" reserved RAM region;
  // current UEFI executes in A7100000 and its stack in A760D000.
  CopyMem ((VOID *)(UINTN)Destination, Kernel, KernelBytes);
  if (ImageSize > KernelBytes) { ZeroMem ((UINT8 *)(UINTN)Destination + KernelBytes, ImageSize - KernelBytes); }
  CopyMem (Ramdisk, mInitrd, mInitrdSize);
  if (FdtOpenInto (Fdt, Tree, (INT32)TreeCapacity)) { return EFI_COMPROMISED_DATA; }
  Node = FdtPathOffset (Tree, "/chosen");
  if (Node < 0) { return EFI_NOT_FOUND; }
  InitrdStart = SwapBytes64 ((UINTN)Ramdisk);
  InitrdEnd = SwapBytes64 ((UINTN)Ramdisk + mInitrdSize);
  if (FdtSetProp (Tree, Node, "linux,initrd-start", &InitrdStart, sizeof (InitrdStart)) ||
      FdtSetProp (Tree, Node, "linux,initrd-end", &InitrdEnd, sizeof (InitrdEnd)) ||
      FdtSetProp (Tree, Node, "bootargs", mCommandAscii, sizeof (mCommandAscii))) { return EFI_COMPROMISED_DATA; }
  FdtDelProp (Tree, Node, "kaslr-seed");
  FdtDelProp (Tree, Node, "rng-seed");
  if (FdtPack (Tree)) { return EFI_COMPROMISED_DATA; }
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Destination, MAX (ImageSize, KernelBytes));
  WriteBackInvalidateDataCacheRange (Tree, TreeCapacity);
  WriteBackInvalidateDataCacheRange (Ramdisk, mInitrdSize);
  DEBUG ((DEBUG_WARN, "SUNUEFI_RAW_READY kernel=0x%lx dtb=%p initrd=%p bytes=%lu\n", Destination, Tree, Ramdisk, (UINT64)mInitrdSize));
  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) { return Status; }
  MapSize += 8 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) { return EFI_OUT_OF_RESOURCES; }
  for (UINTN Attempt = 0; Attempt < 2; ++Attempt) {
    UINTN Size = MapSize;
    Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion);
    if (EFI_ERROR (Status)) { break; }
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) { break; }
  }
  if (EFI_ERROR (Status)) { FreePool (Map); return Status; }
  DEBUG ((DEBUG_WARN, "SUNUEFI_RAW_JUMP\n"));
  // The selected ArmCacheMaintenanceLib deliberately ASSERTs for whole-cache
  // operations. Kernel, DTB and initrd were already cleaned by VA above; clean
  // this image and its marker as well, then use a stack-free final transition.
  WriteBackInvalidateDataCacheRange (Self->ImageBase, (UINTN)Self->ImageSize);
  RawEnter ((UINTN)Tree, Destination, BranchMarker, sizeof (BranchMarker) - 1);
}
#endif

EFI_STATUS EFIAPI LinuxRamBootEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  CONST volatile UINT64 *Record = (CONST volatile UINT64 *)(UINTN)0xA7FFF000;
  CONST VOID *Fdt;
  INT32 Chosen;
  UINT64 Start, End;
  CONST LINUX_RAM_HEADER *Header = NULL;
  CONST UINT8 *Kernel;
  UINT8 Hash[32];
  VOID *NewFdt = NULL;
  VOID *AlignedDtb = NULL;
  VOID *OldFdt = NULL;
  UINTN FdtSize;
  EFI_STATUS Status;
  EFI_HANDLE InitrdHandle = NULL, KernelHandle = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_EVENT BeforeExit=NULL,OnExit=NULL;
  BOOLEAN FaultRecovery=FALSE;

  Print (L"\r\nSunUEFI Linux RAM loader\r\n");
  DEBUG ((DEBUG_WARN, "PIANO_LINUX_RAM_LOADER_START\n"));
  if (Record[0] != 0x534E554546494448ULL || !KnownRam (Record[1], 40)) { return EFI_NOT_FOUND; }
  Fdt = (CONST VOID *)(UINTN)Record[1];
  if (FdtCheckHeader (Fdt) != 0 || FdtTotalSize (Fdt) > 0x200000 ||
      !KnownRam (Record[1], FdtTotalSize (Fdt))) { return EFI_COMPROMISED_DATA; }
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) { return EFI_NOT_FOUND; }
  Start = IntegerProp (Fdt, Chosen, "linux,initrd-start");
  End = IntegerProp (Fdt, Chosen, "linux,initrd-end");
  if (End <= Start || End - Start > 0x10000000 || !KnownRam (Start, End - Start)) { return EFI_BAD_BUFFER_SIZE; }
  Print (L"Bootloader initrd 0x%lx .. 0x%lx\r\n", Start, End);
  DEBUG ((DEBUG_WARN, "SUNUEFI_LINUX_SOURCE_INITRD 0x%lx..0x%lx\n", Start, End));
  for (UINT64 Offset = 0; Offset + sizeof (LINUX_RAM_HEADER) <= End - Start; ++Offset) {
    UINT64 Left = End - Start - Offset;
    CONST UINT8 *P = (CONST UINT8 *)(UINTN)(Start + Offset);
    if (Left < sizeof (LINUX_RAM_HEADER) ||
        (CompareMem (P, mMagic, sizeof (mMagic)) && CompareMem(P,mMagicV2,sizeof(mMagicV2)))) { continue; }
    Header = (CONST LINUX_RAM_HEADER *)P;
    break;
  }
  if (Header == NULL) { Print (L"Linux payload was not found in boot RAM.\r\n"); DEBUG ((DEBUG_WARN, "SUNUEFI_LINUX_PAYLOAD_NOT_FOUND\n")); return EFI_NOT_FOUND; }
  BOOLEAN V2=Header->Version==2;
  UINTN HeaderBytes=V2?sizeof(LINUX_RAM_HEADER_V2):sizeof(*Header);
  if ((Header->Version != 1 && !V2) || Header->HeaderSize != HeaderBytes ||
      HeaderBytes>End-(UINTN)Header ||
      Header->KernelSize < 4096 || Header->KernelSize > 0x4000000 ||
      Header->InitrdSize == 0 || Header->InitrdSize > 0x2000000 ||
      Header->KernelSize + Header->InitrdSize > End - (UINTN)Header - HeaderBytes) { return EFI_COMPROMISED_DATA; }
  Kernel = (CONST UINT8 *)Header + HeaderBytes;
  mInitrd = (VOID *)(Kernel + Header->KernelSize);
  mInitrdSize = (UINTN)Header->InitrdSize;
  if (!Sha256HashAll (Kernel, (UINTN)Header->KernelSize, Hash) || CompareMem (Hash, Header->KernelHash, sizeof (Hash)) ||
      !Sha256HashAll (mInitrd, mInitrdSize, Hash) || CompareMem (Hash, Header->InitrdHash, sizeof (Hash))) {
    Print (L"Linux payload SHA-256 verification failed.\r\n"); return EFI_SECURITY_VIOLATION;
  }
  Print (L"Kernel and initramfs hashes verified: %lu / %lu bytes\r\n", Header->KernelSize, Header->InitrdSize);
  DEBUG ((DEBUG_WARN, "SUNUEFI_LINUX_HASHES_OK kernel=%lu initrd=%lu\n", Header->KernelSize, Header->InitrdSize));
  if(V2) {
    CONST LINUX_RAM_HEADER_V2 *Extended=(CONST LINUX_RAM_HEADER_V2 *)Header;
    CONST UINT8 *PinnedDtb=(CONST UINT8 *)mInitrd+mInitrdSize;
    UINT64 Left=End-(UINTN)PinnedDtb;
    if(Extended->DtbSize<40 || Extended->DtbSize>0x200000 || Extended->DtbSize>Left)return EFI_COMPROMISED_DATA;
    if(!Sha256HashAll(PinnedDtb,(UINTN)Extended->DtbSize,Hash) || CompareMem(Hash,Extended->DtbHash,32)) {
      DEBUG((DEBUG_WARN,"SUNUEFI_LINUX_PINNED_DTB_HASH_FAILED\n"));return EFI_SECURITY_VIOLATION;
    }
    // CPIO gzip length and the enclosing Android ramdisk prefix need not
    // align the following blob. libfdt requires an 8-byte-aligned address.
    AlignedDtb=AllocatePool((UINTN)Extended->DtbSize);
    if(AlignedDtb==NULL)return EFI_OUT_OF_RESOURCES;
    CopyMem(AlignedDtb,PinnedDtb,(UINTN)Extended->DtbSize);
    INT32 DtbStatus=FdtCheckHeader(AlignedDtb);
    if(DtbStatus || FdtTotalSize(AlignedDtb)!=Extended->DtbSize) {
      DEBUG((DEBUG_WARN,"SUNUEFI_LINUX_PINNED_DTB_INVALID fdt_status=%d\n",DtbStatus));
      FreePool(AlignedDtb);return EFI_COMPROMISED_DATA;
    }
    Fdt=AlignedDtb;
    DEBUG((DEBUG_WARN,"SUNUEFI_LINUX_PINNED_DTB bytes=%lu sha256_verified=1 source=payload\n",Extended->DtbSize));
  }
#ifdef SUNUEFI_RAW_HANDOFF
  Status=RawBoot (ImageHandle, Kernel, (UINTN)Header->KernelSize, Fdt);
  if(AlignedDtb!=NULL)FreePool(AlignedDtb);
  return Status;
#endif
  FdtSize = FdtTotalSize (Fdt) + 0x10000;
  NewFdt = AllocatePool (FdtSize);
  if (NewFdt == NULL) { if(AlignedDtb!=NULL)FreePool(AlignedDtb);return EFI_OUT_OF_RESOURCES; }
  if (FdtOpenInto (Fdt, NewFdt, (INT32)FdtSize)) { Status = EFI_COMPROMISED_DATA; goto Cleanup; }
  Chosen = FdtPathOffset (NewFdt, "/chosen");
  FdtDelProp (NewFdt, Chosen, "linux,initrd-start");
  FdtDelProp (NewFdt, Chosen, "linux,initrd-end");
  FdtDelProp (NewFdt, Chosen, "kaslr-seed");
  FdtDelProp (NewFdt, Chosen, "rng-seed");
  if (FdtSetProp (NewFdt, Chosen, "bootargs", mCommandAscii, sizeof (mCommandAscii))) { Status = EFI_COMPROMISED_DATA; goto Cleanup; }
  EfiGetSystemConfigurationTable (&gFdtTableGuid, &OldFdt);
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, NewFdt);
  if (EFI_ERROR (Status)) { goto Cleanup; }
  DEBUG ((DEBUG_WARN, "SUNUEFI_FDT_INSTALLED address=%p size=%u\n", NewFdt, FdtTotalSize (NewFdt)));
  Status = gBS->InstallMultipleProtocolInterfaces (&InitrdHandle,
    &gEfiLoadFile2ProtocolGuid, &mLoad, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (EFI_ERROR (Status)) { goto RestoreFdt; }
  Status = gBS->LoadImage (FALSE, ImageHandle, NULL, (VOID *)Kernel, (UINTN)Header->KernelSize, &KernelHandle);
  if (EFI_ERROR (Status)) { Print (L"Linux LoadImage failed: %r\r\n", Status); DEBUG ((DEBUG_WARN, "SUNUEFI_LINUX_LOAD_IMAGE_FAILED %r\n", Status)); goto Uninstall; }
  Status = gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { goto Unload; }
  Loaded->LoadOptions = (VOID *)mCommand;
  Loaded->LoadOptionsSize = sizeof (mCommand);
  DEBUG ((DEBUG_WARN, "SUNUEFI_KERNEL_IMAGE base=%p size=%lu\n", Loaded->ImageBase, Loaded->ImageSize));
  Print (L"Starting ARM64 Linux EFI-stub from RAM...\r\n");
  DEBUG ((DEBUG_WARN, "PIANO_LINUX_EFI_START_IMAGE\n"));
  Status=PianoStartFaultRecovery();FaultRecovery=!EFI_ERROR(Status);
  DEBUG((DEBUG_WARN,"SUNUEFI_EFI_FAULT_HANDLER %r\n",Status));
  gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,EfiHandoffMarker,"before-exit-boot-services",&gEfiEventBeforeExitBootServicesGuid,&BeforeExit);
  gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,EfiHandoffMarker,"exit-boot-services",&gEfiEventExitBootServicesGuid,&OnExit);
  if(gST->ConOut!=NULL){mOriginalOutput=gST->ConOut->OutputString;gST->ConOut->OutputString=MirrorEfiOutput;}
  Status = gBS->StartImage (KernelHandle, NULL, NULL);
  if(gST->ConOut!=NULL && mOriginalOutput!=NULL)gST->ConOut->OutputString=mOriginalOutput;
  mOriginalOutput=NULL;
  if(BeforeExit!=NULL)gBS->CloseEvent(BeforeExit);
  if(OnExit!=NULL)gBS->CloseEvent(OnExit);
  if(FaultRecovery)PianoStopFaultRecovery();
  Print (L"Linux EFI-stub returned: %r\r\n", Status);
Unload:
  gBS->UnloadImage (KernelHandle);
Uninstall:
  gBS->UninstallMultipleProtocolInterfaces (InitrdHandle,
    &gEfiLoadFile2ProtocolGuid, &mLoad, &gEfiDevicePathProtocolGuid, &mPath, NULL);
RestoreFdt:
  gBS->InstallConfigurationTable (&gFdtTableGuid, OldFdt);
Cleanup:
  if (NewFdt != NULL) { FreePool (NewFdt); }
  if (AlignedDtb != NULL) { FreePool (AlignedDtb); }
  return Status;
}
