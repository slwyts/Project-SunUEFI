// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#define PIANO_PRODUCT_EXIT_PROTOCOL_GUID \
 {0xcf72d98b,0x4dca,0x4a16,{0x87,0x50,0x45,0x58,0x49,0x54,0x50,0x31}}
#define PIANO_PRODUCT_EXIT_REVISION 1ULL
typedef enum {PianoExitUnarmed,PianoExitArmed,PianoExitRetiring,PianoExitClean,PianoExitRetained,PianoExitCompleted} PIANO_PRODUCT_EXIT_PHASE;
typedef struct PIANO_PRODUCT_EXIT_PROTOCOL PIANO_PRODUCT_EXIT_PROTOCOL;
struct PIANO_PRODUCT_EXIT_PROTOCOL {
 UINT64 Revision;
 // Native Core supplies the actual currently executing image identity. This
 // is a trusted firmware callback, not an untrusted UI permission report.
 EFI_STATUS (EFIAPI *BeforeExit)(PIANO_PRODUCT_EXIT_PROTOCOL *,EFI_HANDLE,
   CONST EFI_LOADED_IMAGE_PROTOCOL *,UINTN MapKey);
 // CPU-only, allocation-free real ledger/identity check on standard EBS retry.
 EFI_STATUS (EFIAPI *ObserveClean)(PIANO_PRODUCT_EXIT_PROTOCOL *,EFI_HANDLE,
   CONST EFI_LOADED_IMAGE_PROTOCOL *,UINT64 *BootEpoch);
 VOID (EFIAPI *FailStop)(PIANO_PRODUCT_EXIT_PROTOCOL *,EFI_STATUS);
};
