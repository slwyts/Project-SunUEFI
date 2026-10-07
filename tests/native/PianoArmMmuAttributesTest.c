// SPDX-License-Identifier: BSD-2-Clause-Patent
// Test exact functions extracted from pinned actual ArmMmuLibCore.c, not a
// reimplemented mapping. CPU regime is a host fixture, no actual page table/AT.
#include <assert.h>
#include <stdio.h>
#undef NULL
#include <Uefi.h>
#include <AArch64/AArch64.h>
#include <AArch64/AArch64Mmu.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
STATIC UINTN El,Hcr;
UINTN ArmReadCurrentEL(VOID) { return El; }
UINTN ArmReadHcr(VOID) { return Hcr; }
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Description) { (void)File;(void)Line;(void)Description;assert(!"Unexpected real MMU attribute ASSERT"); }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
#include "PianoActualArmMmuFunctions.h"
int main(void) {
  STATIC CONST UINT64 Addresses[]={0xA00000000ULL,0xA20000000ULL,0xA3FFFF000ULL};
  UINT64 Attr;
  for(unsigned Regime=0;Regime<3;Regime++) {
    El=Regime?AARCH64_EL2:AARCH64_EL1;Hcr=Regime==2?ARM_HCR_E2H:0;
    Attr=ArmMemoryAttributeToPageAttribute(ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);
    UINT64 Xn=Regime==1?TT_XN_MASK:(TT_UXN_MASK|TT_PXN_MASK);
    assert(Attr==(TT_ATTR_INDX_MEMORY_WRITE_BACK|TT_SH_INNER_SHAREABLE|Xn));
    assert(!(Attr&TT_AP_NO_RO));
    for(unsigned I=0;I<sizeof(Addresses)/sizeof(Addresses[0]);I++) {
      UINTN Entry=Attr|TT_AF|TT_TYPE_BLOCK_ENTRY;
      SetOutputAddress(&Entry,Addresses[I],FALSE,0);
      assert(GetOutputAddress(Entry,FALSE,0)==Addresses[I]);
      assert((Entry&~TT_ADDRESS_MASK_BLOCK_ENTRY)==(Attr|TT_AF|TT_TYPE_BLOCK_ENTRY));
      assert(GetOutputAddress(Entry,FALSE,0)>MAX_UINT32);
    }
    assert((ArmMemoryAttributeToPageAttribute(ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK)&(TT_UXN_MASK|TT_PXN_MASK))==0);
  }
  puts("Actual pinned ArmMmuLibCore helpers: WB-inner-shareable XP permissions EL1/EL2/EL2E2H; 64-bit PA beginning/middle/end preserved PASS; no live page-table/CPU/AT claim.");
  return 0;
}
