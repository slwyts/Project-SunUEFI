// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8750 M31/QMP bring-up, based on the Piano UsbConfigDxe implementation in
// mu_wide_platforms 3e1d052064beb6e59cda3cfba87d50e6cc088174 and factory XBL DT.
// No native USB driver is started: the shared DWC3 service retains DMA ownership.
#include "PianoUsbPhy.h"
#include "PianoUsbPhyConfig.h"
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define HS 0x88E3000U
#define SS 0x88E8000U
STATIC BOOLEAN mPhyOwned;
// Native RPMh revision 1.1, verified against the fixed Piano RpmhDxe table.
// Use its resource votes, not raw PMIC regulator writes or a second RSC driver.
typedef struct {
  UINT64 Revision;
  VOID *Init;
  EFI_STATUS(EFIAPI *ValidDrv)(UINT32,BOOLEAN *);
  VOID *TcsCommands,*CreateInternal,*CreateExplicit;
  EFI_STATUS(EFIAPI *Create)(UINT32,CONST CHAR8 *,VOID **);
  VOID *Invalidate,*RegisterCallback;
  EFI_STATUS(EFIAPI *Delete)(VOID *);
  EFI_STATUS(EFIAPI *Find)(UINT32,VOID **);
  EFI_STATUS(EFIAPI *Churn)(VOID *,UINT32);
  VOID *Unused[7];
  EFI_STATUS(EFIAPI *Issue)(VOID *,UINT32,BOOLEAN,UINT32,UINT32,UINT32 *);
} USB_RPMH;
typedef struct {
  UINT64 Revision;VOID *Query;
  EFI_STATUS(EFIAPI *Address)(VOID *,CONST CHAR8 *,UINT32 *);
} USB_CMD_DB;
typedef struct {UINT32 Data;BOOLEAN Valid;UINT8 Reserved[3];UINT32 Dirty;} USB_RPMH_VOTE;
typedef struct USB_RPMH_DRV {
  UINT32 Drv,State,Priority;USB_RPMH_VOTE AtRpmh,Local[3];struct USB_RPMH_DRV *Next;
} USB_RPMH_DRV;
typedef struct {UINT32 Address;USB_RPMH_DRV *Votes;VOID *PdcVotes;} USB_RPMH_RESOURCE;
STATIC_ASSERT(OFFSET_OF(USB_RPMH,Issue)==0x98,"Piano RPMh issue ABI");
STATIC_ASSERT(OFFSET_OF(USB_RPMH_DRV,Local)==0x18 && OFFSET_OF(USB_RPMH_DRV,Next)==0x40,"Piano RPMh vote ABI");
STATIC USB_RPMH *mRpmh;
STATIC VOID *mPowerHandle;
STATIC struct {UINT32 Address,Before,After;BOOLEAN Changed;} mPower[15];
STATIC UINTN mPowerCount;
STATIC EFI_STATUS PowerVote(UINT32 Address,UINT32 Value){
  UINT32 Request=0;EFI_STATUS S=mRpmh->Issue(mPowerHandle,0,TRUE,Address,Value,&Request);
  if(S==EFI_SUCCESS)S=mRpmh->Churn(mPowerHandle,Request);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_POWER_VOTE address=%08x value=%u request=%u status=%r\n",Address,Value,Request,S));
  return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS PowerStart(VOID){
  EFI_GUID Rpmh={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
  EFI_GUID CmdDb={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
  USB_CMD_DB *Db=NULL;EFI_STATUS S=gBS->LocateProtocol(&Rpmh,NULL,(VOID **)&mRpmh);
  if(S!=EFI_SUCCESS)return S;
  S=gBS->LocateProtocol(&CmdDb,NULL,(VOID **)&Db);if(S!=EFI_SUCCESS)return S;
  if(!mRpmh || mRpmh->Revision!=0x10001 || !mRpmh->ValidDrv || !mRpmh->Create || !mRpmh->Delete ||
     !mRpmh->Issue || !mRpmh->Churn || !mRpmh->Find || !Db || Db->Revision!=0x10000 || !Db->Address)return EFI_UNSUPPORTED;
  BOOLEAN Valid=FALSE;S=mRpmh->ValidDrv(2,&Valid);if(S!=EFI_SUCCESS || !Valid)return EFI_UNSUPPORTED;
  // Exact supply resources/voltages from the factory board DT: QMP/M31 core,
  // analog and reference, followed by PM8550B repeater 3.1 V / 1.8 V.
  STATIC CONST struct {CONST CHAR8 *Name;UINT32 Mv;} Rails[]={
    {"ldod2",880},{"ldog3",1200},{"ldof1",880},{"ldob5",3100},{"ldob15",1800}};
  for(UINTN I=0;I<ARRAY_SIZE(Rails);++I){
    UINT32 Base=0;S=Db->Address(Db,Rails[I].Name,&Base);
    if(S!=EFI_SUCCESS || (Base>>16)!=4 || (Base&255))return EFI_UNSUPPORTED;
    for(UINTN J=0;J<3;++J){
      UINT32 Address=Base+(UINT32)J*4,Old=0;USB_RPMH_RESOURCE *Resource=NULL;
      S=mRpmh->Find(Address,(VOID **)&Resource);if(S!=EFI_SUCCESS)return S;
      if(Resource){
        if(Resource->Address!=Address)return EFI_COMPROMISED_DATA;
        USB_RPMH_DRV *Vote=Resource->Votes;UINTN N=0;
        for(;Vote && N<16;Vote=Vote->Next,++N)if(Vote->Drv==2)break;
        if(Vote && N==16)return EFI_COMPROMISED_DATA;
        if(Vote && Vote->Local[0].Valid)Old=Vote->Local[0].Data;
      }
      UINT32 Want=J==0?Rails[I].Mv:J==1?1:7;
      mPower[mPowerCount].Address=Address;mPower[mPowerCount].Before=Old;
      mPower[mPowerCount].After=MAX(Old,Want);++mPowerCount;
    }
  }
  S=mRpmh->Create(2,"SunUEFI USB PHY",&mPowerHandle);if(S!=EFI_SUCCESS || !mPowerHandle)return EFI_OUT_OF_RESOURCES;
  for(UINTN I=0;I<mPowerCount;++I){
    mPower[I].Changed=TRUE;S=PowerVote(mPower[I].Address,mPower[I].After);if(S!=EFI_SUCCESS)return S;
  }
  return gBS->Stall(2000);
}
STATIC EFI_STATUS PowerStop(VOID){
  if(!mPowerHandle){mPowerCount=0;return EFI_SUCCESS;}
  for(UINTN I=mPowerCount;I>0;--I)if(mPower[I-1].Changed){
    EFI_STATUS S=PowerVote(mPower[I-1].Address,mPower[I-1].Before);if(S!=EFI_SUCCESS)return S;
    mPower[I-1].Changed=FALSE;
  }
  EFI_STATUS S=mRpmh->Delete(mPowerHandle);if(S!=EFI_SUCCESS)return S;
  mPowerHandle=NULL;mPowerCount=0;return EFI_SUCCESS;
}
STATIC UINT32 Read(UINTN Base,UINT32 Offset){return MmioRead32(Base+Offset);}
STATIC VOID Write(UINTN Base,UINT32 Offset,UINT32 Value){MmioWrite32(Base+Offset,Value);MemoryFence();}
STATIC EFI_STATUS Reset(EFI_CLOCK_PROTOCOL *Clock){
  STATIC CONST CHAR8 *Names[]={"gcc_qusb2phy_prim_bcr","gcc_usb3_dp_phy_prim_bcr",
    "gcc_usb3_phy_prim_bcr","gcc_usb3phy_phy_prim_bcr"};
  UINTN Ids[ARRAY_SIZE(Names)];EFI_STATUS S;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I){
    S=Clock->GetClockID(Clock,Names[I],&Ids[I]);if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  }
  // Native ResetAll (0x6188): hold every reset, then release in reverse order.
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I){
    S=Clock->ResetClock(Clock,Ids[I],EFI_CLOCK_RESET_ASSERT);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_RESET name=%a assert=1 status=%r\n",Names[I],S));
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  }
  gBS->Stall(10);
  for(UINTN I=ARRAY_SIZE(Names);I>0;--I){
    S=Clock->ResetClock(Clock,Ids[I-1],EFI_CLOCK_RESET_DEASSERT);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_RESET name=%a assert=0 status=%r\n",Names[I-1],S));
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  }
  S=gBS->Stall(10);return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
EFI_STATUS PianoUsbPhyStart(EFI_CLOCK_PROTOCOL *Clock){
  if(mPhyOwned)return EFI_ALREADY_STARTED;
  if(!Clock || Clock->Version!=0x1000b || !Clock->GetClockID || !Clock->ResetClock)return EFI_UNSUPPORTED;
  if((MmioRead32(0xA60C704)&BIT31) || !(MmioRead32(0xA60C70C)&BIT22))return EFI_NOT_READY;
  mPhyOwned=TRUE;
  EFI_STATUS S=PowerStart();if(S!=EFI_SUCCESS)return S;
  S=Reset(Clock);if(S!=EFI_SUCCESS)return S;
  // M31 native init 0x9ce8: assert override/POR, enable 38.4 MHz source,
  // preserve factory tuning (XBL's HS table contains only skip sentinels).
  Write(HS,0x94,2);Write(HS,0x50,2);gBS->Stall(10);
  Write(HS,0x154,1);Write(HS,0xB8,1);Write(HS,0x54,0x69);
  Write(HS,0x16C,0);Write(HS,0x17C,7);Write(HS,0x168,0);Write(HS,0x15C,0);
  Write(HS,0x64,0xF);Write(HS,0x64,0xF);Write(HS,0x3C,1);
  Write(HS,0x54,0x6B);Write(HS,0x54,0x6B);Write(HS,0x50,0);
  Write(HS,0x64,7);Write(HS,0x94,0);gBS->Stall(20);
  // GPIO61 is Piano's real cable polarity signal. Native QMP init uses bit0
  // for the lane and bit1 to enable the software port selection.
  UINT32 Reverse=MmioRead32(0xF100000U+61U*0x1000U+4)&1;
  Write(SS,0x10,(Read(SS,0x10)&~3U)|2U|Reverse);
  for(UINTN I=0;I<ARRAY_SIZE(mPianoUsbSsPhy);++I)
    Write(SS,mPianoUsbSsPhy[I].Offset,mPianoUsbSsPhy[I].Value);
  gBS->Stall(100);
  Write(SS,4,0);Write(SS,0x1E00,0);Write(SS,0x1E44,3);gBS->Stall(100);
  for(UINTN I=0;I<102;++I){if(!(Read(SS,0x1E14)&BIT6)){
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_READY reverse=%u hs_common=%08x hs_ctrl=%08x pcs=%08x factory_xbl_table=1\n",Reverse,Read(HS,0x54),Read(HS,0x64),Read(SS,0x1E14)));
    return EFI_SUCCESS;
  }gBS->Stall(100);}
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_TIMEOUT pcs=%08x\n",Read(SS,0x1E14)));
  return EFI_TIMEOUT;
}
EFI_STATUS PianoUsbPhyStop(VOID){
  if(!mPhyOwned)return EFI_SUCCESS;
  if((MmioRead32(0xA60C704)&BIT31) || !(MmioRead32(0xA60C70C)&BIT22))return EFI_NOT_READY;
  Write(SS,0x1E44,0);Write(SS,0x1E00,1);Write(SS,4,1);Write(SS,8,0);
  Write(HS,0x50,2);Write(HS,0x54,Read(HS,0x54)|BIT1|BIT2);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_STOP_READBACK start=%08x pcs_reset=%08x com_reset=%08x power=%08x hs_por=%08x hs_common=%08x\n",
    Read(SS,0x1E44),Read(SS,0x1E00),Read(SS,4),Read(SS,8),Read(HS,0x50),Read(HS,0x54)));
  // M31/QMP expose byte-wide registers replicated across a 32-bit read.
  // Compare implemented low bits; 0x01010101 is a successful write of 1.
  if((Read(SS,0x1E44)&3)!=0 || (Read(SS,0x1E00)&1)!=1 || (Read(SS,4)&1)!=1 || (Read(SS,8)&1)!=0 ||
     !(Read(HS,0x50)&BIT1) || (Read(HS,0x54)&6)!=6)return EFI_DEVICE_ERROR;
  EFI_STATUS S=PowerStop();if(S!=EFI_SUCCESS)return S;
  mPhyOwned=FALSE;return EFI_SUCCESS;
}
