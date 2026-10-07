// SPDX-License-Identifier: BSD-2-Clause-Patent
// DWC3 EP0 and RAM-only standard fastboot; all hardware DMA uses shared SMMU.
#include "PianoOwnedSmmu.h"
#include "PianoUsbControl.h"
#include "PianoDwc3Service.h"
#if PIANO_USB_SERVICE
#include "Protocol/PianoProductRuntime.h"
#endif
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
STATIC PIANO_SMMU_USB_RETIRE_EVIDENCE mDeviceRetire;
EFI_STATUS PianoDwc3GetRetireEvidence(PIANO_SMMU_USB_RETIRE_EVIDENCE *Evidence) {
  if(Evidence==NULL)return EFI_INVALID_PARAMETER;
  *Evidence=mDeviceRetire;
  return Evidence->Revision==1 && Evidence->DeviceCleanupStatus==EFI_SUCCESS && Evidence->DeviceHalted && Evidence->DmaFreed &&
    Evidence->DmaBuffersFreed==9?EFI_SUCCESS:EFI_NOT_READY;
}
BOOLEAN PianoDwc3ConsumeRebootRequest(VOID) {
  BOOLEAN Request=mRebootAfterCleanup;mRebootAfterCleanup=FALSE;return Request;
}
#if PIANO_USB_FASTBOOT
STATIC PIANO_FASTBOOT mFastboot;
#if PIANO_USB_RAM_BOOT
STATIC PIANO_FB_BOOT mExperimentBoot;
STATIC BOOLEAN mExperimentHasBoot,mBootAckObserved,mBootActionValid;
STATIC PIANO_FB_BOOT_ACTION mBootAction;
#endif
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
EFI_STATUS PianoDwc3SetBootForExperiment(CONST PIANO_FB_BOOT *Boot) {
  if(mExperimentRunning)return EFI_NOT_READY;
#if PIANO_USB_RAM_BOOT
  if(mBootActionValid)return EFI_NOT_READY;
  if(Boot!=NULL && (!Boot->MaxImageBytes || Boot->Ready==NULL || Boot->Validate==NULL || Boot->TakeAfterAck==NULL))return EFI_INVALID_PARAMETER;
  if(Boot!=NULL)mExperimentBoot=*Boot;else ZeroMem(&mExperimentBoot,sizeof(mExperimentBoot));mExperimentHasBoot=Boot!=NULL;return EFI_SUCCESS;
#else
  return Boot==NULL?EFI_SUCCESS:EFI_UNSUPPORTED;
#endif
}
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action) {
  if(Action==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Action,sizeof(*Action));
#if PIANO_USB_RAM_BOOT
  if(!mBootActionValid)return EFI_NOT_FOUND;
  *Action=mBootAction;ZeroMem(&mBootAction,sizeof(mBootAction));mBootActionValid=FALSE;return Action->Status;
#else
  return EFI_NOT_FOUND;
#endif
}
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
#define USB_DIAG_LOG_BYTES 262144U // bounded CPU snapshot; UFS fetch/DMA unchanged
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
STATIC EFI_STATUS CompleteEventDma(PIANO_DMA_BUFFER *Buffer) {
  EFI_STATUS S=PianoDmaComplete(Buffer,EFI_SUCCESS,TRUE);
#if PIANO_USB_FASTBOOT && (PIANO_USB_RAM_BOOT || PIANO_USB_SERVICE)
  if(S!=EFI_SUCCESS){Buffer->Quarantined=TRUE;mFastboot.BootTransferFrozen=TRUE;return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
#endif
  return S;
}
STATIC EFI_STATUS StopTransfer(UINT8 Ep) {
  if(!mPending[Ep])return EFI_SUCCESS;
  EFI_STATUS Status=Command(Ep,8|BIT11|((UINT32)mResource[Ep]<<16),0,0,0);
  if(EFI_ERROR(Status))return Status;
  // EP0 legacy synchronous END path: USB31 has no END polling mode. Wait the
  // 1ms recommended by dwc3_stop_active_transfer before releasing its buffers.
  gBS->Stall(1000);
  Status=CompleteEventDma(&mTrbs[Ep]);if(EFI_ERROR(Status))return Status;
  if(mPayload[Ep]!=NULL){Status=CompleteEventDma(mPayload[Ep]);if(EFI_ERROR(Status))return Status;}
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
#if PIANO_USB_RAM_BOOT
  if(!mFastboot.BootTransferFrozen)mBootAckObserved=FALSE;
#endif
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
#if PIANO_USB_SERVICE
STATIC EFI_STATUS ServiceRequestUi(UINT32 Action);
STATIC EFI_STATUS ServiceBeforeRamlog(BOOLEAN *ServicesLost);
#endif
STATIC EFI_STATUS FastbootDiagnostic(VOID *Context,PIANO_FASTBOOT *State,CONST CHAR8 *Cmd) {
  (VOID)Context;
#if PIANO_USB_SERVICE
  UINT32 UiAction=!AsciiStrCmp(Cmd,"oem setup")?PIANO_PRODUCT_ACTION_SETUP:
    !AsciiStrCmp(Cmd,"oem shell")?PIANO_PRODUCT_ACTION_SHELL:
    !AsciiStrCmp(Cmd,"oem simpleinit")?PIANO_PRODUCT_ACTION_SIMPLEINIT:
    !AsciiStrCmp(Cmd,"oem boot-stable")?PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE:PIANO_PRODUCT_ACTION_NONE;
  if(UiAction!=PIANO_PRODUCT_ACTION_NONE) {
    EFI_STATUS UiStatus=ServiceRequestUi(UiAction);
    // An actual EBS fence forbids even a FAIL response DMA. Ordinary missing
    // runtime/backend rejection keeps the resident owner and replies normally.
    if(UiStatus==EFI_ABORTED)return UiStatus;
    CONST CHAR8 *Reply=UiStatus==EFI_SUCCESS?"OKAY":"FAILUI navigation backend unavailable";
    return FastbootSend(NULL,Reply,AsciiStrLen(Reply));
  }
#endif
  if(!AsciiStrCmp(Cmd,"oem ramlog")) {
    EFI_STATUS S=EFI_SUCCESS;
#if PIANO_USB_SERVICE
    BOOLEAN ServicesLost=FALSE;S=ServiceBeforeRamlog(&ServicesLost);
    if(ServicesLost)return EFI_ABORTED; // EBS forbids snapshot allocation/FAIL DMA.
#endif
    if(S==EFI_SUCCESS)S=SnapshotConsole((CONST volatile UINT32 *)(UINTN)PIANO_USB_CONSOLE_BASE);
    if(!EFI_ERROR(S) && mLogBytes)S=PianoFastbootStageCopy(State,mLogSnapshot,mLogBytes);
    else if(!EFI_ERROR(S))S=EFI_NOT_FOUND;
    if(EFI_ERROR(S)){mFastLogBytes=mFastLogCrc=mFastLogGeneration=0;mLogValid=FALSE;return FastbootSend(NULL,"FAILRAM log snapshot unavailable",32);}
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
  if(mPending[3] || mPending[2] || mFastboot.ExitRequested || mFastboot.RebootRequested || mFastboot.BootPending || mFastboot.BootTransferFrozen)return EFI_SUCCESS;
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
  EFI_STATUS S=CompleteEventDma(&mTrbs[Ep]);if(EFI_ERROR(S))return S;
  S=CompleteEventDma(mPayload[Ep]);if(EFI_ERROR(S))return S;
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
#if PIANO_USB_RAM_BOOT
      if(mFastboot.BootPending && mFrames->Next==NULL && mFrames->Bytes==4 && !CompareMem(mFrames->Data,"OKAY",4))mBootAckObserved=TRUE;
#endif
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
  if(mTrbs[Ep].Active){EFI_STATUS S=CompleteEventDma(&mTrbs[Ep]);if(EFI_ERROR(S))return S;}
  if(mPayload[Ep]!=NULL && mPayload[Ep]->Active){EFI_STATUS S=CompleteEventDma(mPayload[Ep]);if(EFI_ERROR(S))return S;}
  mPending[Ep]=mEnding[Ep]=FALSE;mPayload[Ep]=NULL;
  DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_END_COMPLETE ep=%u raw=%08x\n",Ep,E));
  return FinishConfiguration();
}
#endif
STATIC EFI_STATUS Complete(UINT8 Ep) {
  if(Ep>1 || !mPending[Ep])return EFI_COMPROMISED_DATA;
  EFI_STATUS Dma=CompleteEventDma(&mTrbs[Ep]);if(EFI_ERROR(Dma))return Dma;
  Dma=CompleteEventDma(mPayload[Ep]);if(EFI_ERROR(Dma))return Dma;
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
#if PIANO_USB_RAM_BOOT
  if(mBootActionValid)return EFI_ALREADY_STARTED;
#endif
  mExperimentRunning=TRUE;mExperimentContractFailed=FALSE;
#endif
  ZeroMem(&mDeviceRetire,sizeof(mDeviceRetire));BOOLEAN RetireExact=TRUE;UINTN ActuallyFreed=0;
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
#if PIANO_USB_FASTBOOT && PIANO_USB_RAM_BOOT
  BOOLEAN BootAckReady=FALSE;
  BOOLEAN CleanupUncertain[ARRAY_SIZE(Buffers)];ZeroMem(CleanupUncertain,sizeof(CleanupUncertain));
#endif
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
#if PIANO_USB_RAM_BOOT
  mBootAckObserved=FALSE;
  if(mExperimentHasBoot){Status=PianoFastbootSetBoot(&mFastboot,&mExperimentBoot);if(Status!=EFI_SUCCESS)goto Exit;}
#endif
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
#if PIANO_USB_RAM_BOOT
    if(mFastboot.BootPending && mBootAckObserved && !mFrames && !mPending[3] && !mEnding[3]) {
      mFastboot.BootTransferFrozen=TRUE;BootAckReady=TRUE;break;
    }
#endif
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
    if(Quiet==EFI_SUCCESS)mDeviceRetire.DeviceHalted=(Dr(0xC70C)&BIT22)!=0 && !(Dr(0xC704)&BIT31);
    else RetireExact=FALSE;
    if(EFI_ERROR(Quiet)) {
      PianoSmmuLogFaults(&Context->After);
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_DMA_UNQUIESCED_RESET buffers_retained=1\n"));
#if PIANO_USB_FASTBOOT && PIANO_USB_RAM_BOOT
      mFastboot.BootTransferFrozen=TRUE;
      mBootAction=(PIANO_FB_BOOT_ACTION){.Context=mFastboot.Boot.Context,.View=mFastboot.BootView,.Status=Quiet,.Retained=TRUE,
        .Proof={.AckCompleted=mBootAckObserved,.QueueEmpty=mFrames==NULL && !mPending[3],.DispatchFrozen=TRUE,.AckBytes=mBootAckObserved?4:0}};mBootActionValid=TRUE;
#else
      gRT->ResetSystem(EfiResetCold,Quiet,0,NULL);
#endif
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
#if PIANO_USB_FASTBOOT && PIANO_USB_RAM_BOOT
    EFI_STATUS S=PianoDmaComplete(Buffers[I],EFI_SUCCESS,TRUE); // Halt is proven; preserve the operation error separately.
    if(S!=EFI_SUCCESS){CleanupUncertain[I]=TRUE;Buffers[I]->Quarantined=TRUE;Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
#else
    EFI_STATUS S=PianoDmaComplete(Buffers[I],Status,TRUE);
    if(EFI_ERROR(S))Status=S;
#endif
    if(S!=EFI_SUCCESS)RetireExact=FALSE;
  }
  Dw(0xC708,0);Dw(0xC408,BIT31);Count=Dr(0xC40C)&0xFFFF;if(Count)Dw(0xC40C,Count);
  Dw(0xC400,OldLow);Dw(0xC404,OldHigh);Dw(0xC408,OldSize|BIT31);Dw(0xC700,OldDcfg);Dw(0xC110,OldGctl);
  UINTN Retained=0;
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I) {
#if PIANO_USB_FASTBOOT
    if(mExperimentContractFailed && Buffers[I]->Signature)Buffers[I]->Quarantined=TRUE;
#endif
    if(Buffers[I]->Signature){EFI_STATUS S=PianoDmaFree(Buffers[I]);
#if PIANO_USB_FASTBOOT && PIANO_USB_RAM_BOOT
      if(S!=EFI_SUCCESS){CleanupUncertain[I]=TRUE;Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
#else
      if(EFI_ERROR(S))Status=S;
#endif
      if(S==EFI_SUCCESS && !Buffers[I]->Signature)++ActuallyFreed;else RetireExact=FALSE;
    }
    if(Buffers[I]->Signature)++Retained;
  }
  if(Retained && !EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  mRebootAfterCleanup=RebootReady && Status==EFI_SUCCESS && Retained==0;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_CLEANUP status=%r buffers_retained=%u reboot_acknowledged=%u reboot_ready=%u\n",
    Status,(UINT32)Retained,RebootReady,mRebootAfterCleanup));
#if PIANO_USB_FASTBOOT
#if PIANO_USB_RAM_BOOT
  UINTN Uncertain=0;for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(CleanupUncertain[I])++Uncertain;
  if(BootAckReady || (mFastboot.BootPending && mFastboot.BootTransferFrozen)) {
    mFastboot.BootProof=(PIANO_FB_BOOT_PROOF){.AckCompleted=mBootAckObserved,.QueueEmpty=mFrames==NULL && !mPending[3],
      .DeviceHalted=(Dr(0xC70C)&BIT22)!=0 && !(Dr(0xC704)&BIT31),.DmaFreed=Retained==0 && Uncertain==0,.DispatchFrozen=mFastboot.BootTransferFrozen,
      .AckBytes=mBootAckObserved?4:0,.DmaBuffersFreed=(UINT32)(ARRAY_SIZE(Buffers)-Retained)};
    mBootAction=(PIANO_FB_BOOT_ACTION){.Context=mFastboot.Boot.Context,.View=mFastboot.BootView,.Proof=mFastboot.BootProof,.Status=Status,.Retained=TRUE};
    mBootActionValid=TRUE;
    if(BootAckReady && Status==EFI_SUCCESS && Retained==0 && Uncertain==0 && mFastboot.BootProof.DeviceHalted && mFastboot.Download==mFastboot.BootValidatedDownload &&
       mFastboot.Received==mFastboot.BootValidatedBytes && mFastboot.Expected==mFastboot.BootValidatedBytes && mFastboot.Complete &&
       mFastboot.UploadBorrowed && mFastboot.Upload==mFastboot.Download && mFastboot.UploadBytes==mFastboot.Received) {
      EFI_STATUS Take=mFastboot.Boot.Validate(mFastboot.Boot.Context,&mFastboot,&mFastboot.BootView);
      if(Take==EFI_SUCCESS && mFastboot.Download==mFastboot.BootValidatedDownload && mFastboot.Received==mFastboot.BootValidatedBytes)
        Take=mFastboot.Boot.TakeAfterAck(mFastboot.Boot.Context,&mFastboot,&mFastboot.BootView,&mBootAction.Token);
      else if(Take==EFI_SUCCESS)Take=EFI_COMPROMISED_DATA;
      if(Take==EFI_SUCCESS && mBootAction.Token!=NULL && mFastboot.Download==NULL && mFastboot.Upload==NULL && !mFastboot.UploadBytes &&
         !mFastboot.UploadBorrowed && !mFastboot.Expected && !mFastboot.Received && !mFastboot.Receiving && !mFastboot.Complete) {
        mBootAction.Status=EFI_SUCCESS;mBootAction.Taken=TRUE;mBootAction.Retained=FALSE;mFastboot.BootTransferFrozen=FALSE;
      } else {Status=Take==EFI_SUCCESS?EFI_COMPROMISED_DATA:EFI_ERROR(Take)?Take:EFI_DEVICE_ERROR;mBootAction.Status=Status;}
    } else {if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;mBootAction.Status=Status;}
    DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_BOOT_TRANSFER status=%r taken=%u retained=%u ack=%u halted=%u dma_freed=%u\n",
      mBootAction.Status,mBootAction.Taken,mBootAction.Retained,mBootAction.Proof.AckCompleted,mBootAction.Proof.DeviceHalted,mBootAction.Proof.DmaBuffersFreed));
  }
  if(Uncertain){mFastboot.BootTransferFrozen=TRUE;}
#endif
  ClearFastboot();
  if(Retained==0 && !mFastboot.BootTransferFrozen)mExperimentRunning=FALSE; // Clean errors may retry; unknown ownership may not.
#endif
  if(mLogSnapshot!=NULL){ZeroMem(mLogSnapshot,USB_DIAG_LOG_BYTES);FreePool(mLogSnapshot);mLogSnapshot=NULL;}mLogValid=FALSE;
  mDeviceRetire.Revision=1;mDeviceRetire.DeviceCleanupStatus=Status;mDeviceRetire.DmaBuffersFreed=(UINT32)ActuallyFreed;
  mDeviceRetire.DmaFreed=RetireExact && Retained==0 && ActuallyFreed==9;
  return Status;
}

#if PIANO_USB_FASTBOOT && PIANO_USB_SERVICE
#define SERVICE_EVENTS 64U
STATIC struct {
  PIANO_OWNED_SMMU *Context;PIANO_DMA_DEVICE *Device;PIANO_DWC3_SERVICE_CONFIG Config;
  PIANO_DWC3_SERVICE_STATUS State;
  UINT32 Events[SERVICE_EVENTS];UINTN Head,Count;
  UINT32 OldGctl,OldDcfg,OldSize,OldLow,OldHigh,OldSessionHs,OldSessionSs;
  BOOLEAN SessionSet,Uncertain;
  BOOLEAN CsftrstTimeoutPreDma,EverStarted,CleanupCompleted,RegistersProgrammed,ResetOnlyRetired;
  UINT32 OldEventCount,OldEventsEnabled,OldEndpointsEnabled;
  EFI_STATUS StartStatus,CleanupStatus;
  UINT32 DmaBuffersAllocated;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *UiRuntime;
  PIANO_PRODUCT_RUNTIME_PROTOCOL UiMethods;
} mService;
STATIC PIANO_DMA_BUFFER *CONST mServiceBuffers[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx,&mTrbs[2],&mTrbs[3],&mBulkRx,&mBulkTx};
STATIC BOOLEAN ServiceBuffersEmpty(VOID) {
  for(UINTN I=0;I<ARRAY_SIZE(mServiceBuffers);++I){PIANO_DMA_BUFFER *B=mServiceBuffers[I];
    if(B->Signature || B->Cpu || B->Bytes || B->DeviceAddress || B->Active || B->Mapped || B->Quarantined || B->ExitRetained)return FALSE;}
  return TRUE;
}
STATIC BOOLEAN ServiceRestoreReadback(UINT32 Offset,UINT32 Expected) {
  UINT32 Current=Dr(Offset);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=restore offset=%04x expected=%08x current=%08x match=%u\n",Offset,Expected,Current,Expected==Current));
  return Expected==Current;
}
STATIC BOOLEAN ServiceAtApp(VOID) {
  if(mService.State.ServicesLost || gBS==NULL || gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL)return FALSE;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);return Old==TPL_APPLICATION;
}
STATIC EFI_STATUS ServiceBeforeRamlog(BOOLEAN *ServicesLost) {
  *ServicesLost=mService.State.ServicesLost;if(*ServicesLost)return EFI_ABORTED;
  EFI_STATUS (*Callback)(VOID *)=mService.Config.BeforeRamlog;
  if(Callback==NULL)return EFI_SUCCESS; // Preserve unbound/legacy diagnostics.
  if(!mService.State.Started || mService.State.Phase!=PianoUsbServiceListening || mService.State.Retained || !mExperimentRunning)return EFI_NOT_READY;
  if(!ServiceAtApp())return EFI_UNSUPPORTED;
  *ServicesLost=mService.State.ServicesLost;if(*ServicesLost)return EFI_ABORTED;
  EFI_STATUS S=Callback(mService.Config.Context); // Once per command, before snapshot.
  *ServicesLost=mService.State.ServicesLost;if(*ServicesLost)return EFI_ABORTED;
  return S==EFI_SUCCESS?EFI_SUCCESS:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC BOOLEAN SameUiRuntime(CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *A,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *B) {
  return A->Revision==B->Revision && A->Pump==B->Pump && A->BootServicesAlive==B->BootServicesAlive &&
    A->RequestAction==B->RequestAction && A->GetPendingAction==B->GetPendingAction && A->AckAction==B->AckAction;
}
STATIC EFI_STATUS ServiceRequestUi(UINT32 Action) {
  if(mService.State.ServicesLost)return EFI_ABORTED;
  if(!mService.State.Started || mService.State.Phase!=PianoUsbServiceListening || mService.State.Retained ||
     !mExperimentRunning || !ServiceAtApp() || gBS->LocateProtocol==NULL)return EFI_NOT_READY;
  if((Action<PIANO_PRODUCT_ACTION_SIMPLEINIT || Action>PIANO_PRODUCT_ACTION_SHELL)&&Action!=PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE)return EFI_INVALID_PARAMETER;
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Runtime);
  if(mService.State.ServicesLost)return EFI_ABORTED;
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  if(Runtime==NULL || Runtime->Revision!=PIANO_PRODUCT_RUNTIME_REVISION || !Runtime->Pump || !Runtime->BootServicesAlive ||
     !Runtime->RequestAction || !Runtime->GetPendingAction || !Runtime->AckAction)return EFI_COMPROMISED_DATA;
  if(mService.UiRuntime!=NULL && (Runtime!=mService.UiRuntime || !SameUiRuntime(Runtime,&mService.UiMethods)))return EFI_COMPROMISED_DATA;
  PIANO_PRODUCT_RUNTIME_PROTOCOL Methods=*Runtime;
  BOOLEAN Alive=Methods.BootServicesAlive(Runtime);
  if(mService.State.ServicesLost)return EFI_ABORTED;
  if(!Alive || !SameUiRuntime(Runtime,&Methods))return EFI_NOT_READY;
  if(mService.UiRuntime==NULL){mService.UiRuntime=Runtime;mService.UiMethods=Methods;}
  S=Methods.RequestAction(Runtime,Action); // CPU latch only; no nested dispatch.
  if(mService.State.ServicesLost)return EFI_ABORTED;
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  if(!SameUiRuntime(Runtime,&Methods))return EFI_COMPROMISED_DATA;
  Alive=Methods.BootServicesAlive(Runtime);
  if(mService.State.ServicesLost)return EFI_ABORTED;
  return Alive?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS ServiceError(EFI_STATUS S) {
  if(S==EFI_SUCCESS)S=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(S))S=EFI_DEVICE_ERROR;
  if(!mService.EverStarted)mService.StartStatus=S;
  mService.State.LastStatus=S;mService.State.Action=PianoUsbServiceActionFault;mService.State.Phase=PianoUsbServiceStopRequested;
  return S;
}
STATIC EFI_STATUS ServiceReset(BOOLEAN *CsftrstTimeout) {
  UINT32 Id=Dr(0xC120),Revision=Dr(0xC1A0),Type=Dr(0xC1A4),Ip=Id>>16;
  BOOLEAN Dwc31=Ip==0x3331,Dwc32=Ip==0x3332;
  // GSNPSID identifies the IP, not the DWC31 revision. Unknown DWC31 uses
  // both conservative bounded polling and the older synchronization delay.
  BOOLEAN Known31=FALSE;
  if(Dwc31)switch(Revision){
    case 0x3131302a:case 0x3132302a:case 0x3136302a:case 0x3137302a:case 0x3138302a:
    case 0x3139302a:case 0x3230302a:case 0x3231302a:case 0x3232302a:case 0x3233302a:case 0x3234302a:
      Known31=TRUE;break;
    default:break;
  }
  UINTN Step=(Dwc31||Dwc32)?20000:10,Limit=(Dwc31||Dwc32)?200000:100000;
  UINTN Sync=Dwc31&&(!Known31||Revision<=0x3138302a)?50000:0,Waited=0;
  *CsftrstTimeout=FALSE;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_RESET phase=begin gsnpsid=%08x ver_number=%08x ver_type=%08x limit_us=%lu sync_us=%lu dctl=%08x gusb2=%08x pipe=%08x\n",
    Id,Revision,Type,(UINT64)Limit,(UINT64)Sync,Dr(0xC704),Dr(0xC200),Dr(0xC2C0)));
  Dw(0xC704,(Dr(0xC704)&~(BIT31|(0xfU<<5)))|BIT30);
  for(;;) {
    if(mService.State.ServicesLost)return EFI_ABORTED;
    if(!(Dr(0xC704)&BIT30))break;
    if(Waited==Limit) {
      *CsftrstTimeout=TRUE;
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_RESET phase=timeout waited_us=%lu dctl=%08x dsts=%08x gusb2=%08x pipe=%08x\n",
        (UINT64)Waited,Dr(0xC704),Dr(0xC70C),Dr(0xC200),Dr(0xC2C0)));
      return EFI_TIMEOUT;
    }
    EFI_STATUS S=gBS->Stall(Step);if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
    Waited+=Step;
  }
  if(Sync){EFI_STATUS S=gBS->Stall(Sync);if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  if(mService.State.ServicesLost)return EFI_ABORTED;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_RESET phase=done waited_us=%lu sync_us=%lu dctl=%08x dsts=%08x gusb2=%08x pipe=%08x\n",
    (UINT64)Waited,(UINT64)Sync,Dr(0xC704),Dr(0xC70C),Dr(0xC200),Dr(0xC2C0)));
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ServiceStart(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device,CONST PIANO_DWC3_SERVICE_CONFIG *Config) {
  if(Context==NULL || Device==NULL || Config==NULL || Config->NowUs==NULL)return EFI_INVALID_PARAMETER;
  if(gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL)return EFI_UNSUPPORTED;
  if(!ServiceAtApp())return EFI_UNSUPPORTED;
  if(mExperimentRunning || mService.State.Retained || mService.State.ServicesLost)return EFI_ALREADY_STARTED;
  if(!Context->Attached || !Context->Verified || Context->Domain==NULL || !Context->TableMemory.Signature || Context->TableMemory.Quarantined ||
     Device->Context!=Context || Device->StreamId!=0x40)return EFI_NOT_READY;
#if PIANO_USB_RAM_BOOT
  if(mBootActionValid)return EFI_ALREADY_STARTED;
#endif
  ZeroMem(&mService,sizeof(mService));mService.Context=Context;mService.Device=Device;mService.Config=*Config;mService.State.Revision=1;
  mService.StartStatus=mService.CleanupStatus=EFI_NOT_READY;
  ZeroMem(&mDeviceRetire,sizeof(mDeviceRetire));
  mExperimentRunning=TRUE;mExperimentContractFailed=FALSE;mRebootAfterCleanup=FALSE;
  mService.OldGctl=Dr(0xC110);mService.OldDcfg=Dr(0xC700);mService.OldSize=Dr(0xC408);mService.OldLow=Dr(0xC400);mService.OldHigh=Dr(0xC404);
  mService.OldSessionHs=Dr(USB_SESSION_HS);mService.OldSessionSs=Dr(USB_SESSION_SS);
  mService.OldEventCount=Dr(0xC40C)&0xffff;mService.OldEventsEnabled=Dr(0xC708);mService.OldEndpointsEnabled=Dr(0xC720);
  mRingPosition=mPhase=mStatusEp=0;mConfigured=mThreeStage=FALSE;ZeroMem(&mControl,sizeof(mControl));
  ZeroMem(mPending,sizeof(mPending));ZeroMem(mEnding,sizeof(mEnding));ZeroMem(mPayload,sizeof(mPayload));ZeroMem(mPosted,sizeof(mPosted));
  mBulkLive=mBulkPrepared=mConfigWaiting=mStatusWaiting=FALSE;mBulkOutBytes=mBulkInBytes=0;
  ClearFastboot();EFI_STATUS S=PianoFastbootInit(&mFastboot,NULL,FastbootSend,NULL);
  if(S!=EFI_SUCCESS)return ServiceError(S);
  mFastboot.Query=FastbootQuery;mFastboot.Diagnostic=FastbootDiagnostic;
  if(mExperimentHasStorage){S=PianoFastbootSetStorage(&mFastboot,&mExperimentStorage);if(S!=EFI_SUCCESS)return ServiceError(S);}
#if PIANO_USB_RAM_BOOT
  mBootAckObserved=FALSE;
  if(mExperimentHasBoot){S=PianoFastbootSetBoot(&mFastboot,&mExperimentBoot);if(S!=EFI_SUCCESS)return ServiceError(S);}
#endif
  S=ServiceReset(&mService.CsftrstTimeoutPreDma);if(S!=EFI_SUCCESS)return ServiceError(S);
  CONST PIANO_DMA_DIRECTION Directions[]={PianoDmaFromDevice,PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice,
    PianoDmaBidirectional,PianoDmaBidirectional,PianoDmaFromDevice,PianoDmaToDevice};
  for(UINTN I=0;I<ARRAY_SIZE(mServiceBuffers);++I) {
    ZeroMem(mServiceBuffers[I],sizeof(*mServiceBuffers[I]));
    S=PianoDmaAllocate(Device,4096,4096,32,Directions[I],mServiceBuffers[I]);if(S!=EFI_SUCCESS){mService.Uncertain=!EFI_ERROR(S);return ServiceError(S);}
    ++mService.DmaBuffersAllocated;
    S=PianoDmaMap(mServiceBuffers[I]);if(S!=EFI_SUCCESS){mService.Uncertain=TRUE;return ServiceError(S);}
  }
  S=PianoDwc3CheckStorageForExperiment("service-dwc-mapped",TRUE);if(S!=EFI_SUCCESS)return ServiceError(S);
  mService.RegistersProgrammed=TRUE;
  Dw(0xC110,(mService.OldGctl&~(3U<<12))|(2U<<12)|BIT0);
  Dw(0xC700,(mService.OldDcfg&~(7U|(0x7FU<<3)|(31U<<17)))|4U|(16U<<17));
  Dw(0xC400,(UINT32)mRing.DeviceAddress);Dw(0xC404,(UINT32)(mRing.DeviceAddress>>32));Dw(0xC408,4096);
  UINT32 Count=Dr(0xC40C)&0xffff;if(Count)Dw(0xC40C,Count);
  S=PianoDmaBegin(&mRing,"USB_SERVICE_EVENT_RING");if(S!=EFI_SUCCESS)return ServiceError(S);
  S=Command(0,9,0,0,0);if(S==EFI_SUCCESS)S=ConfigureEp(0,FALSE);if(S==EFI_SUCCESS)S=ConfigureEp(1,FALSE);
  if(S==EFI_SUCCESS)S=Command(2,2,1,0,0);if(S==EFI_SUCCESS)S=Command(3,2,1,0,0);if(S!=EFI_SUCCESS)return ServiceError(S);
  Dw(0xC720,3);Dw(0xC708,BIT0|BIT1|BIT2|BIT3|BIT9);
  Dw(USB_SESSION_HS,mService.OldSessionHs|USB_SESSION_HS_VALID);Dw(USB_SESSION_SS,mService.OldSessionSs|USB_SESSION_SS_PRESENT);mService.SessionSet=TRUE;
  if(Dr(USB_SESSION_HS)!=(mService.OldSessionHs|USB_SESSION_HS_VALID) || Dr(USB_SESSION_SS)!=(mService.OldSessionSs|USB_SESSION_SS_PRESENT))return ServiceError(EFI_DEVICE_ERROR);
  S=PianoDwc3CheckStorageForExperiment("service-before-run",TRUE);if(S!=EFI_SUCCESS)return ServiceError(S);
  Dw(0xC704,(Dr(0xC704)&~(BIT9|BIT10|BIT11|BIT12))|BIT31);gBS->Stall(1000);
  S=ArmSetup();if(S!=EFI_SUCCESS)return ServiceError(S);
  mService.EverStarted=TRUE;mService.StartStatus=EFI_SUCCESS;
  mService.State.Started=TRUE;mService.State.Phase=PianoUsbServiceListening;mService.State.LastStatus=EFI_SUCCESS;
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ServicePollBounded(UINTN MaxEvents) {
  if(!MaxEvents || MaxEvents>SERVICE_EVENTS)return EFI_INVALID_PARAMETER;
  if(!mService.State.Started || mService.State.ServicesLost || mService.State.Phase!=PianoUsbServiceListening)return EFI_NOT_READY;
  UINT32 Count=Dr(0xC40C)&0xffff;if(!Count)return EFI_NOT_READY;
  if(Count>4096 || (Count&3)){mService.Uncertain=TRUE;return ServiceError(EFI_COMPROMISED_DATA);}
  EFI_STATUS S=PianoDmaSyncForCpuQuiet(&mRing);if(S!=EFI_SUCCESS){mService.Uncertain=TRUE;return ServiceError(S);}
  UINTN Available=SERVICE_EVENTS-mService.Count,N=MIN((UINTN)(Count/4),MIN(MaxEvents,Available));
  if(!N){mService.Uncertain=TRUE;return ServiceError(EFI_OUT_OF_RESOURCES);}
  for(UINTN I=0;I<N;++I) {
    mService.Events[(mService.Head+mService.Count)%SERVICE_EVENTS]=((UINT32 *)mRing.Cpu)[mRingPosition/4];
    ++mService.Count;mRingPosition=(mRingPosition+4)%4096;
  }
  Dw(0xC40C,(UINT32)(4*N));mService.State.WorkPending=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ServicePumpApp(UINT32 Reason,UINTN BudgetUs) {
  if(!Reason || (Reason&~7U) || !BudgetUs || BudgetUs>100000)return EFI_INVALID_PARAMETER;
  if(!mService.State.Started || mService.State.ServicesLost)return EFI_NOT_READY;
  if(!ServiceAtApp())return EFI_UNSUPPORTED;
  if(mService.State.Busy)return EFI_ALREADY_STARTED;
  if(mService.State.Phase!=PianoUsbServiceListening)return mService.State.LastStatus;
  mService.State.Busy=TRUE;UINT64 Start=mService.Config.NowUs(mService.Config.Context);EFI_STATUS S=EFI_SUCCESS;
  for(UINTN I=0;I<SERVICE_EVENTS;++I) {
    // Only queue bookkeeping is masked; restore APP before any Command/BS or
    // storage/GOP/allocator work. A timer producer cannot race Count's RMW.
    EFI_TPL Old=gBS->RaiseTPL(TPL_CALLBACK);
    EFI_STATUS Poll=PianoDwc3ServicePollBounded(8);
    if(Poll!=EFI_SUCCESS && Poll!=EFI_NOT_READY){gBS->RestoreTPL(Old);S=Poll;break;}
    if(!mService.Count){gBS->RestoreTPL(Old);break;}
    UINT32 E=mService.Events[mService.Head];mService.Head=(mService.Head+1)%SERVICE_EVENTS;--mService.Count;
    gBS->RestoreTPL(Old);
    S=Event(E);if(S!=EFI_SUCCESS){S=ServiceError(S);break;}
    if(mExperimentContractFailed){S=ServiceError(EFI_DEVICE_ERROR);break;}
    if(!mFrames && !mPending[3] && !mEnding[3]) {
      if(mFastboot.RebootRequested || mFastboot.ExitRequested) {
        mService.State.Action=mFastboot.RebootRequested?PianoUsbServiceActionReboot:PianoUsbServiceActionContinue;
        mService.State.Phase=PianoUsbServiceStopRequested;break;
      }
#if PIANO_USB_RAM_BOOT
      if(mFastboot.BootPending && mBootAckObserved){mFastboot.BootTransferFrozen=TRUE;mService.State.Action=PianoUsbServiceActionBoot;mService.State.Phase=PianoUsbServiceStopRequested;break;}
#endif
    }
    UINT64 Now=mService.Config.NowUs(mService.Config.Context);if(Now<Start){S=ServiceError(EFI_COMPROMISED_DATA);break;}
    if(Now-Start>=BudgetUs)break;
  }
  mService.State.Configured=mConfigured;mService.State.QueuedEvents=(UINT32)mService.Count;
  mService.State.WorkPending=mService.Count!=0;
  mService.State.BulkActive=mService.State.Phase==PianoUsbServiceListening && mBulkLive &&
    (mFrames!=NULL || mPending[3] || mFastboot.Receiving);
  mService.State.OutBytes=mBulkOutBytes;mService.State.InBytes=mBulkInBytes;
  mService.State.Busy=FALSE;return S;
}
EFI_STATUS PianoDwc3ServiceStop(EFI_STATUS Reason) {
  if(mService.State.ServicesLost)return EFI_ACCESS_DENIED;
  if(!ServiceAtApp())return EFI_UNSUPPORTED;
  if(!mExperimentRunning)return mService.State.Retained?EFI_ACCESS_DENIED:EFI_NOT_READY;
  if(mService.State.Busy)return EFI_ALREADY_STARTED;
  mService.CleanupCompleted=FALSE;mService.CleanupStatus=EFI_NOT_READY;
  mService.ResetOnlyRetired=FALSE;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=begin start_status=%r reason=%r allocated=%u programmed=%u uncertain=%u dctl=%08x dsts=%08x\n",
    mService.StartStatus,Reason,mService.DmaBuffersAllocated,mService.RegistersProgrammed,mService.Uncertain,Dr(0xC704),Dr(0xC70C)));
  mService.State.Busy=TRUE;mService.State.BulkActive=FALSE;EFI_STATUS S=Halt(),Cleanup=EFI_SUCCESS;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=halt status=%r dctl=%08x dsts=%08x\n",S,Dr(0xC704),Dr(0xC70C)));
  if(S!=EFI_SUCCESS){mService.CleanupStatus=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;mService.State.Retained=TRUE;mService.State.Phase=PianoUsbServiceRetained;mFastboot.BootTransferFrozen=TRUE;mService.State.Busy=FALSE;mService.State.LastStatus=mService.CleanupStatus;return mService.State.LastStatus;}
  mService.State.DeviceHalted=TRUE;mService.State.Started=FALSE;
  EFI_STATUS Status=mService.State.LastStatus!=EFI_SUCCESS?mService.State.LastStatus:Reason;
  if(Status!=EFI_SUCCESS && !EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  if(mService.SessionSet) {
    Dw(USB_SESSION_SS,mService.OldSessionSs);Dw(USB_SESSION_HS,mService.OldSessionHs);
    BOOLEAN Ss=ServiceRestoreReadback(USB_SESSION_SS,mService.OldSessionSs),Hs=ServiceRestoreReadback(USB_SESSION_HS,mService.OldSessionHs);
    if(!Ss||!Hs){Status=Cleanup=EFI_DEVICE_ERROR;mService.Uncertain=TRUE;}
  }
  UINTN Retained=0,Freed=0;
  for(UINTN I=0;I<ARRAY_SIZE(mServiceBuffers);++I) {
    PIANO_DMA_BUFFER *B=mServiceBuffers[I];
    if(B->Active){S=PianoDmaComplete(B,EFI_SUCCESS,TRUE);if(S!=EFI_SUCCESS){B->Quarantined=TRUE;mService.Uncertain=TRUE;Status=Cleanup=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}}
  }
  if(mService.CsftrstTimeoutPreDma) {
    // CSFTRST failed before we allocated a ring or changed its registers.
    // Do not restore unused inherited addresses or require IMASK to latch
    // while the device reset remains asserted. Prove actual inactivity instead.
    UINT32 Gctl=Dr(0xC110),Dctl=Dr(0xC704),Dsts=Dr(0xC70C),Size=Dr(0xC408),Count=Dr(0xC40C)&0xffff,Events=Dr(0xC708),Endpoints=Dr(0xC720);
    BOOLEAN Empty=ServiceBuffersEmpty();
    BOOLEAN Inactive=mService.StartStatus==EFI_TIMEOUT && !mService.RegistersProgrammed && !mService.EverStarted &&
      !mService.DmaBuffersAllocated && !mService.SessionSet && !mService.Uncertain && !mExperimentContractFailed && Empty &&
      !(mService.OldSize&0xffff) && !mService.OldEventCount && !mService.OldEventsEnabled && !mService.OldEndpointsEnabled &&
      ((Gctl>>12)&3)==2 && !(Dctl&BIT31) && (Dsts&BIT22) && !(Size&0xffff) && !Count && !Events && !Endpoints;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=reset-only inactive=%u empty=%u gctl=%08x dcfg=%08x dctl=%08x dsts=%08x event_size=%08x count=%08x events=%08x endpoints=%08x old_size=%08x old_count=%08x old_events=%08x old_endpoints=%08x\n",
      Inactive,Empty,Gctl,Dr(0xC700),Dctl,Dsts,Size,Count,Events,Endpoints,mService.OldSize,mService.OldEventCount,mService.OldEventsEnabled,mService.OldEndpointsEnabled));
    if(Inactive)mService.ResetOnlyRetired=TRUE;
    else{Status=Cleanup=EFI_DEVICE_ERROR;mService.Uncertain=TRUE;}
  } else {
    Dw(0xC708,0);Dw(0xC408,BIT31);UINT32 Count=Dr(0xC40C)&0xffff;if(Count)Dw(0xC40C,Count);
    Dw(0xC400,mService.OldLow);Dw(0xC404,mService.OldHigh);Dw(0xC408,mService.OldSize|BIT31);Dw(0xC700,mService.OldDcfg);Dw(0xC110,mService.OldGctl);
    CONST UINT32 Offsets[]={0xC708,0xC400,0xC404,0xC408,0xC700,0xC110};
    CONST UINT32 Expected[]={0,mService.OldLow,mService.OldHigh,mService.OldSize|BIT31,mService.OldDcfg,mService.OldGctl};
    for(UINTN I=0;I<ARRAY_SIZE(Offsets);++I)if(!ServiceRestoreReadback(Offsets[I],Expected[I])){
      Status=Cleanup=EFI_DEVICE_ERROR;mService.Uncertain=TRUE;}
  }
  for(UINTN I=0;I<ARRAY_SIZE(mServiceBuffers);++I) {
    PIANO_DMA_BUFFER *B=mServiceBuffers[I];if((mService.Uncertain || mExperimentContractFailed) && B->Signature)B->Quarantined=TRUE;
    if(B->Signature){S=PianoDmaFree(B);
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=dma-free index=%u status=%r signature=%08x active=%u mapped=%u quarantined=%u\n",
        (UINT32)I,S,B->Signature,B->Active,B->Mapped,B->Quarantined));
      if(S==EFI_SUCCESS && !B->Signature)++Freed;else {mService.Uncertain=TRUE;Status=Cleanup=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}}
    if(B->Signature)++Retained;
  }
  mService.State.DmaBuffersFreed=(UINT32)Freed;mService.State.DmaFreed=Retained==0 && !mService.Uncertain;
