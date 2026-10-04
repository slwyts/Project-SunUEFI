// SPDX-License-Identifier: BSD-2-Clause-Patent
// Isolated USB2 DWC3 EP0; all event/TRB/data addresses use shared DMA/SMMU.
#include "PianoOwnedSmmu.h"
#include "PianoUsbControl.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#define DW 0xA600000U
// Native UsbfnDwc3Dxe RVAs e988/e9c0 use these exact USB0 QSCRATCH registers.
#define USB_SESSION_HS 0xF8810U
#define USB_SESSION_SS 0xF8830U
#define USB_SESSION_HS_VALID (BIT20|BIT28)
#define USB_SESSION_SS_PRESENT BIT24
typedef struct {UINT32 Low,High,Size,Control;} DWC_TRB;
STATIC PIANO_DMA_BUFFER mRing,mTrbs[2],mSetup,mTx;
STATIC PIANO_DMA_BUFFER *mPayload[2];
STATIC UINT8 mResource[2];STATIC BOOLEAN mPending[2];
STATIC UINTN mRingPosition;STATIC UINT8 mPhase,mStatusEp;
STATIC BOOLEAN mThreeStage,mConfigured;
STATIC PIANO_USB_CONTROL mControl;
// Vendor IN 5B exposes only fixed status registers and a bounded console copy.
// Snapshot storage is CPU-only; every reply uses the existing mapped mTx.
#define USB_DIAG_LOG_BYTES 65536U
#define USB_DIAG_PAGE_BYTES 512U
#define USB_DIAG_PAGE_HEADER 24U
STATIC UINT8 *mLogSnapshot;
STATIC UINT32 mLogBytes,mLogCrc,mLogGeneration;
STATIC UINT16 mLogFlags;
STATIC BOOLEAN mLogValid;
STATIC UINT32 mDeviceEvents,mSetupEvents,mDiagReplies;
STATIC UINT32 Dr(UINT32 Offset){return MmioRead32(DW+Offset);}
STATIC VOID Dw(UINT32 Offset,UINT32 Value){MmioWrite32(DW+Offset,Value);MemoryFence();}
STATIC UINT16 DiagLe16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC VOID DiagPut32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
STATIC VOID DiagPut16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
STATIC UINT32 DiagCrc(CONST UINT8 *P,UINTN Bytes) {
  UINT32 Crc=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I) {
    Crc^=P[I];for(UINTN N=0;N<8;++N)Crc=(Crc>>1)^((Crc&1)?0xEDB88320U:0);
  }
  return ~Crc;
}
STATIC EFI_STATUS SnapshotConsole(CONST volatile UINT32 *Header) {
  CONST UINTN Capacity=0x200000-12;
  mLogValid=FALSE;
  if(mLogSnapshot==NULL){mLogSnapshot=AllocatePool(USB_DIAG_LOG_BYTES);if(mLogSnapshot==NULL)return EFI_OUT_OF_RESOURCES;}
  for(UINTN Attempt=0;Attempt<3;++Attempt) {
    UINT32 Signature=Header[0],Start=Header[1],Size=Header[2];MemoryFence();
    if(Signature!=0x43474244 || Start>=Capacity || Size>Capacity)return EFI_COMPROMISED_DATA;
    UINTN Bytes=MIN((UINTN)Size,(UINTN)USB_DIAG_LOG_BYTES),Offset=(Start+Capacity-Bytes)%Capacity;
    CONST volatile UINT8 *Ring=(CONST volatile UINT8 *)Header+12;
    for(UINTN I=0;I<Bytes;++I)mLogSnapshot[I]=Ring[(Offset+I)%Capacity];
    MemoryFence();
    if(Header[0]!=Signature || Header[1]!=Start || Header[2]!=Size)continue;
    // Prefer the current session marker when retained inside the bounded tail.
    STATIC CONST CHAR8 Marker[]="SUNUEFI_RAMLOG_BEGIN";UINTN Begin=0;BOOLEAN Found=FALSE;
    for(UINTN I=0;I+sizeof(Marker)-1<=Bytes;++I)
      if((I==0 || mLogSnapshot[I-1]=='\n') && !CompareMem(mLogSnapshot+I,Marker,sizeof(Marker)-1)){Begin=I;Found=TRUE;}
    if(Begin){CopyMem(mLogSnapshot,mLogSnapshot+Begin,Bytes-Begin);Bytes-=Begin;}
    mLogBytes=(UINT32)Bytes;mLogCrc=DiagCrc(mLogSnapshot,Bytes);
    mLogFlags=(Size>USB_DIAG_LOG_BYTES?1:0)|(Found?2:0);
    if(++mLogGeneration==0)++mLogGeneration;
    mLogValid=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_DIAG_SNAPSHOT generation=%u bytes=%u crc32=%08x flags=%x\n",mLogGeneration,mLogBytes,mLogCrc,mLogFlags));
    return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
STATIC EFI_STATUS DiagnosticSetup(CONST UINT8 U[8],UINT8 *Data,UINTN Capacity,UINTN *Bytes,
                                  PIANO_USB_CONTROL_ACTION *Action) {
  if(U[0]!=0xC0 || U[1]!=0x5B || mControl.Configuration!=1)return EFI_UNSUPPORTED;
  UINT16 Op=DiagLe16(U+2),Page=DiagLe16(U+4),Length=DiagLe16(U+6);
  if(Capacity<USB_DIAG_PAGE_HEADER+USB_DIAG_PAGE_BYTES)return EFI_BAD_BUFFER_SIZE;
  *Bytes=0;*Action=PianoUsbStall;
  if(Op==0 && Page==0 && Length>=48) {
    ZeroMem(Data,48);CopyMem(Data,"SUNDBG01",8);
    Data[8]=mControl.Address;Data[9]=mControl.Configuration;Data[10]=mControl.SuperSpeed;Data[11]=mPhase;
    DiagPut32(Data+12,Dr(0xC700));DiagPut32(Data+16,Dr(0xC70C));DiagPut32(Data+20,Dr(0xC110));
    DiagPut32(Data+24,Dr(USB_SESSION_HS));DiagPut32(Data+28,Dr(USB_SESSION_SS));
    DiagPut32(Data+32,mDeviceEvents);DiagPut32(Data+36,mSetupEvents);DiagPut32(Data+40,mDiagReplies+1);
    DiagPut32(Data+44,(mConfigured?1U:0)|(mLogValid?2U:0));*Bytes=48;
  } else if(Op==1 && Page==0 && Length>=24) {
    EFI_STATUS Status=SnapshotConsole((CONST volatile UINT32 *)(UINTN)0xA3500000);
    if(EFI_ERROR(Status))return Status;
    CopyMem(Data,"SUNLOG01",8);DiagPut32(Data+8,mLogGeneration);DiagPut32(Data+12,mLogBytes);
    DiagPut32(Data+16,mLogCrc);DiagPut16(Data+20,USB_DIAG_PAGE_BYTES);DiagPut16(Data+22,mLogFlags);*Bytes=24;
  } else if(Op==2 && mLogValid) {
    UINTN Offset=(UINTN)Page*USB_DIAG_PAGE_BYTES;
    if(Offset>=mLogBytes)return EFI_UNSUPPORTED;
    UINT16 Payload=(UINT16)MIN((UINTN)mLogBytes-Offset,(UINTN)USB_DIAG_PAGE_BYTES);
    // Exact length avoids needing an extra ZLP for a short final page.
    if(Length!=USB_DIAG_PAGE_HEADER+Payload)return EFI_BAD_BUFFER_SIZE;
    CopyMem(Data,"SUNPAGE1",8);DiagPut32(Data+8,mLogGeneration);DiagPut32(Data+12,(UINT32)Offset);
    DiagPut16(Data+16,Payload);DiagPut16(Data+18,USB_DIAG_PAGE_HEADER);
    DiagPut32(Data+20,DiagCrc(mLogSnapshot+Offset,Payload));CopyMem(Data+USB_DIAG_PAGE_HEADER,mLogSnapshot+Offset,Payload);
    *Bytes=USB_DIAG_PAGE_HEADER+Payload;
  } else return EFI_UNSUPPORTED;
  ++mDiagReplies;*Action=PianoUsbDataIn;return EFI_SUCCESS;
}
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
    ++mSetupEvents;
    UINT8 *U=mSetup.Cpu;UINTN Bytes=0;PIANO_USB_CONTROL_ACTION Action;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SETUP type=%02x request=%02x value=%04x index=%04x length=%u\n",
      U[0],U[1],U[2]|U[3]<<8,U[4]|U[5]<<8,U[6]|U[7]<<8));
    EFI_STATUS S=PianoUsbControlSetup(&mControl,U,mTx.Cpu,mTx.Bytes,&Bytes,&Action);
    if(S==EFI_UNSUPPORTED && U[0]==0xC0 && U[1]==0x5B)
      S=DiagnosticSetup(U,mTx.Cpu,mTx.Bytes,&Bytes,&Action);
    mThreeStage=Action==PianoUsbDataIn;mStatusEp=mThreeStage?0:1;
    if(EFI_ERROR(S)) {
      S=Command(0,4,0,0,0);if(EFI_ERROR(S))return S;
      return ArmSetup();
    }
    if(mControl.SetAddress) {
      // DWC3 requires DCFG.DevAddr before the STATUS STARTTRANSFER.
      // MiCode dwc3_ep0_set_address and the core programming model do this
      // in SETUP; commit our software Chapter 9 state only after STATUS.
      UINT32 Before=Dr(0xC700),Expected=(Before&~(0x7FU<<3))|((UINT32)mControl.PendingAddress<<3);
      Dw(0xC700,Expected);UINT32 After=Dr(0xC700);
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_ADDRESS_ARM pending=%u dcfg_before=%08x dcfg_after=%08x phase=setup\n",
        mControl.PendingAddress,Before,After));
      if(After!=Expected)return EFI_DEVICE_ERROR;
    }
    if(mThreeStage){mPhase=1;return Transfer(1,5,&mTx,(UINT32)Bytes,"USB_EP0_DATA_IN");}
    mPhase=2;return EFI_SUCCESS; // Wait for STATUS XferNotReady.
  }
  if(mPhase==1){mPhase=2;return EFI_SUCCESS;}
  if(mPhase==3) {
    BOOLEAN Address=mControl.SetAddress,Config=mControl.SetConfiguration;
    PianoUsbControlStatusComplete(&mControl);
    if(Address || Config)DEBUG((DEBUG_WARN,"SUNUEFI_USB_ENUM_STATE address=%u configuration=%u dcfg=%08x\n",mControl.Address,mControl.Configuration,Dr(0xC700)));
    if(mControl.Configuration==1)mConfigured=TRUE;
    return ArmSetup();
  }
  return EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS Event(UINT32 E) {
  if(E&1) {
    ++mDeviceEvents;
    UINT32 Type=(E>>8)&15;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_EVENT type=%u raw=%08x dsts=%08x\n",Type,E,Dr(0xC70C)));
    if(Type==1) {
      // End each own transfer before returning its memory to the CPU.
      EFI_STATUS S=StopTransfer(0);if(!EFI_ERROR(S))S=StopTransfer(1);if(EFI_ERROR(S))return S;
      BOOLEAN Super=mControl.SuperSpeed;ZeroMem(&mControl,sizeof(mControl));mControl.SuperSpeed=Super;
      mLogValid=FALSE;
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
  UINT32 OldSessionHs=Dr(USB_SESSION_HS),OldSessionSs=Dr(USB_SESSION_SS);
  BOOLEAN SessionSet=FALSE;
  EFI_STATUS Status=EFI_SUCCESS;mRingPosition=0;mConfigured=FALSE;
  mLogValid=FALSE;mLogBytes=mLogCrc=mLogGeneration=0;mLogFlags=0;
  mDeviceEvents=mSetupEvents=mDiagReplies=0;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SESSION_SAVED hs=%08x ss=%08x dsts=%08x\n",OldSessionHs,OldSessionSs,Dr(0xC70C)));
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
  // Qualcomm glue must present VBUS/session to the core before RUN_STOP.
  // Retaining the PHY alone does not transfer a Type-C attach notification.
  Dw(USB_SESSION_HS,OldSessionHs|USB_SESSION_HS_VALID);
  Dw(USB_SESSION_SS,OldSessionSs|USB_SESSION_SS_PRESENT);SessionSet=TRUE;
  UINT32 SessionHs=Dr(USB_SESSION_HS),SessionSs=Dr(USB_SESSION_SS);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SESSION_SET hs_target=%08x ss_target=%08x hs_read=%08x ss_read=%08x dsts=%08x\n",
    OldSessionHs|USB_SESSION_HS_VALID,OldSessionSs|USB_SESSION_SS_PRESENT,SessionHs,SessionSs,Dr(0xC70C)));
  if(SessionHs!=(OldSessionHs|USB_SESSION_HS_VALID) || SessionSs!=(OldSessionSs|USB_SESSION_SS_PRESENT)) {
    Status=EFI_DEVICE_ERROR;goto Exit;
  }
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
  // Never remove session-valid while the core may still own DMA buffers.
  if(SessionSet) {
    Dw(USB_SESSION_SS,OldSessionSs);Dw(USB_SESSION_HS,OldSessionHs);
    UINT32 RestoredHs=Dr(USB_SESSION_HS),RestoredSs=Dr(USB_SESSION_SS);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SESSION_RESTORE hs_saved=%08x ss_saved=%08x hs_read=%08x ss_read=%08x dsts=%08x\n",
      OldSessionHs,OldSessionSs,RestoredHs,RestoredSs,Dr(0xC70C)));
    if(RestoredHs!=OldSessionHs || RestoredSs!=OldSessionSs)Status=EFI_DEVICE_ERROR;
  }
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Active)PianoDmaComplete(Buffers[I],Status,TRUE);
  Dw(0xC708,0);Dw(0xC408,BIT31);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Dw(0xC400,OldLow);Dw(0xC404,OldHigh);Dw(0xC408,OldSize|BIT31);Dw(0xC700,OldDcfg);Dw(0xC110,OldGctl);
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Signature){EFI_STATUS S=PianoDmaFree(Buffers[I]);if(EFI_ERROR(S))Status=S;}
  if(mLogSnapshot!=NULL){FreePool(mLogSnapshot);mLogSnapshot=NULL;}mLogValid=FALSE;
  return Status;
}
