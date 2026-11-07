// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only Type-C state of the PMIH010X charger (SID 7). Layout from stock
// PmicDxe (GUID 5776232e-082d-4b75-9a0e-fe1d13f7a5d9):
//   RVA 0x2b670   charger = lowest TYPE 0x02 peripheral, SUBTYPE 0x7f/0xa8/0xba
//   RVA 0x29bc4   +0x300 on all blocks for SUBTYPE 0xba..0xc4
//   .data 0x3c4c0 Type-C block = charger + 0x600
//   RVA 0x2ab8c   +0x06 SNK Rp BIT1..3, +0x08 SRC Rd BIT3/BIT2, +0x0B BIT0 attached, BIT1 CC
//   RVA 0x24068   direct read only for PMIC model 0x47/0x56
// Stock VBUS enable (SCHG +0x60, RVA 0x238cc) is empty, so no SPMI write channel is used
// Observer channel per MiCode spmi-pmic-arb.c v7: 0x20 * APID for EE 0
#include "PianoUsbTypec.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>

#define ARB_CORE 0x0C400000U
#define ARB_CFG  0x0C42D000U
#define ARB_OBS  0x0C440000U
#define PMIC_SID 7U
#define TYPEC_STATUS 0x06U

STATIC struct {
  BOOLEAN Tried,Ready;
  UINT32 Count;
  UINT16 TypecApid,DcdcApid,UsbinApid;
  UINT16 Charger,Typec;
  UINT8 Model,Subtype;
  BOOLEAN LastValid;
  UINT8 Last[6];
} mTypec;