#if PIANO_USB_RAM_BOOT
  if(mService.State.Action==PianoUsbServiceActionBoot || (mFastboot.BootPending && mFastboot.BootTransferFrozen)) {
    mFastboot.BootProof=(PIANO_FB_BOOT_PROOF){mBootAckObserved,mFrames==NULL&&!mPending[3]&&!mEnding[3],TRUE,mService.State.DmaFreed,TRUE,mBootAckObserved?4:0,(UINT32)Freed};
    mBootAction=(PIANO_FB_BOOT_ACTION){.Context=mFastboot.Boot.Context,.View=mFastboot.BootView,.Proof=mFastboot.BootProof,.Status=Status,.Retained=TRUE};mBootActionValid=TRUE;
    if(Status==EFI_SUCCESS && mBootAckObserved && mService.State.DmaFreed && Freed==9 && mFastboot.Download==mFastboot.BootValidatedDownload && mFastboot.Received==mFastboot.BootValidatedBytes) {
      S=mFastboot.Boot.Validate(mFastboot.Boot.Context,&mFastboot,&mFastboot.BootView);
      if(S==EFI_SUCCESS && mFastboot.Download==mFastboot.BootValidatedDownload && mFastboot.Received==mFastboot.BootValidatedBytes)
        S=mFastboot.Boot.TakeAfterAck(mFastboot.Boot.Context,&mFastboot,&mFastboot.BootView,&mBootAction.Token);
      else if(S==EFI_SUCCESS)S=EFI_COMPROMISED_DATA;
      if(S==EFI_SUCCESS && mBootAction.Token && !mFastboot.Download && !mFastboot.Upload && !mFastboot.UploadBytes && !mFastboot.UploadBorrowed &&
         !mFastboot.Expected && !mFastboot.Received && !mFastboot.Receiving && !mFastboot.Complete){mBootAction.Taken=TRUE;mBootAction.Retained=FALSE;mFastboot.BootTransferFrozen=FALSE;}
      else {Status=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;mBootAction.Status=Status;}
    } else {if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;mBootAction.Status=Status;}
  }
