// SPDX-License-Identifier: BSD-2-Clause-Patent
// DWC3 EP0 and RAM-only standard fastboot; all hardware DMA uses shared SMMU.
#include "PianoOwnedSmmu.h"
#include "PianoUsbControl.h"
#ifndef PIANO_USB_FASTBOOT
#define PIANO_USB_FASTBOOT 0
#endif
#ifndef PIANO_USB_SCREENSHOT
#define PIANO_USB_SCREENSHOT 0
#endif
#if PIANO_USB_FASTBOOT
#include "PianoFastboot.h"
#if defined(PIANO_USB_UFS_FETCH) && PIANO_USB_UFS_FETCH
#include "PianoUsbStorageExperiment.h"
#else
typedef EFI_STATUS (*PIANO_USB_STORAGE_CHECK)(VOID *Context,CONST CHAR8 *Phase,BOOLEAN FullCapture);
#endif
#if PIANO_USB_SCREENSHOT
#include "PianoFastbootScreen.h"
#endif
#endif
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
STATIC PIANO_DMA_BUFFER mRing,mTrbs[4],mSetup,mTx;
#if PIANO_USB_FASTBOOT
STATIC PIANO_DMA_BUFFER mBulkRx,mBulkTx;
#endif
STATIC PIANO_DMA_BUFFER *mPayload[4];
STATIC UINT8 mResource[4];STATIC BOOLEAN mPending[4],mEnding[4];
STATIC UINT32 mPosted[4];
STATIC UINTN mRingPosition;STATIC UINT8 mPhase,mStatusEp;
STATIC BOOLEAN mThreeStage,mConfigured;
STATIC PIANO_USB_CONTROL mControl;
STATIC BOOLEAN mRebootAfterCleanup;
BOOLEAN PianoDwc3ConsumeRebootRequest(VOID) {
  BOOLEAN Request=mRebootAfterCleanup;mRebootAfterCleanup=FALSE;return Request;
}
#if PIANO_USB_FASTBOOT
STATIC PIANO_FASTBOOT mFastboot;
STATIC PIANO_FB_STORAGE mExperimentStorage;
STATIC BOOLEAN mExperimentHasStorage,mExperimentContractFailed;
STATIC PIANO_USB_STORAGE_CHECK mExperimentCheck;
STATIC VOID *mExperimentCheckContext;
STATIC BOOLEAN mExperimentRunning;
STATIC BOOLEAN mBulkLive,mBulkPrepared,mConfigWaiting,mStatusWaiting;
STATIC UINT32 mFastLogBytes,mFastLogCrc,mFastLogGeneration;
#if PIANO_USB_SCREENSHOT
STATIC UINT32 mScreenWidth,mScreenHeight,mScreenBytes,mScreenCrc,mScreenGeneration;
STATIC VOID ClearScreenMetadata(VOID){mScreenWidth=mScreenHeight=mScreenBytes=mScreenCrc=mScreenGeneration=0;}
#endif
STATIC UINT64 mBulkOutBytes,mBulkInBytes;
// Send copies CPU frames immediately. Hardware sees only the separate 4K DMA
// bounce buffers, so reset can clear staged pools without touching active DMA.
typedef struct FB_FRAME {struct FB_FRAME *Next;UINTN Bytes,Offset;UINT8 Data[1];} FB_FRAME;
STATIC FB_FRAME *mFrames,*mFramesTail;
STATIC UINTN mFrameCount,mFrameBytes,mInFlightBytes;
#define FB_QUEUE_FRAMES 16U
#define FB_QUEUE_BYTES (PIANO_FASTBOOT_MAX_DOWNLOAD+1024U)
EFI_STATUS PianoDwc3SetStorageForExperiment(CONST PIANO_FB_STORAGE *Storage) {
  if(mExperimentRunning)return EFI_NOT_READY;
  if(Storage!=NULL && (Storage->Ready==NULL || Storage->Info==NULL || Storage->ReadBlocks==NULL))return EFI_INVALID_PARAMETER;
  if(Storage!=NULL)mExperimentStorage=*Storage;else ZeroMem(&mExperimentStorage,sizeof(mExperimentStorage));
  mExperimentHasStorage=Storage!=NULL;return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3SetStorageCheckForExperiment(PIANO_USB_STORAGE_CHECK Check,VOID *Context) {
  if(mExperimentRunning)return EFI_NOT_READY;
  if(Check==NULL && Context!=NULL)return EFI_INVALID_PARAMETER;
  mExperimentCheck=Check;mExperimentCheckContext=Context;return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3CheckStorageForExperiment(CONST CHAR8 *Phase,BOOLEAN FullCapture) {
  if(mExperimentContractFailed)return EFI_DEVICE_ERROR;
  if(mExperimentCheck==NULL)return EFI_SUCCESS;
  if(!mExperimentRunning || Phase==NULL)return EFI_NOT_READY;
  EFI_STATUS S=mExperimentCheck(mExperimentCheckContext,Phase,FullCapture);
  if(S!=EFI_SUCCESS){mExperimentContractFailed=TRUE;return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
#endif
// Vendor IN 5B exposes only fixed status registers and a bounded console copy.
// Snapshot storage is CPU-only; every reply uses the existing mapped mTx.
#define USB_DIAG_LOG_BYTES 65536U
#define USB_DIAG_PAGE_BYTES 512U
#define USB_DIAG_PAGE_HEADER 24U
#ifndef PIANO_USB_CONSOLE_BASE
#define PIANO_USB_CONSOLE_BASE 0xA3500000U
#endif
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
    if(Signature!=0x43474244 || Start>=Capacity || Size>Capacity || Start>Size)return EFI_COMPROMISED_DATA;
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
    EFI_STATUS Status=SnapshotConsole((CONST volatile UINT32 *)(UINTN)PIANO_USB_CONSOLE_BASE);
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
  // DWC3 requires USB2 ENBLSLPM/SUSPHY clear for HS/FS commands and END.
  UINT32 Phy=Dr(0xC200),Saved=0;
  if(!mControl.SuperSpeed || (Cmd&15)==8) {
    Saved=Phy&(BIT6|BIT8);if(Saved)Dw(0xC200,Phy&~Saved);
  }
  UINT32 Base=0xC800+Ep*16;
  Dw(Base,P2);Dw(Base+4,P1);Dw(Base+8,P0);Dw(Base+12,Cmd|BIT10);
  for(UINTN I=0;I<1000;++I) {
    UINT32 V=Dr(Base+12);
    if(!(V&BIT10)) {
      if(Ep<2 || (V&0xF000) || (Cmd&15)!=6)
        DEBUG((DEBUG_WARN,"SUNUEFI_USB_EPCMD ep=%u cmd=%x status=%u value=%08x\n",Ep,Cmd,V>>12&15,V));
      if(Saved)Dw(0xC200,Phy);
      if(V&0xF000)return EFI_DEVICE_ERROR;
      if((Cmd&15)==6)mResource[Ep]=(V>>16)&0x7F;
      return EFI_SUCCESS;
    }
    gBS->Stall(10);
  }
  if(Saved)Dw(0xC200,Phy);
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
  // EP0 legacy synchronous END path: USB31 has no END polling mode. Wait the
  // 1ms recommended by dwc3_stop_active_transfer before releasing its buffers.
  gBS->Stall(1000);
  Status=PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(Status))return Status;
  if(mPayload[Ep]!=NULL){Status=PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(Status))return Status;}
  mPending[Ep]=FALSE;mPayload[Ep]=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS Transfer(UINT8 Ep,UINT32 Type,PIANO_DMA_BUFFER *Payload,UINT32 Bytes,CONST CHAR8 *Name) {
  if(Ep>3 || mPending[Ep] || mEnding[Ep] || Payload==NULL || Bytes>Payload->Bytes)return EFI_NOT_READY;
  DWC_TRB *T=mTrbs[Ep].Cpu;
  *T=(DWC_TRB){(UINT32)Payload->DeviceAddress,(UINT32)(Payload->DeviceAddress>>32),Bytes,
    (Type<<4)|BIT0|BIT1|BIT10|BIT11};
  EFI_STATUS Status=PianoDmaBegin(Payload,Name);if(EFI_ERROR(Status))return Status;
  Status=PianoDmaBegin(&mTrbs[Ep],Name);
  if(EFI_ERROR(Status)){PianoDmaComplete(Payload,Status,TRUE);return Status;}
  // Even a failed START may have consumed the TRB; retain until END or halt.
  mPending[Ep]=TRUE;mPayload[Ep]=Payload;mPosted[Ep]=Bytes;
  Status=Command(Ep,6,(UINT32)(mTrbs[Ep].DeviceAddress>>32),(UINT32)mTrbs[Ep].DeviceAddress,0);
  if(Ep<2 || EFI_ERROR(Status))DEBUG((DEBUG_WARN,"SUNUEFI_USB_TRANSFER ep=%u type=%u bytes=%u trb_iova=%lx data_iova=%lx status=%r\n",
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
#if PIANO_USB_FASTBOOT
STATIC UINT32 BulkMps(VOID){return mControl.SuperSpeed?1024:(mControl.Speed==1 || mControl.Speed==3?64:512);}
STATIC VOID ClearFrames(VOID) {
  while(mFrames!=NULL){FB_FRAME *Next=mFrames->Next;ZeroMem(mFrames->Data,mFrames->Bytes);FreePool(mFrames);mFrames=Next;}
  mFramesTail=NULL;mFrameCount=mFrameBytes=mInFlightBytes=0;
}
STATIC VOID ClearFastboot(VOID) {
  ClearFrames();PianoFastbootReset(&mFastboot);
  mFastLogBytes=mFastLogCrc=mFastLogGeneration=0;mLogValid=FALSE;
  if(mLogSnapshot!=NULL)ZeroMem(mLogSnapshot,USB_DIAG_LOG_BYTES);
#if PIANO_USB_SCREENSHOT
  ClearScreenMetadata();
#endif
}
STATIC EFI_STATUS FastbootSend(VOID *Context,CONST VOID *Data,UINTN Bytes) {
  (VOID)Context;
  if(Data==NULL || !Bytes || Bytes>FB_QUEUE_BYTES || mFrameCount>=FB_QUEUE_FRAMES || mFrameBytes>FB_QUEUE_BYTES-Bytes)
    return EFI_OUT_OF_RESOURCES;
  FB_FRAME *Frame=AllocatePool(sizeof(*Frame)+Bytes-1);if(Frame==NULL)return EFI_OUT_OF_RESOURCES;
  Frame->Next=NULL;Frame->Bytes=Bytes;Frame->Offset=0;CopyMem(Frame->Data,Data,Bytes);
  if(mFramesTail!=NULL)mFramesTail->Next=Frame;else mFrames=Frame;
  mFramesTail=Frame;++mFrameCount;mFrameBytes+=Bytes;return EFI_SUCCESS;
}
STATIC VOID FastHex(UINT32 Value,CHAR8 *Out) {
  STATIC CONST CHAR8 Digits[]="0123456789abcdef";
  for(UINTN I=0;I<8;++I)Out[I]=Digits[(Value>>((7-I)*4))&15];
  Out[8]=0;
}
STATIC EFI_STATUS FastbootQuery(VOID *Context,CONST CHAR8 *Name,CHAR8 Value[60]) {
  (VOID)Context;
  if(!AsciiStrCmp(Name,"SunUEFI:log-size"))FastHex(mFastLogBytes,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:log-crc32"))FastHex(mFastLogCrc,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:log-generation"))FastHex(mFastLogGeneration,Value);
#if PIANO_USB_SCREENSHOT
  else if(!AsciiStrCmp(Name,"SunUEFI:screen-width"))FastHex(mScreenWidth,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:screen-height"))FastHex(mScreenHeight,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:screen-size"))FastHex(mScreenBytes,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:screen-crc32"))FastHex(mScreenCrc,Value);
  else if(!AsciiStrCmp(Name,"SunUEFI:screen-generation"))FastHex(mScreenGeneration,Value);
#endif
  else if(!AsciiStrCmp(Name,"SunUEFI:usb-state")) {
    CopyMem(Value,"configured-speed-",17);Value[17]=mControl.SuperSpeed?'S':(BulkMps()==64?'F':'H');Value[18]=0;
  } else return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FastbootDiagnostic(VOID *Context,PIANO_FASTBOOT *State,CONST CHAR8 *Cmd) {
  (VOID)Context;
  if(!AsciiStrCmp(Cmd,"oem ramlog")) {
    EFI_STATUS S=SnapshotConsole((CONST volatile UINT32 *)(UINTN)PIANO_USB_CONSOLE_BASE);
    if(!EFI_ERROR(S) && mLogBytes)S=PianoFastbootStageCopy(State,mLogSnapshot,mLogBytes);
    else if(!EFI_ERROR(S))S=EFI_NOT_FOUND;
    if(EFI_ERROR(S)){mFastLogBytes=mFastLogCrc=mFastLogGeneration=0;return FastbootSend(NULL,"FAILRAM log snapshot unavailable",32);}
    mFastLogBytes=mLogBytes;mFastLogCrc=mLogCrc;mFastLogGeneration=mLogGeneration;
#if PIANO_USB_SCREENSHOT
    ClearScreenMetadata();
#endif
    return FastbootSend(NULL,"OKAY",4);
  }
#if PIANO_USB_SCREENSHOT
  if(!AsciiStrCmp(Cmd,"oem screenshot")) {
    UINT32 W=0,H=0,Bytes=0,Crc=0;
    EFI_STATUS S=PianoFastbootCaptureScreen(State,&W,&H,&Bytes,&Crc);
    if(S!=EFI_SUCCESS || !W || !H || !Bytes || State->Upload==NULL || State->UploadBytes!=Bytes || State->UploadBorrowed) {
      ClearScreenMetadata();CONST CHAR8 *Failure="FAILGOP screenshot unavailable";return FastbootSend(NULL,Failure,AsciiStrLen(Failure));
    }
    mScreenWidth=W;mScreenHeight=H;mScreenBytes=Bytes;mScreenCrc=Crc;
    if(++mScreenGeneration==0)++mScreenGeneration;
    mFastLogBytes=mFastLogCrc=mFastLogGeneration=0;
    DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_SCREENSHOT width=%u height=%u bytes=%u crc32=%08x generation=%u\n",W,H,Bytes,Crc,mScreenGeneration));
    return FastbootSend(NULL,"OKAY",4);
  }
#endif
  if(!AsciiStrCmp(Cmd,"oem status")) {
    CONST CHAR8 *Rows[]={"INFOstorage-policy:no-persistent-writes","INFOtransport:owned-DMA-SMMU-bulk","INFOram-upload:download-or-frozen-log"};
    for(UINTN I=0;I<ARRAY_SIZE(Rows);++I){EFI_STATUS S=FastbootSend(NULL,Rows[I],AsciiStrLen(Rows[I]));if(EFI_ERROR(S))return S;}
    CHAR8 Row[60]="INFODSTS:";FastHex(Dr(0xC70C),Row+9);
    EFI_STATUS S=FastbootSend(NULL,Row,AsciiStrLen(Row));if(EFI_ERROR(S))return S;
    CopyMem(Row,"INFODCFG:",9);FastHex(Dr(0xC700),Row+9);
    S=FastbootSend(NULL,Row,AsciiStrLen(Row));if(EFI_ERROR(S))return S;
    return FastbootSend(NULL,"OKAY",4);
  }
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS BulkPump(VOID) {
  if(!mBulkLive || mEnding[2] || mEnding[3])return EFI_SUCCESS;
  if(mFrames!=NULL) {
    if(mPending[3])return EFI_SUCCESS;
    mInFlightBytes=MIN(mFrames->Bytes-mFrames->Offset,mBulkTx.Bytes);
    CopyMem(mBulkTx.Cpu,mFrames->Data+mFrames->Offset,mInFlightBytes);
    return Transfer(3,1,&mBulkTx,(UINT32)mInFlightBytes,"USB_FASTBOOT_IN");
  }
  if(mPending[3] || mPending[2] || mFastboot.ExitRequested || mFastboot.RebootRequested)return EFI_SUCCESS;
  UINTN Bytes=BulkMps(); // A command <=64 fits in one packet at every supported speed.
  if(mFastboot.Receiving) {
    UINTN Remaining=mFastboot.Expected-mFastboot.Received;
    Bytes=MIN(mBulkRx.Bytes,ALIGN_VALUE(Remaining,BulkMps()));
  }
  ZeroMem(mBulkRx.Cpu,mBulkRx.Bytes);
  return Transfer(2,1,&mBulkRx,(UINT32)Bytes,"USB_FASTBOOT_OUT");
}
STATIC EFI_STATUS BulkStop(VOID) {
  mBulkLive=mBulkPrepared=FALSE;ClearFastboot();
  for(UINT8 Ep=2;Ep<4;++Ep) {
    if(!mPending[Ep] || mEnding[Ep])continue;
    // CMDIOC's EPCMDCMPLT is the ownership boundary on DWC USB3.1.
    mEnding[Ep]=TRUE;
    EFI_STATUS S=Command(Ep,8|BIT8|BIT11|((UINT32)mResource[Ep]<<16),0,0,0);
    if(EFI_ERROR(S))return S; // Retain active buffers until global Halt.
  }
  Dw(0xC720,3);return EFI_SUCCESS;
}
STATIC EFI_STATUS BulkPrepare(VOID) {
  if(mEnding[2] || mEnding[3] || mPending[2] || mPending[3])return EFI_NOT_READY;
  // DEPSTARTCFG(0) and one transfer resource per physical endpoint are set
  // once at core initialization, like Linux dwc3_gadget_start_config.
  for(UINT8 Ep=2;Ep<4;++Ep) {
    UINT32 P0=(2U<<1)|(BulkMps()<<3)|((Ep&1)?(1U<<17):0); // IN FIFO1, OUT FIFO0.
    EFI_STATUS S=Command(Ep,1,P0,BIT8|BIT10|((UINT32)Ep<<25),0);if(EFI_ERROR(S))return S;
  }
  Dw(0xC720,15);mBulkPrepared=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_BULK_READY speed=%u mps=%u ep_out=01 ep_in=81\n",mControl.Speed,BulkMps()));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS StartStatus(VOID) {
  mStatusWaiting=FALSE;mPhase=3;
  return Transfer(mStatusEp,mThreeStage?4:3,mStatusEp?&mTx:&mSetup,0,"USB_EP0_STATUS");
}
STATIC EFI_STATUS FinishConfiguration(VOID) {
  if(!mConfigWaiting || mEnding[2] || mEnding[3])return EFI_SUCCESS;
  if(mControl.PendingConfiguration) {EFI_STATUS S=BulkPrepare();if(EFI_ERROR(S))return S;}
  mConfigWaiting=FALSE;
  return mStatusWaiting?StartStatus():EFI_SUCCESS;
}
STATIC EFI_STATUS BulkComplete(UINT8 Ep,UINT8 EventStatus) {
  if(!mPending[Ep])return mEnding[Ep] || !mBulkLive?EFI_SUCCESS:EFI_COMPROMISED_DATA;
  EFI_STATUS S=PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(S))return S;
  S=PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(S))return S;
  DWC_TRB *T=mTrbs[Ep].Cpu;UINT32 Residual=T->Size&0xFFFFFF,Status=T->Size>>28;
  mPending[Ep]=FALSE;mPayload[Ep]=NULL;
  if(mEnding[Ep])return EFI_SUCCESS; // Still wait for END before reuse.
  if((EventStatus&1) || (T->Control&1) || Status==4 || Residual>mPosted[Ep])return EFI_DEVICE_ERROR;
  UINTN Bytes=mPosted[Ep]-Residual;
  if(!mBulkLive)return EFI_SUCCESS;
  if(Ep==2) {
    mBulkOutBytes+=Bytes;
    // No OUT TRB is posted while a response is queued or IN owns its bounce.
    if(mFrames!=NULL || mPending[3])return EFI_COMPROMISED_DATA;
    BOOLEAN Download=mFastboot.Receiving;
    S=PianoFastbootPacket(&mFastboot,mBulkRx.Cpu,Bytes);if(EFI_ERROR(S))return S;
    if(mExperimentContractFailed)return EFI_DEVICE_ERROR; // Never start reply DMA after a failed CB contract.
    if(!Download)DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_COMMAND bytes=%u receiving=%u queued=%u\n",(UINT32)Bytes,mFastboot.Receiving,(UINT32)mFrameCount));
    if(mFastboot.Receiving || (Download && mFastboot.Complete) || mFastboot.Upload==NULL) {
      mFastLogBytes=mFastLogCrc=mFastLogGeneration=0;
#if PIANO_USB_SCREENSHOT
      ClearScreenMetadata();
#endif
    }
  } else {
    if(Residual || mFrames==NULL || Bytes!=mInFlightBytes)return EFI_DEVICE_ERROR;
    mBulkInBytes+=Bytes;mFrames->Offset+=Bytes;mInFlightBytes=0;
    if(mFrames->Offset==mFrames->Bytes) {
      FB_FRAME *Done=mFrames;mFrames=Done->Next;--mFrameCount;mFrameBytes-=Done->Bytes;
      ZeroMem(Done->Data,Done->Bytes);FreePool(Done);if(mFrames==NULL)mFramesTail=NULL;
    }
  }
  return BulkPump();
}
STATIC EFI_STATUS BulkEnded(UINT8 Ep,UINT32 E) {
  if(((E>>24)&15)!=8)return EFI_SUCCESS;
  if(!mEnding[Ep])return EFI_SUCCESS;
  if((E>>12)&15)return EFI_DEVICE_ERROR;
  if(mTrbs[Ep].Active){EFI_STATUS S=PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(S))return S;}
  if(mPayload[Ep]!=NULL && mPayload[Ep]->Active){EFI_STATUS S=PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(S))return S;}
  mPending[Ep]=mEnding[Ep]=FALSE;mPayload[Ep]=NULL;
  DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_END_COMPLETE ep=%u raw=%08x\n",Ep,E));
  return FinishConfiguration();
}
#endif
STATIC EFI_STATUS Complete(UINT8 Ep) {
  if(Ep>1 || !mPending[Ep])return EFI_COMPROMISED_DATA;
  EFI_STATUS Dma=PianoDmaComplete(&mTrbs[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(Dma))return Dma;
  Dma=PianoDmaComplete(mPayload[Ep],EFI_SUCCESS,TRUE);if(EFI_ERROR(Dma))return Dma;
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
#if PIANO_USB_FASTBOOT
    if(mControl.SetConfiguration) {
      S=BulkStop();if(EFI_ERROR(S))return S;
      mConfigWaiting=TRUE;mStatusWaiting=FALSE;
      S=FinishConfiguration();if(EFI_ERROR(S))return S;
    }
#endif
    if(mThreeStage){mPhase=1;return Transfer(1,5,&mTx,(UINT32)Bytes,"USB_EP0_DATA_IN");}
    mPhase=2;return EFI_SUCCESS; // Wait for STATUS XferNotReady.
  }
  if(mPhase==1){mPhase=2;return EFI_SUCCESS;}
  if(mPhase==3) {
    BOOLEAN Address=mControl.SetAddress,Config=mControl.SetConfiguration;
    PianoUsbControlStatusComplete(&mControl);
    if(Address || Config)DEBUG((DEBUG_WARN,"SUNUEFI_USB_ENUM_STATE address=%u configuration=%u dcfg=%08x\n",mControl.Address,mControl.Configuration,Dr(0xC700)));
    mConfigured=mControl.Configuration==1;
#if PIANO_USB_FASTBOOT
    if(Config){mBulkLive=mConfigured && mBulkPrepared;EFI_STATUS S=BulkPump();if(EFI_ERROR(S))return S;}
#endif
    return ArmSetup();
  }
  return EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS Event(UINT32 E) {
  if(E&1) {
    ++mDeviceEvents;
    UINT32 Type=(E>>8)&15;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_EVENT type=%u raw=%08x dsts=%08x\n",Type,E,Dr(0xC70C)));
    if(Type==0 || Type==1) {
#if PIANO_USB_FASTBOOT
      EFI_STATUS Bulk=BulkStop();if(EFI_ERROR(Bulk))return Bulk;
      mConfigWaiting=mStatusWaiting=FALSE;
#endif
      // End each own transfer before returning its memory to the CPU.
      EFI_STATUS S=StopTransfer(0);if(!EFI_ERROR(S))S=StopTransfer(1);if(EFI_ERROR(S))return S;
      BOOLEAN Super=mControl.SuperSpeed;UINT8 Speed=mControl.Speed;
      ZeroMem(&mControl,sizeof(mControl));mControl.SuperSpeed=Super;mControl.Speed=Speed;
      mConfigured=FALSE;mLogValid=FALSE;
      if(Type==0){mPhase=0;Dw(0xC700,Dr(0xC700)&~(0x7FU<<3));return EFI_SUCCESS;}
      Dw(0xC700,Dr(0xC700)&~(0x7FU<<3));return ArmSetup();
    }
    if(Type==2) {
      mControl.Speed=(UINT8)(Dr(0xC70C)&7);mControl.SuperSpeed=mControl.Speed>=4;
      EFI_STATUS S=ConfigureEp(0,TRUE);if(!EFI_ERROR(S))S=ConfigureEp(1,TRUE);
      if(!EFI_ERROR(S) && !mPending[0] && !mPending[1])S=ArmSetup();
      return S;
    }
    if(Type==9)return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
  }
  UINT8 Ep=(E>>1)&31,Type=(E>>6)&15,Status=(E>>12)&15;
  if(Ep<2 || Type==7 || (Status&1))DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP_EVENT ep=%u type=%u status=%u raw=%08x\n",Ep,Type,Status,E));
  if(Ep>1) {
#if PIANO_USB_FASTBOOT
    if(Ep>3)return EFI_UNSUPPORTED;
    if(Type==7)return BulkEnded(Ep,E);
    if(Type==1)return BulkComplete(Ep,Status);
    return EFI_SUCCESS;
#else
    return EFI_UNSUPPORTED;
#endif
  }
  if(Type==1)return Complete(Ep);
  if(Type==3 && mPhase==2 && (Status&3)==2) {
    if(Ep!=mStatusEp)return EFI_COMPROMISED_DATA;
#if PIANO_USB_FASTBOOT
    if(mConfigWaiting){mStatusWaiting=TRUE;return EFI_SUCCESS;}
    return StartStatus();
#else
    mPhase=3;return Transfer(Ep,mThreeStage?4:3,Ep?&mTx:&mSetup,0,"USB_EP0_STATUS");
#endif
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device) {
#if PIANO_USB_FASTBOOT
  if(mExperimentRunning)return EFI_ALREADY_STARTED;
  mExperimentRunning=TRUE;mExperimentContractFailed=FALSE;
#endif
#if PIANO_USB_FASTBOOT
  PIANO_DMA_BUFFER *Buffers[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx,&mTrbs[2],&mTrbs[3],&mBulkRx,&mBulkTx};
  PIANO_DMA_DIRECTION Directions[]={PianoDmaFromDevice,PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice,
    PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice};
#else
  PIANO_DMA_BUFFER *Buffers[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx};
  PIANO_DMA_DIRECTION Directions[]={PianoDmaFromDevice,PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice};
#endif
  UINT32 OldGctl=Dr(0xC110),OldDcfg=Dr(0xC700),OldSize=Dr(0xC408),OldLow=Dr(0xC400),OldHigh=Dr(0xC404),Count=0;
  UINT32 OldSessionHs=Dr(USB_SESSION_HS),OldSessionSs=Dr(USB_SESSION_SS);
  BOOLEAN SessionSet=FALSE,RebootReady=FALSE;
  mRebootAfterCleanup=FALSE;
  EFI_STATUS Status=EFI_SUCCESS;mRingPosition=0;mConfigured=FALSE;
  mLogValid=FALSE;mLogBytes=mLogCrc=mLogGeneration=0;mLogFlags=0;
  mDeviceEvents=mSetupEvents=mDiagReplies=0;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SESSION_SAVED hs=%08x ss=%08x dsts=%08x\n",OldSessionHs,OldSessionSs,Dr(0xC70C)));
  ZeroMem(&mControl,sizeof(mControl));ZeroMem(mPending,sizeof(mPending));ZeroMem(mPayload,sizeof(mPayload));
  ZeroMem(mEnding,sizeof(mEnding));mControl.SuperSpeed=TRUE;mControl.Speed=4;
#if PIANO_USB_FASTBOOT
  mBulkLive=mBulkPrepared=mConfigWaiting=mStatusWaiting=FALSE;
  ClearFastboot();PianoFastbootInit(&mFastboot,NULL,FastbootSend,NULL);
  if(mExperimentHasStorage) {
    Status=PianoFastbootSetStorage(&mFastboot,&mExperimentStorage);
    if(EFI_ERROR(Status))goto Exit;
  }
  mFastboot.Query=FastbootQuery;mFastboot.Diagnostic=FastbootDiagnostic;
  mBulkOutBytes=mBulkInBytes=0;
#endif
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
#if PIANO_USB_FASTBOOT
  Status=PianoDwc3CheckStorageForExperiment("coexist-dwc-mapped",TRUE);if(Status!=EFI_SUCCESS)goto Exit;
#endif
  Dw(0xC110,(OldGctl&~(3U<<12))|(2U<<12)|BIT0); // Device role, retain PHY configuration.
  Dw(0xC700,(OldDcfg&~(7U|(0x7FU<<3)|(31U<<17)))|4U|(16U<<17)); // Retained SS path, address zero.
  Dw(0xC400,(UINT32)mRing.DeviceAddress);Dw(0xC404,(UINT32)(mRing.DeviceAddress>>32));
  Dw(0xC408,4096);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Status=PianoDmaBegin(&mRing,"USB_EVENT_RING");if(EFI_ERROR(Status))goto Exit;
  Status=Command(0,9,0,0,0);if(EFI_ERROR(Status))goto Exit;
  Status=ConfigureEp(0,FALSE);if(!EFI_ERROR(Status))Status=ConfigureEp(1,FALSE);if(EFI_ERROR(Status))goto Exit;
#if PIANO_USB_FASTBOOT
  Status=Command(2,2,1,0,0);if(!EFI_ERROR(Status))Status=Command(3,2,1,0,0);if(EFI_ERROR(Status))goto Exit;
#endif
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
#if PIANO_USB_FASTBOOT
  Status=PianoDwc3CheckStorageForExperiment("coexist-before-run",TRUE);if(Status!=EFI_SUCCESS)goto Exit;
#endif
  Dw(0xC704,(Dr(0xC704)&~(BIT9|BIT10|BIT11|BIT12))|BIT31);
  gBS->Stall(1000);
  Status=ArmSetup();if(EFI_ERROR(Status))goto Exit;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP0_RUN dsts=%08x event_iova=%lx\n",Dr(0xC70C),mRing.DeviceAddress));
  for(UINTN I=0;I<90000;++I) {
    Count=Dr(0xC40C)&0xFFFF;
    if(Count) {
      if(Count>4096 || (Count&3)){Status=EFI_COMPROMISED_DATA;break;}
      Status=PianoDmaSyncForCpuQuiet(&mRing);if(EFI_ERROR(Status))break;
      for(UINT32 N=0;N<Count;N+=4) {
        UINT32 E=((UINT32 *)mRing.Cpu)[mRingPosition/4];mRingPosition=(mRingPosition+4)%4096;
        Status=Event(E);if(EFI_ERROR(Status))break;
      }
      Dw(0xC40C,Count);if(EFI_ERROR(Status))break;
    }
#if PIANO_USB_FASTBOOT
    if(!mFrames && !mPending[3] && (mFastboot.ExitRequested || mFastboot.RebootRequested))break;
#endif
    gBS->Stall(1000);
  }
  if(!mConfigured && !EFI_ERROR(Status))Status=EFI_NOT_READY;
  PianoDmaReportQuietSync(&mRing);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_EP0_RESULT status=%r configured=%u address=%u configuration=%u\n",Status,mConfigured,mControl.Address,mControl.Configuration));
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_FINAL_REGS gctl=%08x dcfg=%08x dctl=%08x dsts=%08x gevntcount=%08x usb2phy=%08x usb3pipe=%08x\n",
    Dr(0xC110),Dr(0xC700),Dr(0xC704),Dr(0xC70C),Dr(0xC40C),Dr(0xC200),Dr(0xC2C0)));
  PianoSmmuLogFaults(&Context->After);
#if PIANO_USB_FASTBOOT
  DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_RESULT status=%r configured=%u out_bytes=%lu in_bytes=%lu queue=%u\n",
    Status,mConfigured,mBulkOutBytes,mBulkInBytes,(UINT32)mFrameCount));
#endif
Exit:
#if PIANO_USB_FASTBOOT
  RebootReady=mFastboot.RebootRequested && mFrames==NULL && !mPending[3] && !mEnding[3];
#endif
  {EFI_STATUS Quiet=Halt();
    if(EFI_ERROR(Quiet)) {
      PianoSmmuLogFaults(&Context->After);
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_DMA_UNQUIESCED_RESET buffers_retained=1\n"));
      gRT->ResetSystem(EfiResetCold,Quiet,0,NULL);
      CpuDeadLoop();return Quiet; // A failed reset must never let caller remove clocks/SMMU.
    }}
  // Never remove session-valid while the core may still own DMA buffers.
  if(SessionSet) {
    Dw(USB_SESSION_SS,OldSessionSs);Dw(USB_SESSION_HS,OldSessionHs);
    UINT32 RestoredHs=Dr(USB_SESSION_HS),RestoredSs=Dr(USB_SESSION_SS);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SESSION_RESTORE hs_saved=%08x ss_saved=%08x hs_read=%08x ss_read=%08x dsts=%08x\n",
      OldSessionHs,OldSessionSs,RestoredHs,RestoredSs,Dr(0xC70C)));
    if(RestoredHs!=OldSessionHs || RestoredSs!=OldSessionSs)Status=EFI_DEVICE_ERROR;
  }
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Active) {
    EFI_STATUS S=PianoDmaComplete(Buffers[I],Status,TRUE);if(EFI_ERROR(S))Status=S;
  }
  Dw(0xC708,0);Dw(0xC408,BIT31);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Dw(0xC400,OldLow);Dw(0xC404,OldHigh);Dw(0xC408,OldSize|BIT31);Dw(0xC700,OldDcfg);Dw(0xC110,OldGctl);
  UINTN Retained=0;
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I) {
#if PIANO_USB_FASTBOOT
    if(mExperimentContractFailed && Buffers[I]->Signature)Buffers[I]->Quarantined=TRUE;
#endif
    if(Buffers[I]->Signature){EFI_STATUS S=PianoDmaFree(Buffers[I]);if(EFI_ERROR(S))Status=S;}
    if(Buffers[I]->Signature)++Retained;
  }
  if(Retained && !EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  mRebootAfterCleanup=RebootReady && Status==EFI_SUCCESS && Retained==0;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_CLEANUP status=%r buffers_retained=%u reboot_acknowledged=%u reboot_ready=%u\n",
    Status,(UINT32)Retained,RebootReady,mRebootAfterCleanup));
#if PIANO_USB_FASTBOOT
  ClearFastboot();
  if(Retained==0)mExperimentRunning=FALSE; // Clean Halt errors/NOTREADY may be retried; retained ownership may not.
#endif
  if(mLogSnapshot!=NULL){ZeroMem(mLogSnapshot,USB_DIAG_LOG_BYTES);FreePool(mLogSnapshot);mLogSnapshot=NULL;}mLogValid=FALSE;
  return Status;
}
