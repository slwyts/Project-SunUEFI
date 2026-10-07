// SPDX-License-Identifier: BSD-2-Clause-Patent
// One product supervisor publishes one instance for all EFI entry policies.
// Timer/key notify may latch action/work only; APP pump owns deferred dispatch.
#ifndef PIANO_PRODUCT_RUNTIME_PROTOCOL_H
#define PIANO_PRODUCT_RUNTIME_PROTOCOL_H
#include <Uefi.h>

#define PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID \
  {0x618c4e8d,0x29ab,0x4fdc,{0xa4,0x13,0x60,0x2b,0x46,0x39,0x72,0xa1}}
#define PIANO_PRODUCT_RUNTIME_REVISION 1ULL
#define PIANO_PRODUCT_PUMP_WAIT_EVENT BIT0
#define PIANO_PRODUCT_PUMP_GUI        BIT1
#define PIANO_PRODUCT_PUMP_APP        BIT2

#define PIANO_PRODUCT_ACTION_NONE       0U
#define PIANO_PRODUCT_ACTION_SIMPLEINIT 1U
#define PIANO_PRODUCT_ACTION_SETUP      2U
#define PIANO_PRODUCT_ACTION_SHELL      3U
// Return from a cooperative UI to the parent shared-core supervisor. Parent
// alone inspects the real USB action and retires all owners; never launch an
// image, stop controllers or call ResetSystem from a pump/key notification.
#define PIANO_PRODUCT_ACTION_RETURN_CORE 4U
// Input-only request. Provider records a trusted UI reboot reason and exposes
// RETURN_CORE (4) through GetPendingAction; consumers never observe action 5.
#define PIANO_PRODUCT_ACTION_REQUEST_REBOOT 5U
// Input-only typed Continue. Current parent policy returns to Android via
// all-owner retirement+cold reboot; a future configured OS loader can resolve
// this same action explicitly. Pending consumers still observe only 0..4.
#define PIANO_PRODUCT_ACTION_REQUEST_CONTINUE 6U
// Fixed installed ESP image; Core reads the file before retiring device owners.
#define PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE 7U

typedef struct PIANO_PRODUCT_RUNTIME_PROTOCOL PIANO_PRODUCT_RUNTIME_PROTOCOL;
struct PIANO_PRODUCT_RUNTIME_PROTOCOL {
  UINT64 Revision;
  // Only at actual TPL_APPLICATION, while BS is live and no active pump.
  // BudgetUs is admission/work-slice budget, not preemption of a blocking HAL.
  // Exactly one provider instance performs transport/backend work.
  EFI_STATUS (EFIAPI *Pump)(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Reason,UINTN BudgetUs);
  // CPU-only flag read; a consumer must still refuse cached calls after its
  // own EBS fence, as this boot-service provider memory may be reclaimed.
  BOOLEAN (EFIAPI *BootServicesAlive)(PIANO_PRODUCT_RUNTIME_PROTOCOL *This);
  // CPU-only bounded action latch, permitted in F12/notify context. No BS,
  // allocation, driver I/O, execution or fake keyboard events in these calls.
  EFI_STATUS (EFIAPI *RequestAction)(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action);
  // Exact success returns coherent pending action+sequence. No pending action
  // is represented by Action=NONE; consumers never execute from notify.
  EFI_STATUS (EFIAPI *GetPendingAction)(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 *Action,UINT64 *Sequence);
  // Parent supervisor alone consumes an action; a stale sequence must not
  // clear a newer request. GUI cooperatively exits without acknowledging.
  EFI_STATUS (EFIAPI *AckAction)(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT64 Sequence);
};
#endif