#endif
  if(Retained || mService.Uncertain){mFastboot.BootTransferFrozen=TRUE;if(Status==EFI_SUCCESS)Status=EFI_DEVICE_ERROR;}
  ClearFastboot();mService.Count=mService.Head=0;
  if(mLogSnapshot!=NULL){
    ZeroMem(mLogSnapshot,USB_DIAG_LOG_BYTES);S=gBS->FreePool?gBS->FreePool(mLogSnapshot):EFI_UNSUPPORTED;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=log-free status=%r\n",S));
    if(S==EFI_SUCCESS)mLogSnapshot=NULL;
    else{Status=Cleanup=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;mService.Uncertain=TRUE;}
  }
  mLogValid=FALSE;
  mService.State.Retained=Retained!=0 || mService.Uncertain || mFastboot.BootTransferFrozen;
  mService.State.Phase=mService.State.Retained?PianoUsbServiceRetained:PianoUsbServiceOff;
  mService.State.LastStatus=Status;mService.State.Busy=FALSE;mService.State.WorkPending=FALSE;mService.State.QueuedEvents=0;
  mDeviceRetire=(PIANO_SMMU_USB_RETIRE_EVIDENCE){.Revision=1,.DeviceCleanupStatus=Status,.DeviceHalted=mService.State.DeviceHalted,
    .DmaFreed=mService.State.DmaFreed,.DmaBuffersFreed=(UINT32)Freed};
  if(!mService.State.Retained)mExperimentRunning=FALSE;
  mService.CleanupStatus=Cleanup;mService.CleanupCompleted=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SERVICE_CLEANUP stage=done start_status=%r cleanup_status=%r allocated=%u freed=%u retained=%u uncertain=%u lost=%u reset_only=%u\n",
    mService.StartStatus,Cleanup,mService.DmaBuffersAllocated,(UINT32)Freed,mService.State.Retained,mService.Uncertain,mService.State.ServicesLost,mService.ResetOnlyRetired));
  return Status;
}
EFI_STATUS PianoDwc3GetStartupFailureEvidence(PIANO_USB_STARTUP_FAILURE_EVIDENCE *Out) {
  if(Out==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Out,sizeof(*Out));Out->Revision=1;Out->Context=mService.Context;
  Out->StartStatus=mService.StartStatus;Out->CleanupStatus=mService.CleanupStatus;
  Out->CsftrstTimeoutPreDma=mService.CsftrstTimeoutPreDma;Out->ServiceNeverStarted=!mService.EverStarted;
  Out->DeviceHalted=mService.State.DeviceHalted;Out->DmaFreed=mService.State.DmaFreed;
  Out->Retained=mService.State.Retained||mService.Uncertain;Out->ServicesLost=mService.State.ServicesLost;
  Out->DmaBuffersAllocated=mService.DmaBuffersAllocated;Out->DmaBuffersFreed=mService.State.DmaBuffersFreed;
  if(!Out->Context || !Out->CsftrstTimeoutPreDma || !mService.ResetOnlyRetired || Out->StartStatus!=EFI_TIMEOUT || !mService.CleanupCompleted ||
     Out->CleanupStatus!=EFI_SUCCESS || !Out->ServiceNeverStarted || !Out->DeviceHalted || !Out->DmaFreed ||
     Out->Retained || Out->ServicesLost || Out->DmaBuffersAllocated || Out->DmaBuffersFreed ||
     mService.State.Busy || mService.State.Started || mService.State.Phase!=PianoUsbServiceOff || mExperimentRunning || mLogSnapshot)return EFI_NOT_READY;
  if(!ServiceBuffersEmpty())return EFI_NOT_READY;
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status) {
  if(Status==NULL)return EFI_INVALID_PARAMETER;
  *Status=mService.State;Status->QueuedEvents=(UINT32)mService.Count;Status->WorkPending=mService.Count!=0;
  return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ServiceFenceExit(VOID) {
  mService.State.ServicesLost=TRUE;mService.State.BulkActive=FALSE;if(!mService.State.Started){mService.State.Phase=PianoUsbServiceExited;return EFI_SUCCESS;}
  Dw(0xC704,Dr(0xC704)&~BIT31);BOOLEAN Halted=FALSE;
  for(UINTN I=0;I<1000000;++I)if((Dr(0xC70C)&BIT22) && !(Dr(0xC704)&BIT31)){Halted=TRUE;break;}
  mService.State.DeviceHalted=Halted;mService.State.Retained=TRUE;mService.State.Started=FALSE;mService.State.Phase=PianoUsbServiceExited;
  mFastboot.BootTransferFrozen=TRUE;for(UINTN I=0;I<ARRAY_SIZE(mServiceBuffers);++I)if(mServiceBuffers[I]->Signature)mServiceBuffers[I]->ExitRetained=TRUE;
  return Halted?EFI_SUCCESS:EFI_TIMEOUT;
}
#else
EFI_STATUS PianoDwc3ServiceStart(PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D,CONST PIANO_DWC3_SERVICE_CONFIG *Config){(VOID)C;(VOID)D;(VOID)Config;return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3ServicePollBounded(UINTN N){(VOID)N;return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3ServicePumpApp(UINT32 Reason,UINTN BudgetUs){(VOID)Reason;(VOID)BudgetUs;return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3ServiceStop(EFI_STATUS Reason){(VOID)Reason;return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3ServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *S){if(S==NULL)return EFI_INVALID_PARAMETER;ZeroMem(S,sizeof(*S));S->Revision=1;return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3ServiceFenceExit(VOID){return EFI_UNSUPPORTED;}
EFI_STATUS PianoDwc3GetStartupFailureEvidence(PIANO_USB_STARTUP_FAILURE_EVIDENCE *E){if(E==NULL)return EFI_INVALID_PARAMETER;ZeroMem(E,sizeof(*E));E->Revision=1;return EFI_UNSUPPORTED;}
#endif
