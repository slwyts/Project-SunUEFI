// SPDX-License-Identifier: GPL-2.0-or-later
// Y700 NT36536 SPI touchscreen. GPIO SPI keeps this input path independent
// of GENI GPI/DMA and its secure firmware configuration. Only the four
// board-verified SPI pins are driven; all mux/output state is restored.
// Register/protocol sources are recorded in docs/devel/y700-touch.md.
#include "Y700Touch.h"
#include "Y700TouchConfig.h"
#include "Y700TouchDecode.h"
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EFIClock.h>
#include <Guid/ConsoleInDevice.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
#include <Library/FdtLib.h>

#define PIN_BASE 0x0F100000U
#define PIN_CTL(P) (PIN_BASE+(P)*0x1000U)
#define PIN_IO(P) (PIN_CTL(P)+4)
#define PIN_MISO 48U
#define PIN_MOSI 49U
#define PIN_CLK 50U
#define PIN_CS 51U
#define PIN_IRQ 162U
#define IRQ_CFG (PIN_CTL(PIN_IRQ)+8)
#define IRQ_STATUS (PIN_CTL(PIN_IRQ)+12)
#define TOUCH_SE 0x00A90000U
#define QUEUE 32U
STATIC UINT32 mCtl[4],mIo[4];
STATIC BOOLEAN mPins,mRunning,mRetained,mExited,mBusy;
STATIC EFI_STATUS mStatus=EFI_NOT_STARTED;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL mPointer;
STATIC EFI_ABSOLUTE_POINTER_MODE mMode={0,0,0,65535,65535,0,0};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mTimer,mExit;
STATIC EFI_CLOCK_PROTOCOL *mClock;
STATIC UINTN mClockId;
STATIC BOOLEAN mClockHeld;
STATIC BOOLEAN mIrqOwned;
STATIC UINT32 mIrqSavedCtl,mIrqSavedCfg,mIrqInitialStatus,mIrqEdges,mIrqIdle;
STATIC EFI_ABSOLUTE_POINTER_STATE mQueue[QUEUE],mLast;
STATIC UINTN mHead,mTail;
STATIC UINT32 mFrames,mBad,mEvents,mTransfers,mMaxX,mMaxY,mStep,mConsecutiveBad;
STATIC UINT32 mGets,mTaken,mLastReadUs,mMaxReadUs,mBadSum,mIrqBefore,mIrqAfter;
STATIC UINT8 mLastBad[Y700_TOUCH_FRAME_BYTES];
STATIC UINT8 mLastGood[Y700_TOUCH_FRAME_BYTES],mResetState[6],mHistory[256],mRecoveryInfo[38],mHostCommand[2];
STATIC UINT32 mWatchdogStreak,mWatchdogs,mHandshakes,mRecoveries,mGetTpl;
STATIC BOOLEAN mRecoveryPending;
STATIC UINT8 mWatchdogFirst;
STATIC EFI_STATUS mRecoveryStatus=EFI_NOT_STARTED;
STATIC EFI_STATUS mNormalStatus=EFI_NOT_STARTED;
STATIC UINT8 mNormalAck=0xff,mNormalReset[6];
STATIC BOOLEAN mHeadersVerified,mFirmwareCrcVerified;
STATIC UINT8 mPrimary,mVersion;
STATIC struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} mPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0x638c662c,0xcac9,0x46d4,{0xa6,0xa1,0x59,0x37,0x30,0x30,0x54,0x50}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}};