STATIC UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC BOOLEAN ExpectedArbiter(CONST VOID *Fdt) {
  STATIC CONST UINT32 Reg[]={ARB_CFG,0x4000,ARB_CORE,0x3000,0x0C500000,0x400000,ARB_OBS,0x80000,0x0C4C0000,0x10000};
  INT32 Node=FdtPathOffset(Fdt,"/soc/qcom,spmi@c42d000"),Len;
  if(Node<0)return FALSE;
  CONST UINT8 *P=FdtGetProp(Fdt,Node,"reg",&Len);
  if(P==NULL || Len!=sizeof(Reg))return FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(Reg);++I)if(Be32(P+4*I)!=Reg[I])return FALSE;
  P=FdtGetProp(Fdt,Node,"qcom,ee",&Len);
  if(P==NULL || Len!=4 || Be32(P)!=0)return FALSE;
  P=FdtGetProp(Fdt,Node,"qcom,bus-id",&Len);
  return P!=NULL && Len==4 && Be32(P)==0;
}
// Prefer EE 0's APID; observer reads need no write ownership
STATIC EFI_STATUS FindApid(UINT16 Ppid,UINT16 *Apid) {
  BOOLEAN Found=FALSE;
  for(UINT32 I=0;I<mTypec.Count;++I) {
    UINT32 Map=MmioRead32(ARB_CORE+0x2000+4*I);
    if(Map==0 || ((Map>>8)&0xFFF)!=Ppid)continue;
    UINT32 Owner=MmioRead32(ARB_CFG+4*I)&7;
    if(!Found || Owner==0){*Apid=(UINT16)I;Found=TRUE;}
    if(Owner==0)break;
  }
  return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
// EXT_READL on the EE0 observer channel only, never the RW channel
STATIC EFI_STATUS ObserverRead(UINT16 Apid,UINT8 Offset,UINT8 *Data,UINTN Bytes) {
  if(Apid>=1024 || Data==NULL || Bytes==0 || Bytes>8 || (UINTN)Offset+Bytes>0x100)return EFI_INVALID_PARAMETER;
  UINTN Channel=ARB_OBS+0x20*(UINTN)Apid;
  MmioWrite32(Channel,(1U<<27)|((UINT32)Offset<<4)|(UINT32)(Bytes-1));
  for(UINTN Us=0;Us<1000;++Us) {
    UINT32 State=MmioRead32(Channel+8);
    if(State&1) {
      if(State&0xE)return EFI_DEVICE_ERROR;
      UINT32 Low=MmioRead32(Channel+0x18),High=Bytes>4?MmioRead32(Channel+0x1C):0;
      for(UINTN I=0;I<Bytes;++I)Data[I]=(UINT8)((I<4?Low:High)>>((I&3)*8));
      return EFI_SUCCESS;
    }
    gBS->Stall(1);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS PeripheralRead(UINT16 Address,UINT8 Offset,UINT8 *Data,UINTN Bytes) {
  UINT16 Apid;EFI_STATUS S=FindApid((UINT16)((PMIC_SID<<8)|(Address>>8)),&Apid);
  return S==EFI_SUCCESS?ObserverRead(Apid,Offset,Data,Bytes):S;
}
STATIC EFI_STATUS Discover(CONST VOID *Fdt) {
  if(Fdt==NULL || !ExpectedArbiter(Fdt))return EFI_UNSUPPORTED;
  UINT32 Version=MmioRead32(ARB_CORE);mTypec.Count=MmioRead32(ARB_CORE+4)&0x7FF;
  if(Version<0x70000000 || Version>=0x80000000 || mTypec.Count==0 || mTypec.Count>1024)return EFI_UNSUPPORTED;
  UINT8 Id[2]={0};EFI_STATUS S=PeripheralRead(0x100,4,Id,2); // REVID TYPE/SUBTYPE
  mTypec.Model=Id[1];
  if(S!=EFI_SUCCESS || Id[0]!=0x51 || (Id[1]!=0x47 && Id[1]!=0x56)) {
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_UNSUPPORTED revid=%r type=%02x model=%02x\n",S,Id[0],Id[1]));
    return EFI_UNSUPPORTED;
  }
  UINT16 Best=0;BOOLEAN Found=FALSE;
  for(UINT32 I=0;I<mTypec.Count;++I) {
    UINT32 Map=MmioRead32(ARB_CORE+0x2000+4*I);
    if(Map==0 || ((Map>>16)&0xF)!=PMIC_SID)continue;
    UINT16 Address=(UINT16)(((Map>>8)&0xFF)<<8);
    if(Found && Address>=Best)continue;
    UINT8 Kind[2]={0};
    if(ObserverRead((UINT16)I,4,Kind,2)!=EFI_SUCCESS || Kind[0]!=0x02)continue;
    if(Kind[1]!=0x7f && Kind[1]!=0xa8 && Kind[1]!=0xba)continue;
    Best=Address;mTypec.Subtype=Kind[1];Found=TRUE;
  }
  if(!Found){DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_UNSUPPORTED model=%02x charger=absent\n",mTypec.Model));return EFI_UNSUPPORTED;}
  UINT16 Shift=(mTypec.Subtype>=0xba && mTypec.Subtype<=0xc4)?0x300:0;
  mTypec.Charger=Best;mTypec.Typec=(UINT16)(Best+Shift+0x600);
  S=FindApid((UINT16)((PMIC_SID<<8)|(mTypec.Typec>>8)),&mTypec.TypecApid);
  if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_UNSUPPORTED charger=%04x typec=%04x apid=%r\n",Best,mTypec.Typec,S));return EFI_UNSUPPORTED;}
  if(FindApid((UINT16)((PMIC_SID<<8)|((Best+Shift+0x100)>>8)),&mTypec.DcdcApid)!=EFI_SUCCESS)mTypec.DcdcApid=MAX_UINT16;
  if(FindApid((UINT16)((PMIC_SID<<8)|((Best+Shift+0x300)>>8)),&mTypec.UsbinApid)!=EFI_SUCCESS)mTypec.UsbinApid=MAX_UINT16;
  UINT8 Block[2]={0};S=ObserverRead(mTypec.TypecApid,4,Block,2);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_LAYOUT model=%02x charger=%04x subtype=%02x typec=%04x typec_type=%02x typec_subtype=%02x read=%r readonly=1\n",
    mTypec.Model,Best,mTypec.Subtype,mTypec.Typec,Block[0],Block[1],S));
  return S;
}
STATIC VOID LogPower(VOID) {
  UINT8 Dcdc[16]={0},Usbin[8]={0},Rt[3]={0};EFI_STATUS D=EFI_NOT_FOUND,U=EFI_NOT_FOUND;
  if(mTypec.DcdcApid!=MAX_UINT16) {
    D=ObserverRead(mTypec.DcdcApid,0x06,Dcdc,8);
    if(D==EFI_SUCCESS)D=ObserverRead(mTypec.DcdcApid,0x40,Dcdc+8,8);
    if(D==EFI_SUCCESS)D=ObserverRead(mTypec.DcdcApid,0x10,Rt,1);
  }
  if(mTypec.UsbinApid!=MAX_UINT16) {
    U=ObserverRead(mTypec.UsbinApid,0x06,Usbin,8);
    if(U==EFI_SUCCESS)U=ObserverRead(mTypec.UsbinApid,0x10,Rt+1,1);
  }
  (VOID)ObserverRead(mTypec.TypecApid,0x10,Rt+2,1);
  // Type-C +0x44: port mode (DRP, sink only, disabled)
  UINT8 Mode=0;EFI_STATUS M=ObserverRead(mTypec.TypecApid,0x44,&Mode,1);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_MODE cfg=%02x status=%r\n",Mode,M));
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC_POWER dcdc=%r %02x%02x%02x%02x%02x%02x%02x%02x cmd=%02x%02x%02x%02x%02x%02x%02x%02x usbin=%r %02x%02x%02x%02x%02x%02x%02x%02x rt=%02x/%02x/%02x\n",
    D,Dcdc[0],Dcdc[1],Dcdc[2],Dcdc[3],Dcdc[4],Dcdc[5],Dcdc[6],Dcdc[7],Dcdc[8],Dcdc[9],Dcdc[10],Dcdc[11],Dcdc[12],Dcdc[13],Dcdc[14],Dcdc[15],
    U,Usbin[0],Usbin[1],Usbin[2],Usbin[3],Usbin[4],Usbin[5],Usbin[6],Usbin[7],Rt[0],Rt[1],Rt[2]));
}
EFI_STATUS PianoUsbTypecRead(CONST VOID *Fdt,PIANO_USB_TYPEC_STATE *State) {
  if(State==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(State,sizeof(*State));
  if(!mTypec.Tried){mTypec.Tried=TRUE;mTypec.Ready=Discover(Fdt)==EFI_SUCCESS;}
  if(!mTypec.Ready)return EFI_UNSUPPORTED;
  EFI_STATUS S=ObserverRead(mTypec.TypecApid,TYPEC_STATUS,State->Raw,sizeof(State->Raw));
  if(S!=EFI_SUCCESS)return S;
  UINT8 Snk=State->Raw[0x06-TYPEC_STATUS],Src=State->Raw[0x08-TYPEC_STATUS],Misc=State->Raw[0x0B-TYPEC_STATUS];
  State->Attached=(Misc&BIT0)!=0;
  State->Orientation=(Misc&BIT1)?2:1;
  // Any Rp (PC or charger) keeps the device role so Fastboot is never cut off
  State->PartnerSink=State->Attached && (Src&(BIT3|BIT2))!=0 && (Snk&(BIT3|BIT2|BIT1|BIT0))==0;
  if(!mTypec.LastValid || CompareMem(mTypec.Last,State->Raw,sizeof(mTypec.Last))) {
    CopyMem(mTypec.Last,State->Raw,sizeof(mTypec.Last));mTypec.LastValid=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_TYPEC raw=%02x%02x%02x%02x%02x%02x attached=%u partner_sink=%u cc=%u\n",
      State->Raw[0],State->Raw[1],State->Raw[2],State->Raw[3],State->Raw[4],State->Raw[5],
      State->Attached,State->PartnerSink,State->Attached?State->Orientation:0));
    LogPower();
  }
  return EFI_SUCCESS;
}
