// SPDX-License-Identifier: BSD-2-Clause-Patent
// Controller register/clock probe only. No UIC command, DMA, LUN or disk I/O.
#include <Uefi.h>
#include <Protocol/EFIClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#define HCI 0x01D84000U
STATIC EFI_STATUS (*mProbeAction)(CONST VOID *Fdt);
STATIC EFI_CLOCK_PROTOCOL *mHeldClock;
STATIC UINTN mHeldDomain,mHeldIds[10],mHeldCount;
STATIC BOOLEAN mDomainHeld,mRetain;
VOID PianoUfsRetainClocks(VOID){mRetain=TRUE;}
VOID PianoUfsStopClocks(VOID) {
  if(mHeldClock==NULL)return;
  while(mHeldCount){--mHeldCount;mHeldClock->DisableClock(mHeldClock,mHeldIds[mHeldCount]);}
  if(mDomainHeld)mHeldClock->DisableClockPowerDomain(mHeldClock,mHeldDomain);
  mDomainHeld=FALSE;mHeldClock=NULL;mRetain=FALSE;
}
VOID PianoUfsSetProbeAction(EFI_STATUS (*Action)(CONST VOID *Fdt)){mProbeAction=Action;}
STATIC UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC BOOLEAN Controller(CONST VOID *Fdt) {
  STATIC CONST UINT32 Reg[]={HCI,0x3000,0x01D88000,0x18000,0x01DA5000,0x2000,0x01DA4000,0x10};
  INT32 N=FdtPathOffset(Fdt,"/soc/ufshc@1d84000"),Len;if(N<0)return FALSE;
  CONST UINT8 *P=FdtGetProp(Fdt,N,"reg",&Len);if(P==NULL || Len!=sizeof(Reg))return FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(Reg);++I)if(Be32(P+4*I)!=Reg[I])return FALSE;
  P=FdtGetProp(Fdt,N,"iommus",&Len);return P!=NULL && Len==12 && Be32(P+4)==0x60 && Be32(P+8)==0;
}
STATIC EFI_STATUS ReadRegister(UINT32 Offset,UINT32 *Value) {
  if(Value==NULL || !(Offset==0 || Offset==4 || Offset==8 || Offset==0x20 || Offset==0x24 ||
     Offset==0x30 || Offset==0x34 || Offset==0x50 || Offset==0x54 || Offset==0x58 || Offset==0x60 ||
     Offset==0x70 || Offset==0x74 || Offset==0x78 || Offset==0x80 || Offset==0xE4 || Offset==0x300))return EFI_ACCESS_DENIED;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_REG_NEXT addr=%08x\n",HCI+Offset));
  *Value=MmioRead32(HCI+Offset);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_REG offset=%02x value=%08x readonly=1\n",Offset,*Value));
  return EFI_SUCCESS;
}
VOID PianoProbeUfs(CONST VOID *Fdt) {
  if(mHeldClock!=NULL){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CLOCK_ALREADY_HELD\n"));return;}
  mRetain=FALSE;
  if(!Controller(Fdt)){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DT_REJECTED\n"));return;}
  EFI_CLOCK_PROTOCOL *Clock=NULL;EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if(EFI_ERROR(Status)){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CLOCK_PROTOCOL %r\n",Status));return;}
  // The captured ClockDxe uses these alias callbacks for typed resource IDs.
  // Verify this known ABI before using the less frequently tested GDSC calls.
  if(Clock->Version!=0x1000b || (VOID *)Clock->GetClockPowerDomainID!=(VOID *)Clock->GetClockID ||
     (VOID *)Clock->EnableClockPowerDomain!=(VOID *)Clock->EnableClock ||
     (VOID *)Clock->DisableClockPowerDomain!=(VOID *)Clock->DisableClock) {
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CLOCK_ABI_REJECTED\n"));return;
  }
  STATIC CONST CHAR8 *Names[]={"gcc_ufs_phy_ahb_clk","gcc_aggre_ufs_phy_axi_clk", "gcc_ufs_phy_axi_clk",
    "gcc_ufs_phy_unipro_core_clk","gcc_ufs_phy_ice_core_clk","gcc_ufs_phy_phy_aux_clk",
    "gcc_ufs_phy_rx_symbol_0_clk","gcc_ufs_phy_rx_symbol_1_clk","gcc_ufs_phy_tx_symbol_0_clk","tcsr_ufs_clkref_en"};
  UINTN Domain=0,Ids[ARRAY_SIZE(Names)];
  Status=Clock->GetClockPowerDomainID(Clock,"gcc_ufs_phy_gdsc",&Domain);
  if(!EFI_ERROR(Status))Status=Clock->EnableClockPowerDomain(Clock,Domain);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DOMAIN %r\n",Status));
  if(EFI_ERROR(Status))return;
  mHeldClock=Clock;mHeldDomain=Domain;mDomainHeld=TRUE;mHeldCount=0;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    Status=Clock->GetClockID(Clock,Names[I],&Ids[I]);
    if(!EFI_ERROR(Status))Status=Clock->EnableClock(Clock,Ids[I]);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CLOCK %a %r\n",Names[I],Status));
    if(EFI_ERROR(Status))goto Cleanup;
    mHeldIds[mHeldCount++]=Ids[I];
  }
  STATIC CONST UINT32 Offsets[]={0,4,8,0x20,0x24,0x30,0x34,0x50,0x54,0x58,0x60,0x70,0x74,0x78,0x80,0xE4,0x300};
  for(UINTN I=0;I<ARRAY_SIZE(Offsets);++I){UINT32 Value;Status=ReadRegister(Offsets[I],&Value);if(EFI_ERROR(Status))break;}
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_PROBE_END %r physical_lun_access=0 dma_submitted=0\n",Status));
  if(!EFI_ERROR(Status) && mProbeAction!=NULL) {
    Status=mProbeAction(Fdt);DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DMA_ACTION %r\n",Status));
  }
Cleanup:
  if(mRetain && !EFI_ERROR(Status)){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CLOCK_RETAINED blockio_lifetime=1\n"));return;}
  PianoUfsStopClocks();
}
