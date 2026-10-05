// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Protocol/PianoProductRuntime.h>
#define PIANO_PRODUCT_IDLE_PROTOCOL_GUID {0x8be6f769,0x8c63,0x44e5,{0x99,0x62,0x50,0x49,0x41,0x4e,0x49,0x31}}
#define PIANO_PRODUCT_IDLE_REVISION 1ULL
typedef struct PIANO_PRODUCT_IDLE_PROTOCOL PIANO_PRODUCT_IDLE_PROTOCOL;
struct PIANO_PRODUCT_IDLE_PROTOCOL {
  UINT64 Revision;
  CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime;
  // CPU-only snapshot of the most recent completed APP pump. No MMIO/BS,
  // allocation, command, dispatch, owner teardown or cached readiness claim.
  EFI_STATUS (EFIAPI *Read)(PIANO_PRODUCT_IDLE_PROTOCOL *,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *,UINT64 *Sample,BOOLEAN *BulkActive);
};
