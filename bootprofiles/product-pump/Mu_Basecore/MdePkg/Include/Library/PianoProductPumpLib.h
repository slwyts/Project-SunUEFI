// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_PRODUCT_PUMP_LIB_H
#define PIANO_PRODUCT_PUMP_LIB_H
#include <Uefi.h>
#include <Protocol/PianoProductRuntime.h>
EFI_STATUS EFIAPI PianoProductPumpApplication(UINT32 Reason,UINTN BudgetUs);
EFI_STATUS EFIAPI PianoProductGetPendingAction(UINT32 *Action,UINT64 *Sequence);
// Exact pending RETURN_CORE query; no acknowledgement or application dispatch.
// Lost Boot Services stops before UI callers can perform ordinary cleanup.
BOOLEAN EFIAPI PianoProductReturnCoreRequested(VOID);
// Any real pending UI/core transition. Provider same-current navigation is a
// successful no-op, so active consumers never see a spurious self transition.
BOOLEAN EFIAPI PianoProductUiReturnRequested(VOID);
EFI_STATUS EFIAPI PianoProductRequestNavigation(UINT32 Action);
// Trusted UI asks the parent to perform all-owner retirement before resetting.
EFI_STATUS EFIAPI PianoProductRequestReboot(VOID);
// CPU-only implementation identity; unsupported targets must not bypass owners.
BOOLEAN EFIAPI PianoProductRebootManaged(VOID);
// CPU-only local fence check; never calls a cached provider after EBS.
BOOLEAN EFIAPI PianoProductPumpBootServicesAlive(VOID);
#endif
