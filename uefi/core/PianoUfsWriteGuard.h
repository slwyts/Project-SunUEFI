// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsWriteTest.h"
// Shared capability predicate only. No allocation, I/O, write authorization,
// restore transaction, protocol registration or cached-readiness bypass.
STATIC inline EFI_STATUS PianoUfsWriteGuardCheck(CONST PIANO_UFS_WRITE_GUARD *G) {
  if(G==NULL)return EFI_INVALID_PARAMETER;
  if(G->Lun!=4 || G->Collected!=0x1F || G->CapacityBytes!=PIANO_UFS_WRITE_TEST_CAPACITY ||
     G->CapacityStatus!=EFI_SUCCESS || G->ModeSenseStatus!=EFI_SUCCESS || G->UnitStatus!=EFI_SUCCESS ||
     G->PermanentFlagStatus!=EFI_SUCCESS || G->PowerOnFlagStatus!=EFI_SUCCESS)return EFI_NOT_READY;
  if(G->Fua!=TRUE || G->ModeWriteProtected!=FALSE || G->UnitWriteProtect>2 ||
     G->PermanentEnabled!=FALSE || G->PowerOnEnabled!=FALSE)return EFI_WRITE_PROTECTED;
  return EFI_SUCCESS;
}