STATIC VOID Drive(UINT32 Pin,BOOLEAN High) {MmioWrite32(PIN_IO(Pin),High?2:0);}
STATIC UINT32 GpioConfig(UINTN Index,BOOLEAN Output,UINT32 Pull) {
  UINT32 Value=(mCtl[Index]&~0x3ffU)|(2U<<6)|Pull|(Output?BIT9:0);
  // Match pinctrl-msm's ownership switch for non-egpio functions on sun.
  if(Value&BIT11)Value|=BIT12;
  return Value;
}
STATIC UINT8 Byte(UINT8 Tx) {
  UINT8 Rx=0;
  for(UINTN I=0;I<8;++I) {
    Drive(PIN_MOSI,(Tx&0x80)!=0);Tx<<=1;
    NanoSecondDelay(100);Drive(PIN_CLK,TRUE);
    Rx=(UINT8)((Rx<<1)|(MmioRead32(PIN_IO(PIN_MISO))&1));
    NanoSecondDelay(100);Drive(PIN_CLK,FALSE);
  }
  return Rx;
}
STATIC VOID Select(BOOLEAN Active) {
  Drive(PIN_CS,!Active);MicroSecondDelay(2);
}
STATIC VOID Page(UINT32 Address) {
  Select(TRUE);Byte(0xff);Byte((UINT8)(Address>>15));Byte((UINT8)(Address>>7));Select(FALSE);
  ++mTransfers;
}
STATIC VOID Read(UINT32 Address,UINT8 *Data,UINTN Bytes) {
  Page(Address);Select(TRUE);Byte((UINT8)(Address&0x7f));Byte(0);
  for(UINTN I=0;I<Bytes;++I)Data[I]=Byte(0);
  Select(FALSE);++mTransfers;
}
STATIC VOID Write(UINT32 Address,CONST UINT8 *Data,UINTN Bytes) {
  Page(Address);Select(TRUE);Byte((UINT8)(Address&0x7f)|0x80);
  for(UINTN I=0;I<Bytes;++I)Byte(Data[I]);
  Select(FALSE);++mTransfers;
}
STATIC VOID WriteByte(UINT32 Address,UINT8 Value){Write(Address,&Value,1);}

