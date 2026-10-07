// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
typedef struct {
  CONST VOID *Image;
  UINTN Bytes;
  UINT8 Sha256[32];
  VOID *Lease;
} PIANO_PRODUCT_PAYLOAD_VIEW;
// The product builder supplies the compiled trusted digest. All addresses
// come from the actual BootShim handoff and bounded /chosen initrd range.
EFI_STATUS PianoProductAcquireSimpleInit(PIANO_PRODUCT_PAYLOAD_VIEW *View);
EFI_STATUS PianoProductValidateSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View);
EFI_STATUS PianoProductReleaseSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View);
EFI_STATUS PianoProductPayloadGetFdt(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View,CONST VOID **Fdt);
// Cached only from a successfully validated payload; 0 legacy/default,
// 1 menu, 2 Linux Stable, 3 Setup. Remains available after the startup loan.
UINT32 PianoProductPayloadBootTarget(VOID);
