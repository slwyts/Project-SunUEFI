// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/PianoProductPumpLib.h>
BOOLEAN EFIAPI PianoProductPumpShouldIdle(VOID) { return TRUE; }
EFI_STATUS EFIAPI PianoProductPumpApplication(UINT32 Reason,UINTN BudgetUs) { (VOID)Reason;(VOID)BudgetUs;return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI PianoProductGetPendingAction(UINT32 *Action,UINT64 *Sequence) {
  if(Action==NULL || Sequence==NULL)return EFI_INVALID_PARAMETER;
  *Action=PIANO_PRODUCT_ACTION_NONE;*Sequence=0;return EFI_UNSUPPORTED;
}
BOOLEAN EFIAPI PianoProductPumpBootServicesAlive(VOID) { return TRUE; }
BOOLEAN EFIAPI PianoProductReturnCoreRequested(VOID) { return FALSE; }
BOOLEAN EFIAPI PianoProductUiReturnRequested(VOID) { return FALSE; }
EFI_STATUS EFIAPI PianoProductRequestNavigation(UINT32 Action) { (VOID)Action;return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI PianoProductRequestReboot(VOID) { return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI PianoProductRequestContinue(VOID) { return EFI_UNSUPPORTED; }
BOOLEAN EFIAPI PianoProductRebootManaged(VOID){return FALSE;}
