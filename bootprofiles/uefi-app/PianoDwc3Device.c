// SPDX-License-Identifier: BSD-2-Clause-Patent
// Isolated USB2 DWC3 EP0; all event/TRB/data addresses use shared DMA/SMMU.
#include "PianoOwnedSmmu.h"
#include "PianoUsbControl.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#define DW 0xA600000U
typedef struct {UINT32 Low,High,Size,Control;} DWC_TRB;
STATIC PIANO_DMA_BUFFER mRing,mTrbs[2],mSetup,mTx;
STATIC PIANO_DMA_BUFFER *mPayload[2];
STATIC UINT8 mResource[2];STATIC BOOLEAN mPending[2];
STATIC UINTN mRingPosition;STATIC UINT8 mPhase,mStatusEp;
STATIC BOOLEAN mThreeStage,mConfigured;
STATIC PIANO_USB_CONTROL mControl;
STATIC UINT32 Dr(UINT32 Offset){return MmioRead32(DW+Offset);}
STATIC VOID Dw(UINT32 Offset,UINT32 Value){MmioWrite32(DW+Offset,Value);MemoryFence();}
STATIC EFI_STATUS Command(UINT8 Ep,UINT32 Cmd,UINT32 P0,UINT32 P1,UINT32 P2) {
  UINT32 Base=0xC800+Ep*16;
  Dw(Base,P2);Dw(Base+4,P1);Dw(Base+8,P0);Dw(Base+12,Cmd|BIT10);
  for(UINTN I=0;I<1000;++I) {
    UINT32 V=Dr(Base+12);
    if(!(V&BIT10)) {
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_EPCMD ep=%u cmd=%x status=%u value=%08x\n",Ep,Cmd,V>>12&15,V));
      if(V&0xF000)return EFI_DEVICE_ERROR;
      if((Cmd&15)==6)mResource[Ep]=(V>>16)&0x7F;
      return EFI_SUCCESS;
    }
    gBS->Stall(10);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Halt(VOID) {
  Dw(0xC704,Dr(0xC704)&~BIT31);
  for(UINTN I=0;I<10000;++I){if(Dr(0xC70C)&BIT22)return EFI_SUCCESS;gBS->Stall(10);}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS StopTransfer(UINT8 Ep) {
  if(!mPending[Ep])return EFI_SUCCESS;
  EFI_STATUS Status=Command(Ep,8|BIT11|((UINT32)mResource[Ep]<<16),0,0,0);
  if(EFI_ERROR(Status))return Status;
  PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);
  if(mPayload[Ep]!=NULL)PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);
  mPending[Ep]=FALSE;mPayload[Ep]=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS Transfer(UINT8 Ep,UINT32 Type,PIANO_DMA_BUFFER *Payload,UINT32 Bytes,CONST CHAR8 *Name) {
  if(Ep>1 || mPending[Ep] || Bytes>Payload->Bytes)return EFI_NOT_READY;
  DWC_TRB *T=mTrbs[Ep].Cpu;
  *T=(DWC_TRB){(UINT32)Payload->DeviceAddress,(UINT32)(Payload->DeviceAddress>>32),Bytes,
    (Type<<4)|BIT0|BIT1|BIT10|BIT11};
  EFI_STATUS Status=PianoDmaBegin(Payload,Name);if(EFI_ERROR(Status))return Status;
  Status=PianoDmaBegin(&mTrbs[Ep],Name);
  if(EFI_ERROR(Status)){PianoDmaComplete(Payload,Status,TRUE);return Status;}
  // Even a failed START may have consumed the TRB; retain until END or halt.
  mPending[Ep]=TRUE;mPayload[Ep]=Payload;
  Status=Command(Ep,6,(UINT32)(mTrbs[Ep].DeviceAddress>>32),(UINT32)mTrbs[Ep].DeviceAddress,0);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_TRANSFER ep=%u type=%u bytes=%u trb_iova=%lx data_iova=%lx status=%r\n",
    Ep,Type,Bytes,mTrbs[Ep].DeviceAddress,Payload->DeviceAddress,Status));return Status;
}
STATIC EFI_STATUS ArmSetup(VOID) {
  mPhase=0;ZeroMem(mSetup.Cpu,mSetup.Bytes);
  return Transfer(0,2,&mSetup,8,"USB_EP0_SETUP");
}
STATIC EFI_STATUS ConfigureEp(UINT8 Ep,BOOLEAN Modify) {
  UINT32 MaxPacket=mControl.SuperSpeed?512:64;
  EFI_STATUS Status=Command(Ep,1,(MaxPacket<<3)|(Modify?(2U<<30):0),(1U<<8)|(1U<<10)|((UINT32)Ep<<25),0);
  if(!EFI_ERROR(Status) && !Modify)Status=Command(Ep,2,1,0,0);
  return Status;
}
STATIC EFI_STATUS Complete(UINT8 Ep) {
  if(Ep>1 || !mPending[Ep])return EFI_COMPROMISED_DATA;
  PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);
  DWC_TRB *T=mTrbs[Ep].Cpu;
  UINT32 Residual=T->Size&0xFFFFFF,Status=T->Size>>28;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_XFER_COMPLETE ep=%u phase=%u residual=%u trb_status=%u hwo=%u\n",Ep,mPhase,Residual,Status,T->Control&1));
  mPending[Ep]=FALSE;mPayload[Ep]=NULL;
  if((T->Control&1) || Status==4)return EFI_DEVICE_ERROR;
  if(mPhase==0) {
    UINT8 *U=mSetup.Cpu;UINTN Bytes=0;PIANO_USB_CONTROL_ACTION Action;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SETUP type=%02x request=%02x value=%04x index=%04x length=%u\n",
      U[0],U[1],U[2]|U[3]<<8,U[4]|U[5]<<8,U[6]|U[7]<<8));
    EFI_STATUS S=PianoUsbControlSetup(&mControl,U,mTx.Cpu,mTx.Bytes,&Bytes,&Action);
    mThreeStage=Action==PianoUsbDataIn;mStatusEp=mThreeStage?0:1;
    if(EFI_ERROR(S)) {
      S=Command(0,4,0,0,0);if(EFI_ERROR(S))return S;
      return ArmSetup();
    }
    if(mThreeStage){mPhase=1;return Transfer(1,5,&mTx,(UINT32)Bytes,"USB_EP0_DATA_IN");}
    mPhase=2;return EFI_SUCCESS; // Wait for STATUS XferNotReady.
  }
  if(mPhase==1){mPhase=2;return EFI_SUCCESS;}
  if(mPhase==3) {
    BOOLEAN Address=mControl.SetAddress,Config=mControl.SetConfiguration;
    PianoUsbControlStatusComplete(&mControl);
    if(Address)Dw(0xC700,(Dr(0xC700)&~(0x7FU<<3))|((UINT32)mControl.Address<<3));
    if(Address || Config)DEBUG((DEBUG_WARN,"SUNUEFI_USB_ENUM_STATE address=%u configuration=%u\n",mControl.Address,mControl.Configuration));
    if(mControl.Configuration==1)mConfigured=TRUE;
    return ArmSetup();
  }
  return EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS Event(UINT32 E) {
  if(E&1) {
    UINT32 Type=(E>>8)&15;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_EVENT type=%u raw=%08x dsts=%08x\n",Type,E,Dr(0xC70C)));
    if(Type==1) {
      // End each own transfer before returning its memory to the CPU.
      EFI_STATUS S=StopTransfer(0);if(!EFI_ERROR(S))S=StopTransfer(1);if(EFI_ERROR(S))return S;
      BOOLEAN Super=mControl.SuperSpeed;ZeroMem(&mControl,sizeof(mControl));mControl.SuperSpeed=Super;
      Dw(0xC700,Dr(0xC700)&~(0x7FU<<3));return ArmSetup();
    }
    if(Type==2){mControl.SuperSpeed=(Dr(0xC70C)&7)>=4;EFI_STATUS S=ConfigureEp(0,TRUE);return EFI_ERROR(S)?S:ConfigureEp(1,TRUE);}
    if(Type==9)return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
  }
  UINT8 Ep=(E>>1)&31,Type=(E>>6)&15,Status=(E>>12)&15;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP_EVENT ep=%u type=%u status=%u raw=%08x\n",Ep,Type,Status,E));
  if(Ep>1)return EFI_UNSUPPORTED;
  if(Type==1)return Complete(Ep);
  if(Type==3 && mPhase==2 && (Status&3)==2) {
    if(Ep!=mStatusEp)return EFI_COMPROMISED_DATA;
    mPhase=3;return Transfer(Ep,mThreeStage?4:3,Ep?&mTx:&mSetup,0,"USB_EP0_STATUS");
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device) {
  PIANO_DMA_BUFFER *Buffers[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx};
  PIANO_DMA_DIRECTION Directions[]={PianoDmaFromDevice,PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice};
  UINT32 OldGctl=Dr(0xC110),OldDcfg=Dr(0xC700),OldSize=Dr(0xC408),OldLow=Dr(0xC400),OldHigh=Dr(0xC404),Count=0;
  EFI_STATUS Status=EFI_SUCCESS;mRingPosition=0;mConfigured=FALSE;
  ZeroMem(&mControl,sizeof(mControl));ZeroMem(mPending,sizeof(mPending));ZeroMem(mPayload,sizeof(mPayload));
  mControl.SuperSpeed=TRUE;
  // Reset device state without resetting the inherited PHY / Type-C path.
  Dw(0xC704,(Dr(0xC704)&~BIT31)|BIT30);
  BOOLEAN Reset=FALSE;
  for(UINTN I=0;I<10000;++I){if(!(Dr(0xC704)&BIT30)){Reset=TRUE;break;}gBS->Stall(10);}
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_CORE_RESET done=%u dctl=%08x\n",Reset,Dr(0xC704)));
  if(!Reset){Status=EFI_TIMEOUT;goto Exit;}
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I) {
    ZeroMem(Buffers[I],sizeof(*Buffers[I]));
    Status=PianoDmaAllocate(Device,4096,4096,32,Directions[I],Buffers[I]);if(EFI_ERROR(Status))goto Exit;
    Status=PianoDmaMap(Buffers[I]);if(EFI_ERROR(Status))goto Exit;
  }
  Dw(0xC110,(OldGctl&~(3U<<12))|(2U<<12)|BIT0); // Device role, retain PHY configuration.
  Dw(0xC700,(OldDcfg&~(7U|(0x7FU<<3)|(31U<<17)))|4U|(16U<<17)); // Retained SS path, address zero.
  Dw(0xC400,(UINT32)mRing.DeviceAddress);Dw(0xC404,(UINT32)(mRing.DeviceAddress>>32));
  Dw(0xC408,4096);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Status=PianoDmaBegin(&mRing,"USB_EVENT_RING");if(EFI_ERROR(Status))goto Exit;
  Status=Command(0,9,0,0,0);if(EFI_ERROR(Status))goto Exit;
  Status=ConfigureEp(0,FALSE);if(!EFI_ERROR(Status))Status=ConfigureEp(1,FALSE);if(EFI_ERROR(Status))goto Exit;
  Dw(0xC720,3);Dw(0xC708,BIT0|BIT1|BIT2|BIT3|BIT9);
  Dw(0xC704,(Dr(0xC704)&~(BIT9|BIT10|BIT11|BIT12))|BIT31);
  gBS->Stall(1000);
  Status=ArmSetup();if(EFI_ERROR(Status))goto Exit;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP0_RUN dsts=%08x event_iova=%lx\n",Dr(0xC70C),mRing.DeviceAddress));
  for(UINTN I=0;I<90000;++I) {
    Count=Dr(0xC40C)&0xFFFF;
    if(Count) {
      if(Count>4096 || (Count&3)){Status=EFI_COMPROMISED_DATA;break;}
      Status=PianoDmaSyncForCpu(&mRing);if(EFI_ERROR(Status))break;
      for(UINT32 N=0;N<Count;N+=4) {
        UINT32 E=((UINT32 *)mRing.Cpu)[mRingPosition/4];mRingPosition=(mRingPosition+4)%4096;
        Status=Event(E);if(EFI_ERROR(Status))break;
      }
      Dw(0xC40C,Count);if(EFI_ERROR(Status))break;
    }
    gBS->Stall(1000);
  }
  if(!mConfigured && !EFI_ERROR(Status))Status=EFI_NOT_READY;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP0_RESULT status=%r configured=%u address=%u configuration=%u\n",Status,mConfigured,mControl.Address,mControl.Configuration));
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_FINAL_REGS gctl=%08x dcfg=%08x dctl=%08x dsts=%08x gevntcount=%08x usb2phy=%08x usb3pipe=%08x\n",
    Dr(0xC110),Dr(0xC700),Dr(0xC704),Dr(0xC70C),Dr(0xC40C),Dr(0xC200),Dr(0xC2C0)));
  PianoSmmuLogFaults(&Context->After);
Exit:
  {EFI_STATUS Quiet=Halt();
    if(EFI_ERROR(Quiet)) {
      PianoSmmuLogFaults(&Context->After);
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_DMA_UNQUIESCED_RESET buffers_retained=1\n"));
      gRT->ResetSystem(EfiResetCold,Quiet,0,NULL);return Quiet;
    }}
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Active)PianoDmaComplete(Buffers[I],Status,TRUE);
  Dw(0xC708,0);Dw(0xC408,BIT31);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Dw(0xC400,OldLow);Dw(0xC404,OldHigh);Dw(0xC408,OldSize|BIT31);Dw(0xC700,OldDcfg);Dw(0xC110,OldGctl);
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Signature){EFI_STATUS S=PianoDmaFree(Buffers[I]);if(EFI_ERROR(S))Status=S;}
  return Status;
}