STATIC EFI_STATUS AcquireIrqLatch(VOID) {
  mIrqSavedCtl=MmioRead32(PIN_CTL(PIN_IRQ));
  mIrqSavedCfg=MmioRead32(IRQ_CFG);mIrqInitialStatus=MmioRead32(IRQ_STATUS);
  if(mIrqSavedCtl==MAX_UINT32||mIrqSavedCfg==MAX_UINT32)return EFI_UNSUPPORTED;
  mIrqOwned=TRUE;
  // Disable both TLMM summary and the existing PDC wake output. Raw status
  // latches rising edges without enabling either CPU interrupt path.
  MmioWrite32(IRQ_CFG,mIrqSavedCfg&~(BIT0|BIT7));
  UINT32 Ctl=mIrqSavedCtl&~(0x3cU|BIT9);
  if(Ctl&BIT11)Ctl|=BIT12;
  MmioWrite32(PIN_CTL(PIN_IRQ),Ctl);
  UINT32 Cfg=(mIrqSavedCfg&~0x8fU)|BIT4|BIT2|BIT1;
  MmioWrite32(IRQ_CFG,Cfg);
  // sun has intr_ack_high=false: status is cleared by writing ZERO, not W1C.
  // Enabling raw capture / changing detection can produce one false edge.
  MmioWrite32(IRQ_STATUS,0);
  if(MmioRead32(IRQ_CFG)!=Cfg||MmioRead32(PIN_CTL(PIN_IRQ))!=Ctl)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS RestoreIrqLatch(VOID) {
  if(!mIrqOwned)return EFI_SUCCESS;
  MmioAnd32(IRQ_CFG,~(BIT0|BIT7));MmioWrite32(IRQ_STATUS,0);
  MmioWrite32(PIN_CTL(PIN_IRQ),mIrqSavedCtl);MmioWrite32(IRQ_CFG,mIrqSavedCfg);
  if(MmioRead32(PIN_CTL(PIN_IRQ))!=mIrqSavedCtl||MmioRead32(IRQ_CFG)!=mIrqSavedCfg)return EFI_DEVICE_ERROR;
  mIrqOwned=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS RestorePins(VOID) {
  if(!mPins)return EFI_SUCCESS;
  Drive(PIN_CS,TRUE);Drive(PIN_CLK,FALSE);
  // Disable outputs before restoring their latches and original muxes.
  for(UINTN I=0;I<4;++I)MmioAnd32(PIN_CTL(48+I),~BIT9);
  for(UINTN I=0;I<4;++I)MmioWrite32(PIN_IO(48+I),mIo[I]&BIT1);
  for(UINTN I=0;I<4;++I)MmioWrite32(PIN_CTL(48+I),mCtl[I]);
  for(UINTN I=0;I<4;++I)
    if(MmioRead32(PIN_CTL(48+I))!=mCtl[I] ||
       (MmioRead32(PIN_IO(48+I))&BIT1)!=(mIo[I]&BIT1))return EFI_DEVICE_ERROR;
  mPins=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS AcquirePins(VOID) {
  EFI_GUID ClockGuid=EFI_CLOCK_PROTOCOL_GUID;
  EFI_STATUS E=gBS->LocateProtocol(&ClockGuid,NULL,(VOID **)&mClock);
  if(E!=EFI_SUCCESS||!mClock)return EFI_NOT_FOUND;
  E=mClock->GetClockID(mClock,"gcc_qupv3_wrap1_s4_clk",&mClockId);
  if(E==EFI_SUCCESS)E=mClock->EnableClock(mClock,mClockId);
  if(E!=EFI_SUCCESS)return E;
  mClockHeld=TRUE;
  UINT32 Active=MmioRead32(TOUCH_SE+0x40);
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_ENGINE status=%x gpio_spi=1 dma_allocated=0\n",Active));
  if(Active&(BIT0|BIT12))return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<4;++I) {
    mCtl[I]=MmioRead32(PIN_CTL(48+I));mIo[I]=MmioRead32(PIN_IO(48+I));
    DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_PIN pin=%u ctl=%x io=%x\n",(UINT32)(48+I),mCtl[I],mIo[I]));
    if(mCtl[I]==MAX_UINT32)return EFI_UNSUPPORTED;
  }
  mPins=TRUE;
  // Input first, then preset mode-0 idle levels before enabling output.
  for(UINTN I=0;I<4;++I)MmioWrite32(PIN_CTL(48+I),GpioConfig(I,FALSE,1));
  Drive(PIN_CS,TRUE);Drive(PIN_CLK,FALSE);Drive(PIN_MOSI,FALSE);
  for(UINTN I=1;I<4;++I)MmioWrite32(PIN_CTL(48+I),GpioConfig(I,TRUE,I==3?3:1));
  MicroSecondDelay(100);
  for(UINTN I=0;I<4;++I)
    if((MmioRead32(PIN_CTL(48+I))&0x3c)!=0)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

// Firmware bootstrap and frame decoder are supplied below from the matching
// NT36XXX_SPI source, rather than the unrelated piano/nabu memory maps.
STATIC EFI_STATUS InitializeController(VOID);
STATIC EFI_STATUS DecodeFrame(CONST UINT8 *Data,UINTN Bytes,UINT32 *X,UINT32 *Y,BOOLEAN *Pressed) {
  int Down=0;
  if(Y700TouchDecode(Data,Bytes,mMaxX,mMaxY,&mPrimary,X,Y,&Down)!=0)return EFI_CRC_ERROR;
  *Pressed=Down!=0;return EFI_SUCCESS;
}
#include "Y700TouchFirmware.inc"
STATIC EFI_STATUS NormalReporting(VOID) {
  // Android's captured default is high_report_rate=0. Follow the original
  // ext_cmd_store handshake, rather than relying on an inherited setting.
  UINT8 Data[2]={0x76,0},Ack=0xff;
  for(UINTN I=0;I<5;++I) {
    Write(Y700_TOUCH_EVENT+0x50,Data,sizeof(Data));MicroSecondDelay(20000);
    Read(Y700_TOUCH_EVENT+0x50,&Ack,1);
    mNormalAck=Ack;
    if(Ack==0){Read(Y700_TOUCH_EVENT+0x60,mNormalReset,sizeof(mNormalReset));return mNormalStatus=EFI_SUCCESS;}
  }
  Read(Y700_TOUCH_EVENT+0x60,mNormalReset,sizeof(mNormalReset));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_NORMAL_REPORT status=%r ack=%02x\n",EFI_DEVICE_ERROR,Ack));
  return mNormalStatus=EFI_DEVICE_ERROR;
}
STATIC VOID RecoverySnapshot(VOID) {
  Read(Y700_TOUCH_EVENT+0x60,mResetState,sizeof(mResetState));
  Read(Y700_TOUCH_EVENT+0x50,mHostCommand,sizeof(mHostCommand));
  Read(Y700_TOUCH_EVENT+0x78,mRecoveryInfo,sizeof(mRecoveryInfo));
  STATIC CONST UINT32 Addresses[]={0x124d7c,0x124dbc,0x124dfc,0x124e3c};
  for(UINTN I=0;I<4;++I)Read(Addresses[I],mHistory+64*I,64);
}

STATIC VOID Push(UINT32 X,UINT32 Y,BOOLEAN Pressed) {
  EFI_ABSOLUTE_POINTER_STATE P={0};
  BOOLEAN Landscape=mGop&&mGop->Mode&&mGop->Mode->Info&&
    mGop->Mode->Info->HorizontalResolution>mGop->Mode->Info->VerticalResolution;
  // Inverse of PianoGopDxe's raw X=W-1-Y / raw Y=X mapping.
  P.CurrentX=Landscape?(UINT64)Y*65535/mMaxY:(UINT64)X*65535/mMaxX;
  P.CurrentY=Landscape?(UINT64)(mMaxX-X)*65535/mMaxX:(UINT64)Y*65535/mMaxY;
  P.ActiveButtons=Pressed?EFI_ABSP_TouchActive:0;
  if(CompareMem(&P,&mLast,sizeof(P))==0)return;
  UINTN Next=(mTail+1)%QUEUE;
  // Never overwrite a queued press/release. Leave mLast unchanged so the
  // next good sample retries this transition after the consumer catches up.
  if(Next==mHead)return;
  mQueue[mTail]=P;mTail=Next;
  if(P.ActiveButtons!=mLast.ActiveButtons)DEBUG((DEBUG_WARN,
    "SUNUEFI_Y700_TOUCH_EVENT pressed=%u raw=%u,%u logical=%lu,%lu\n",Pressed,X,Y,P.CurrentX,P.CurrentY));
  mLast=P;++mEvents;gBS->SignalEvent(mPointer.WaitForInput);
}
STATIC VOID Release(VOID) {
  if(!mLast.ActiveButtons)return;
  UINTN Next=(mTail+1)%QUEUE;if(Next==mHead)return;
  EFI_ABSOLUTE_POINTER_STATE P=mLast;P.ActiveButtons=0;
  mQueue[mTail]=P;mTail=Next;mLast=P;++mEvents;
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_EVENT pressed=0 logical=%lu,%lu\n",P.CurrentX,P.CurrentY));
  gBS->SignalEvent(mPointer.WaitForInput);
}
STATIC VOID EFIAPI Poll(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(!mRunning||mExited||mBusy)return;
  // Original nvt_ts_work_func reads only after a rising IRQ. Poll the hardware
  // edge latch rather than GPIO level, which can miss short pulses. Reading
  // event SRAM continuously while the controller is idle is not that flow.
  if(!(MmioRead32(IRQ_STATUS)&BIT0)){++mIrqIdle;return;}
  MmioWrite32(IRQ_STATUS,0);++mIrqEdges;
  mBusy=TRUE;UINT8 Data[Y700_TOUCH_FRAME_BYTES];
  mIrqBefore=MmioRead32(PIN_IO(162))&1;
  UINT64 Begin=GetPerformanceCounter();
  Read(Y700_TOUCH_EVENT,Data,sizeof(Data));
  mLastReadUs=(UINT32)(GetTimeInNanoSecond(GetPerformanceCounter()-Begin)/1000);
  if(mLastReadUs>mMaxReadUs)mMaxReadUs=mLastReadUs;
  mIrqAfter=MmioRead32(PIN_IO(162))&1;
  UINT32 X=0,Y=0;BOOLEAN Pressed=FALSE;
  EFI_STATUS E=DecodeFrame(Data,sizeof(Data),&X,&Y,&Pressed);
  BOOLEAN Watchdog=TRUE,Handshake=TRUE;
  for(UINTN I=0;I<6;++I) {
    if(Data[I]!=0xfd&&Data[I]!=0xfe)Watchdog=FALSE;
    if(Data[I]!=0x77)Handshake=FALSE;
  }
  if(Watchdog) {
    ++mWatchdogs;++mWatchdogStreak;mWatchdogFirst=Data[0];
    if(mWatchdogStreak>10)mRecoveryPending=TRUE;
  }else mWatchdogStreak=0;
  if(Handshake){++mHandshakes;E=EFI_NOT_READY;}
  if(E==EFI_SUCCESS) {++mFrames;mConsecutiveBad=0;CopyMem(mLastGood,Data,sizeof(Data));
    // Releases preserve the last logical point; do not rotate it twice.
    if(!Pressed)Release();else Push(X,Y,TRUE);
  }else {
    ++mBad;++mConsecutiveBad;
    CopyMem(mLastBad,Data,sizeof(Data));mBadSum=0;
    for(UINTN I=0;I<65;++I)mBadSum=(mBadSum+Data[I])&0xff;
    if(mBad<=3)DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_FRAME status=%r first=%02x%02x%02x%02x bad=%u\n",E,Data[0],Data[1],Data[2],Data[3],mBad));
    // A single damaged packet does not lift a finger; a persistently failed
    // controller must not leave the menu in a permanently pressed state.
    if(mConsecutiveBad>=3){Release();mPrimary=0;}
  }
  mBusy=FALSE;
}
STATIC VOID EFIAPI Wait(EFI_EVENT Event,VOID *Context) {
  (VOID)Context;if(!mExited&&mHead!=mTail)gBS->SignalEvent(Event);
}
STATIC EFI_STATUS EFIAPI Reset(EFI_ABSOLUTE_POINTER_PROTOCOL *This,BOOLEAN Extended) {
  (VOID)Extended;if(This!=&mPointer)return EFI_INVALID_PARAMETER;
  if(mExited||!mRunning)return EFI_NOT_READY;
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);mHead=mTail=0;ZeroMem(&mLast,sizeof(mLast));mPrimary=0;gBS->RestoreTPL(Old);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetState(EFI_ABSOLUTE_POINTER_PROTOCOL *This,EFI_ABSOLUTE_POINTER_STATE *State) {
  if(This!=&mPointer||!State)return EFI_INVALID_PARAMETER;
  if(mExited||(!mRunning&&mHead==mTail))return EFI_NOT_READY;
  ++mGets;
  if(mRecoveryPending&&!mBusy) {
    EFI_TPL Entry=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Entry);mGetTpl=(UINT32)Entry;
    // Never download firmware from a timer/notification callback. The GUI
    // consumes this protocol cooperatively at APP and continues pumping USB.
    if(Entry==TPL_APPLICATION) {
      mBusy=TRUE;mRecoveryPending=FALSE;
      mHead=mTail=0;Release();mPrimary=0;
      RecoverySnapshot();
      DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_RECOVERY begin=1 first=%02x reset=%02x%02x%02x%02x%02x%02x host=%02x%02x attempt=%u\n",
        mWatchdogFirst,mResetState[0],mResetState[1],mResetState[2],mResetState[3],mResetState[4],mResetState[5],mHostCommand[0],mHostCommand[1],mRecoveries));
      if(mRecoveries>=3)mRecoveryStatus=EFI_DEVICE_ERROR;
      else {
        ++mRecoveries;
        if(mWatchdogFirst==0xfe){WriteByte(0x1fb43e,0xaa);MicroSecondDelay(15000);WriteByte(0x1fb605,0xa5);}
        mRecoveryStatus=InitializeController();
        if(mRecoveryStatus==EFI_SUCCESS)(VOID)NormalReporting();
      }
      mWatchdogStreak=mConsecutiveBad=0;
      MmioWrite32(IRQ_STATUS,0);
      DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_RECOVERY end=1 status=%r attempt=%u\n",mRecoveryStatus,mRecoveries));
      if(mRecoveryStatus!=EFI_SUCCESS){mStatus=mRecoveryStatus;mRunning=FALSE;}
      mBusy=FALSE;
      // Even a failed recovery must deliver the queued release once. The
      // next empty GetState returns NOT_READY with sampling stopped.
    }
  }
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS E=EFI_NOT_READY;
  if(mHead!=mTail){*State=mQueue[mHead];mHead=(mHead+1)%QUEUE;E=EFI_SUCCESS;++mTaken;}
  gBS->RestoreTPL(Old);return E;
}
STATIC VOID EFIAPI Fence(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mExited=TRUE;if(mPins||mClockHeld||mIrqOwned)mRetained=TRUE;
}
BOOLEAN Y700TouchRetained(VOID){return mRetained;}
VOID Y700TouchReplay(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_RESULT status=%r step=%u running=%u retained=%u pins=%u fw=%02x max=%u,%u frames=%u bad=%u events=%u transfers=%u gpio_spi=1 dma=0\n",
    mStatus,mStep,mRunning,mRetained,mPins,mVersion,mMaxX,mMaxY,mFrames,mBad,mEvents,mTransfers));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_IO read_us=%u max_us=%u gets=%u taken=%u queued=%u bad_streak=%u bad_sum=%x irq=%u,%u\n",
    mLastReadUs,mMaxReadUs,mGets,mTaken,(UINT32)((mTail+QUEUE-mHead)%QUEUE),mConsecutiveBad,mBadSum,mIrqBefore,mIrqAfter));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_WDT count=%u handshake=%u recoveries=%u pending=%u recovery=%r get_tpl=%u reset=%02x%02x%02x%02x%02x%02x host=%02x%02x\n",
    mWatchdogs,mHandshakes,mRecoveries,mRecoveryPending,mRecoveryStatus,mGetTpl,mResetState[0],mResetState[1],mResetState[2],mResetState[3],mResetState[4],mResetState[5],mHostCommand[0],mHostCommand[1]));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_BOOT headers=%u crc=%u normal=%r ack=%02x reset=%02x%02x%02x%02x%02x%02x\n",
    mHeadersVerified,mFirmwareCrcVerified,mNormalStatus,mNormalAck,mNormalReset[0],mNormalReset[1],mNormalReset[2],mNormalReset[3],mNormalReset[4],mNormalReset[5]));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_IRQ owned=%u saved_ctl=%x saved_cfg=%x initial=%x cfg=%x status=%x edges=%u idle=%u\n",
    mIrqOwned,mIrqSavedCtl,mIrqSavedCfg,mIrqInitialStatus,mIrqOwned?MmioRead32(IRQ_CFG):0,mIrqOwned?MmioRead32(IRQ_STATUS):0,mIrqEdges,mIrqIdle));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_LAST_GOOD first=%02x%02x%02x%02x%02x%02x\n",mLastGood[0],mLastGood[1],mLastGood[2],mLastGood[3],mLastGood[4],mLastGood[5]));
  if(mRecoveries)for(UINTN Row=0;Row<4;++Row) {
    CHAR8 Hex[129];STATIC CONST CHAR8 Digits[]="0123456789abcdef";
    for(UINTN I=0;I<64;++I){UINT8 B=mHistory[Row*64+I];Hex[I*2]=Digits[B>>4];Hex[I*2+1]=Digits[B&15];}
    Hex[128]=0;DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_HISTORY index=%u data=%a\n",(UINT32)Row,Hex));
  }
  if(mBad) {
    CHAR8 Hex[2*Y700_TOUCH_FRAME_BYTES+1];STATIC CONST CHAR8 Digits[]="0123456789abcdef";
    for(UINTN I=0;I<Y700_TOUCH_FRAME_BYTES;++I){Hex[2*I]=Digits[mLastBad[I]>>4];Hex[2*I+1]=Digits[mLastBad[I]&15];}
    Hex[sizeof(Hex)-1]=0;DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_LAST_BAD %a\n",Hex));
  }
}
EFI_STATUS Y700TouchStop(VOID) {
  if(mExited||mRetained)return EFI_ACCESS_DENIED;
  EFI_STATUS E;
  mRunning=FALSE;
  if(mTimer) {
    E=gBS->SetTimer(mTimer,TimerCancel,0);if(E!=EFI_SUCCESS)goto Failed;
    E=gBS->CloseEvent(mTimer);if(E!=EFI_SUCCESS)goto Failed;mTimer=NULL;
  }
  if(mHandle) {
    E=gBS->DisconnectController(mHandle,NULL,NULL);if(E!=EFI_SUCCESS)goto Failed;
    E=gBS->UninstallMultipleProtocolInterfaces(mHandle,&gEfiAbsolutePointerProtocolGuid,&mPointer,
      &gEfiConsoleInDeviceGuid,NULL,&gEfiDevicePathProtocolGuid,&mPath,NULL);
    if(E!=EFI_SUCCESS)goto Failed;mHandle=NULL;
  }
  if(mPointer.WaitForInput) {
    E=gBS->CloseEvent(mPointer.WaitForInput);if(E!=EFI_SUCCESS)goto Failed;mPointer.WaitForInput=NULL;
  }
  E=RestoreIrqLatch();if(E!=EFI_SUCCESS)goto Failed;
  E=RestorePins();if(E!=EFI_SUCCESS)goto Failed;
  if(mClockHeld) {E=mClock->DisableClock(mClock,mClockId);if(E!=EFI_SUCCESS)goto Failed;mClockHeld=FALSE;}
  if(mExit){E=gBS->CloseEvent(mExit);if(E!=EFI_SUCCESS)goto Failed;mExit=NULL;}
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_STOP status=%r pins_restored=1 clock_released=1 protocols_removed=1\n",EFI_SUCCESS));
  return EFI_SUCCESS;
Failed:
  mRetained=TRUE;return E;
}
EFI_STATUS Y700TouchStart(CONST VOID *Fdt) {
  if(!Fdt||FdtCheckHeader(Fdt))return EFI_INVALID_PARAMETER;
  INT32 Node=FdtPathOffset(Fdt,Y700_TOUCH_NODE),Len=0;
  CONST UINT8 ExpectedReg[]={0x00,0xa9,0x00,0x00,0x00,0x00,0x40,0x00};
  CONST VOID *Reg=Node<0?NULL:FdtGetProp(Fdt,Node,"reg",&Len);
  if(!Reg||Len!=sizeof(ExpectedReg)||CompareMem(Reg,ExpectedReg,sizeof(ExpectedReg)))return EFI_UNSUPPORTED;
  if(mPins||mRunning||mRetained||mExited||mIrqOwned)return EFI_ALREADY_STARTED;
  mStep=1;EFI_STATUS E=AcquirePins();
  if(E==EFI_SUCCESS)E=AcquireIrqLatch();
  if(E==EFI_SUCCESS){mStep=2;E=InitializeController();}
  // Report-rate configuration is independent from downloading and checking
  // the firmware. Keep its failed ACK visible while observing actual frames;
  // do not discard the input/recovery path before it can diagnose a WDT.
  if(E==EFI_SUCCESS){mStep=29;(VOID)NormalReporting();}
  if(E==EFI_SUCCESS)MmioWrite32(IRQ_STATUS,0);
  if(E==EFI_SUCCESS) {
    mStep=3;(VOID)gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&mGop);
    mPointer.Reset=Reset;mPointer.GetState=GetState;mPointer.Mode=&mMode;
    E=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,Wait,NULL,&mPointer.WaitForInput);
  }
  if(E==EFI_SUCCESS)E=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Fence,NULL,&gEfiEventExitBootServicesGuid,&mExit);
  if(E==EFI_SUCCESS)E=gBS->InstallMultipleProtocolInterfaces(&mHandle,
    &gEfiAbsolutePointerProtocolGuid,&mPointer,&gEfiConsoleInDeviceGuid,NULL,&gEfiDevicePathProtocolGuid,&mPath,NULL);
  if(E==EFI_SUCCESS)E=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,NULL,&mTimer);
  if(E==EFI_SUCCESS){mRunning=TRUE;E=gBS->SetTimer(mTimer,TimerPeriodic,20ULL*10000);}
  if(E==EFI_SUCCESS){mStep=4;(VOID)gBS->ConnectController(mHandle,NULL,NULL,TRUE);}
  else {EFI_STATUS Cleanup=Y700TouchStop();if(Cleanup!=EFI_SUCCESS)E=Cleanup;}
  mStatus=E;Y700TouchReplay();return E;
}
