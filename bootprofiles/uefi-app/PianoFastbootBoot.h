// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
// Structural recognition only. No allocation, physical address dereference,
// authentication, decompression, LoadImage, StartImage or boot state changes.
typedef EFI_STATUS (*PIANO_BOOT_READ)(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer);
typedef struct {VOID *Context;PIANO_BOOT_READ Read;UINT64 Bytes;} PIANO_BOOT_SOURCE;
typedef struct {UINT64 Offset,Bytes;} PIANO_BOOT_RANGE;
typedef enum {PianoBootUnknown,PianoBootAndroid,PianoBootArm64Pe} PIANO_BOOT_KIND;
typedef struct {
  UINT16 Machine,Subsystem,Sections;
  UINT32 EntryRva,ImageBytes,HeaderBytes,SectionAlignment,FileAlignment;
  // PE offsets are relative to its own source (Android: relative to Kernel).
  UINT64 PreferredBase,EntryFileOffset;
  PIANO_BOOT_RANGE Certificate;
} PIANO_BOOT_PE;
typedef struct {
  PIANO_BOOT_KIND Kind;
  UINT32 Version,HeaderBytes,DeclaredHeaderBytes,PageBytes;
  PIANO_BOOT_RANGE Kernel,Ramdisk,Second,RecoveryDtbo,Dtb,Signature,Trailing;
  PIANO_BOOT_RANGE Cmdline,ExtraCmdline;
  BOOLEAN KernelIsArm64Pe;
  BOOLEAN KnownV4CliHeaderQuirk;
  EFI_STATUS KernelPeStatus;
  PIANO_BOOT_PE Pe;
} PIANO_BOOT_IMAGE;
// The reader must supply all requested bytes or fail; range checks occur
// before every callback. Output is zero/Unknown after any top-level failure.
EFI_STATUS PianoFastbootBootParse(CONST PIANO_BOOT_SOURCE *Source,PIANO_BOOT_IMAGE *Image);
