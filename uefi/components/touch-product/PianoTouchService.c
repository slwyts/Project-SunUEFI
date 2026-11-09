// SPDX-License-Identifier: BSD-2-Clause-Patent
// NT36532 TDDI touch on QUP1 SE2. Bring-up is the Linux 7.3 nt36xxx.c probe
// path for a panel the bootloader lit ("No DRM panel to follow"): eng reset,
// chip-ID trim loop, nvt_download_firmware_hw_crc, poll info, fw info, IRQ.
// Like that driver, no reset GPIO, regulator or panel command is touched
#include "PianoTouchService.h"
#include "PianoTouchNvt.h"
#include "PianoTouchMatrix.h"
#include "PianoTouchFirmware.h"
#include <Protocol/EFIClock.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>

#define TOUCH_SE 0xA88000U
#define FIRMWARE_CHUNK 512U
#define FRAME_MIN_GAP_US 16000U
#define POLL_NO_IRQ_US 16000U
#define POLL_IRQ_STALL_US 100000U
#define MAX_READ_ERRORS 50U
#define DOWNLOAD_ATTEMPTS 3U
#define TLMM 0xF100000U
#define CTL(G) (TLMM+(G)*0x1000U)
#define IO(G) (CTL(G)+4U)
#define INTR_CFG(G) (CTL(G)+8U)
#define INTR_STATUS(G) (CTL(G)+0xCU)
#define CTL_FIELDS 0x3FFU
#define CTL_OE BIT9
#define PINS 6U
#define PIN_IRQ 4U
#define PIN_LCD_ID 5U
#define SE2_PATH "/soc/qcom,qupv3_1_geni_se@ac0000/spi@a88000"
STATIC CONST UINT32 mGpio[PINS]={40,41,42,43,162,100};
STATIC CONST CHAR8 *mClockNames[]={"gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk",
  "gcc_qupv3_wrap1_core_2x_clk","gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_s2_clk"};
// Stock board DT name first, then the product Linux DT name
STATIC CONST CHAR8 *mTouchNodes[]={SE2_PATH "/novatek@0",SE2_PATH "/touchscreen@0"};

typedef enum {
  TOff,TClocks,TEngReset,TTrimReset,TTrimRead,TDownloadReset,TDownloadCrc,TAutoCopyOn,TWrite,TAutoCopyWait,
  TCrcEnable,TBootReady,TResetState,TInfo,TReportMode,TReportReady,TArm,TRuntime,TUnavailable,TStopped
} T_STEP;
STATIC CONST CHAR8 *mStepNames[]={"off","clocks","eng-reset","trim-reset","trim-read","download-reset","download-crc",
  "auto-copy-on","write","auto-copy-wait","crc-enable","boot-ready","reset-state","info","report-mode","report-ready","arm","runtime","unavailable","stopped"};

STATIC struct {
  BOOLEAN Started,Busy,InTransfer,Released,Published,Cascade,EdgeSeen,EdgePending,IrqArmed,Pending;
  T_STEP Step;
  UINT64 Frequency,CounterStart,CounterEnd,StartUs,NextUs,LastReadUs;
  UINT32 Table,Retry,Attempt,Part,PartOffset,Polls,Payload,Frames,Reads,ReadErrors,Consecutive,WdtCount,Downloads;
  UINT32 StatePolls,StateReads,StateChanges,Peak;
  INT32 LcdId;
  CONST UINT8 *Fw;UINTN FwBytes;CONST CHAR8 *FwName;
  PIANO_NVT_IMAGE Image;PIANO_NVT_MAP Map;UINT32 Swrst;
  EFI_CLOCK_PROTOCOL *Clock;UINTN ClockIds[5];UINT32 ClockHeld,SeHz;
  struct {UINT32 Ctl,Io,Intr;BOOLEAN Saved,Changed;} Pins[PINS];
  struct {UINT32 Firmware,InterfaceDisable;BOOLEAN Open,Broken;} Spi;
  EFI_STATUS LastStatus,SpiRestore,PinsStatus,ClocksStatus,InputStatus;
  EFI_HANDLE Handle;EFI_EVENT Wait;
  EFI_ABSOLUTE_POINTER_PROTOCOL Pointer;EFI_ABSOLUTE_POINTER_MODE Mode;EFI_ABSOLUTE_POINTER_STATE State;
} mT;
STATIC PIANO_TOUCH_MATRIX mMatrix;
STATIC struct {UINT8 Data[64];UINT32 Us;EFI_STATUS Status;} mFirstFrames[3];
STATIC UINT8 mTx[PIANO_NVT_TRANSFER_LEN+2],mRx[PIANO_NVT_READ_LEN+2],mFrame[PIANO_NVT_READ_LEN];
STATIC struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} mPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0x9F3B6C2E,0x1D47,0x4A85,{0xB0,0xE6,0x5C,0x7D,0x8A,0x91,0xF2,0xB4}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}};

STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC UINT64 NowUs(VOID) {
  UINT64 Counter=GetPerformanceCounter();
  UINT64 Ticks=mT.CounterEnd>=mT.CounterStart?Counter-mT.CounterStart:mT.CounterStart-Counter;
  return (Ticks/mT.Frequency)*1000000ULL+((Ticks%mT.Frequency)*1000000ULL)/mT.Frequency;
}
STATIC EFI_STATUS ClocksOn(VOID) {
  EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;EFI_CLOCK_PROTOCOL *Clock=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if(S!=EFI_SUCCESS || !Clock || Clock->Version!=0x1000b || !Clock->GetClockID || !Clock->EnableClock ||
      !Clock->DisableClock || !Clock->IsClockEnabled || !Clock->IsClockOn || !Clock->GetClockFreqHz || !Clock->SetClockFreqHz)return EFI_UNSUPPORTED;
  mT.Clock=Clock;
  for(UINTN I=0;I<ARRAY_SIZE(mClockNames);++I) {
    BOOLEAN On=FALSE;S=Exact(Clock->GetClockID(Clock,mClockNames[I],&mT.ClockIds[I]));
    if(S==EFI_SUCCESS && I==ARRAY_SIZE(mClockNames)-1){
      BOOLEAN Enabled=FALSE;UINT32 Hz=0;S=Exact(Clock->IsClockEnabled(Clock,mT.ClockIds[I],&Enabled));
      if(S==EFI_SUCCESS && !Enabled){
        EFI_STATUS Rate=Clock->GetClockFreqHz(Clock,mT.ClockIds[I],&Hz);
        // Configure only an unused domain with no ROM active mux record.
        if(Rate==EFI_NOT_STARTED)S=Exact(Clock->SetClockFreqHz(Clock,mT.ClockIds[I],19200000,EFI_CLOCK_FREQUENCY_HZ_EXACT,&Hz));
        else if(Rate!=EFI_SUCCESS)S=Exact(Rate);
        DEBUG((DEBUG_WARN,"PIANO_TOUCH_CLOCK_CONFIG status=%r hz=%u initialized=%u\n",S,Hz,Rate==EFI_NOT_STARTED));
      }
    }
    if(S==EFI_SUCCESS){S=Exact(Clock->EnableClock(Clock,mT.ClockIds[I]));if(S==EFI_SUCCESS)mT.ClockHeld|=1U<<I;}
    if(S==EFI_SUCCESS)S=Exact(Clock->IsClockOn(Clock,mT.ClockIds[I],&On));
    if(S==EFI_SUCCESS && !On)S=EFI_NOT_READY;
    if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"PIANO_TOUCH_CLOCK name=%a status=%r\n",mClockNames[I],S));return S;}
  }
  S=Exact(Clock->GetClockFreqHz(Clock,mT.ClockIds[4],&mT.SeHz));
  return S!=EFI_SUCCESS?S:mT.SeHz<1000000U?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC VOID SavePin(UINTN P) {
  if(mT.Pins[P].Saved)return;
  mT.Pins[P].Ctl=MmioRead32(CTL(mGpio[P]));mT.Pins[P].Io=MmioRead32(IO(mGpio[P]));
  mT.Pins[P].Intr=MmioRead32(INTR_CFG(mGpio[P]));mT.Pins[P].Saved=TRUE;
}
STATIC EFI_STATUS WritePin(UINTN P,UINT32 Value) {
  mT.Pins[P].Changed=TRUE;MmioWrite32(CTL(mGpio[P]),Value);MemoryFence();
  return (MmioRead32(CTL(mGpio[P]))&CTL_FIELDS)==(Value&CTL_FIELDS)?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
// The Y700 driver demonstrates mode-0 GPIO SPI without touching GENI's
// GSI-only registers. Piano's final DT maps MISO/MOSI/CLK/CS to 40/41/42/43.
STATIC VOID Drive(UINTN P,BOOLEAN High){MmioWrite32(IO(mGpio[P]),High?BIT1:0);}
STATIC EFI_STATUS SpiPins(VOID){
  for(UINTN P=0;P<4;++P)SavePin(P);
  for(UINTN P=0;P<4;++P){
    UINT32 Value=(mT.Pins[P].Ctl&~CTL_FIELDS)|(2U<<6)|1U;
    if(Value&BIT11)Value|=BIT12;
    EFI_STATUS S=WritePin(P,Value);if(S!=EFI_SUCCESS)return S;
  }
  Drive(3,TRUE);Drive(2,FALSE);Drive(1,FALSE);
  for(UINTN P=1;P<4;++P){
    UINT32 Value=(mT.Pins[P].Ctl&~CTL_FIELDS)|(2U<<6)|CTL_OE|(P==3?3U:1U);
    if(Value&BIT11)Value|=BIT12;
    EFI_STATUS S=WritePin(P,Value);if(S!=EFI_SUCCESS)return S;
  }
  mT.Spi.Open=TRUE;return EFI_SUCCESS;
}
// gpiod GPIOD_IN: GPIO function, output disabled, pull and drive kept
STATIC EFI_STATUS InputPin(UINTN P){SavePin(P);return WritePin(P,mT.Pins[P].Ctl&~(CTL_OE|(0xFU<<2)));}
STATIC EFI_STATUS RestorePin(UINTN P) {
  if(!mT.Pins[P].Changed)return EFI_SUCCESS;
  MmioWrite32(IO(mGpio[P]),mT.Pins[P].Io&BIT1);MmioWrite32(CTL(mGpio[P]),mT.Pins[P].Ctl);MemoryFence();
  if((MmioRead32(CTL(mGpio[P]))&CTL_FIELDS)!=(mT.Pins[P].Ctl&CTL_FIELDS))return EFI_DEVICE_ERROR;
  mT.Pins[P].Changed=FALSE;return EFI_SUCCESS;
}

// Transfers are synchronous; InTransfer only guards a callback re-entering
STATIC EFI_STATUS Xfer(CONST UINT8 *Tx,UINT8 *Rx,UINTN Len) {
  if(!mT.Spi.Open||mT.Released||!Tx||!Len||Len>sizeof(mTx))return EFI_NOT_READY;
  mT.InTransfer=TRUE;EFI_STATUS S=EFI_SUCCESS;
  UINT64 Deadline=NowUs()+500000;
  Drive(3,FALSE);MicroSecondDelay(2);
  for(UINTN At=0;At<Len;++At){
    UINT8 Out=Tx[At],In=0;
    for(UINTN Bit=0;Bit<8;++Bit){
      Drive(1,(Out&0x80)!=0);Out<<=1;NanoSecondDelay(100);Drive(2,TRUE);
      In=(UINT8)((In<<1)|(MmioRead32(IO(mGpio[0]))&1));
      NanoSecondDelay(100);Drive(2,FALSE);
    }
    if(Rx)Rx[At]=In;
    if((At&63U)==63U&&NowUs()>=Deadline){S=EFI_TIMEOUT;mT.Spi.Broken=TRUE;break;}
  }
  Drive(3,TRUE);MicroSecondDelay(2);mT.InTransfer=FALSE;return S;
}
STATIC EFI_STATUS Write(UINT8 *Buf,UINTN Len){Buf[0]|=0x80;return Xfer(Buf,NULL,Len);}
// Buf[0] is the offset; the IC answers after one dummy byte, so Len - 1 data
// bytes arrive at rx[2..] and are returned in Buf[1..]
STATIC EFI_STATUS Read(UINT8 *Buf,UINTN Len) {
  if(Len+1>sizeof(mRx))return EFI_BAD_BUFFER_SIZE;
  Buf[0]&=0x7F;CopyMem(mTx,Buf,Len);mTx[Len]=0;
  EFI_STATUS S=Xfer(mTx,mRx,Len+1);
  if(S==EFI_SUCCESS)CopyMem(Buf+1,mRx+2,Len-1);
  return S;
}
STATIC EFI_STATUS SetPage(UINT32 Addr){UINT8 B[3]={0xFF,(UINT8)(Addr>>15),(UINT8)(Addr>>7)};return Write(B,3);}
STATIC EFI_STATUS ReadFrame(UINTN Len) {
  UINT8 B[65];

  // Long GPIO-SPI chip selects stall the running controller. Readdress each
  // chunk, then let the payload checksum reject a frame changed between reads.
  mFrame[0]=0;
  for(UINTN At=1;At<Len;At+=sizeof(B)-1) {
    UINT32 Address=mT.Map.EventBuf+(UINT32)At-1;
    UINTN Bytes=MIN(Len-At,sizeof(B)-1);

    EFI_STATUS S=SetPage(Address);
    if(S!=EFI_SUCCESS)return S;
    ZeroMem(B,sizeof(B));
    B[0]=(UINT8)(Address&0x7F);
    S=Read(B,Bytes+1);
    if(S!=EFI_SUCCESS)return S;
    CopyMem(mFrame+At,B+1,Bytes);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WriteAddr(UINT32 Addr,UINT8 Data) {
  EFI_STATUS S=SetPage(Addr);if(S!=EFI_SUCCESS)return S;
  UINT8 B[2]={(UINT8)(Addr&0x7F),Data};return Write(B,2);
}
STATIC EFI_STATUS WriteBank(UINT32 Des,UINT32 Sram,UINT32 LenAddr,UINT32 Size,UINT32 Golden,UINT32 Crc) {
  UINT8 B[5];EFI_STATUS S=SetPage(Des);if(S!=EFI_SUCCESS)return S;
  B[0]=(UINT8)(Des&0x7F);B[1]=(UINT8)Sram;B[2]=(UINT8)(Sram>>8);B[3]=(UINT8)(Sram>>16);if((S=Write(B,4))!=EFI_SUCCESS)return S;
  B[0]=(UINT8)(LenAddr&0x7F);B[1]=(UINT8)Size;B[2]=(UINT8)(Size>>8);B[3]=(UINT8)(Size>>16);if((S=Write(B,4))!=EFI_SUCCESS)return S;
  B[0]=(UINT8)(Golden&0x7F);B[1]=(UINT8)Crc;B[2]=(UINT8)(Crc>>8);B[3]=(UINT8)(Crc>>16);B[4]=(UINT8)(Crc>>24);
  return Write(B,5);
}

STATIC EFI_STATUS EFIAPI PointerReset(EFI_ABSOLUTE_POINTER_PROTOCOL *This,BOOLEAN Ext) {
  if(This!=&mT.Pointer)return EFI_INVALID_PARAMETER;(VOID)Ext;mT.Pending=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PointerState(EFI_ABSOLUTE_POINTER_PROTOCOL *This,EFI_ABSOLUTE_POINTER_STATE *State) {
  if(This!=&mT.Pointer || !State)return EFI_INVALID_PARAMETER;
  ++mT.StatePolls;
  if(!mT.Pending || mT.Released)return EFI_NOT_READY;
  *State=mT.State;mT.Pending=FALSE;++mT.StateReads;return EFI_SUCCESS;
}
STATIC VOID EFIAPI PointerWait(EFI_EVENT Event,VOID *Context){(VOID)Context;if(mT.Pending && !mT.Released)gBS->SignalEvent(Event);}
STATIC EFI_STATUS Unpublish(VOID);
STATIC EFI_STATUS Publish(VOID) {
  mT.Mode=(EFI_ABSOLUTE_POINTER_MODE){0,0,0,PIANO_TOUCH_SCREEN_W-1,PIANO_TOUCH_SCREEN_H-1,0,0};
  mT.Pointer.Reset=PointerReset;mT.Pointer.GetState=PointerState;mT.Pointer.Mode=&mT.Mode;
  EFI_STATUS S=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_CALLBACK,PointerWait,NULL,&mT.Wait);
  if(S!=EFI_SUCCESS){mT.Wait=NULL;return S;}
  mT.Pointer.WaitForInput=mT.Wait;
  S=gBS->InstallMultipleProtocolInterfaces(&mT.Handle,&gEfiDevicePathProtocolGuid,&mPath,
    &gEfiAbsolutePointerProtocolGuid,&mT.Pointer,NULL);
  if(S!=EFI_SUCCESS){gBS->CloseEvent(mT.Wait);mT.Wait=NULL;mT.Handle=NULL;return S;}
  mT.Published=TRUE;
  // A retire run from an install notify found nothing to uninstall yet
  if(mT.Released)return Unpublish();
  (VOID)gBS->ConnectController(mT.Handle,NULL,NULL,TRUE);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Unpublish(VOID) {
  if(!mT.Published)return EFI_SUCCESS;
  EFI_STATUS S=gBS->DisconnectController(mT.Handle,NULL,NULL);
  if(S!=EFI_SUCCESS && S!=EFI_NOT_FOUND && S!=EFI_NOT_STARTED)return S;
  S=gBS->UninstallMultipleProtocolInterfaces(mT.Handle,&gEfiDevicePathProtocolGuid,&mPath,
    &gEfiAbsolutePointerProtocolGuid,&mT.Pointer,NULL);
  if(S!=EFI_SUCCESS)return S;
  mT.Published=FALSE;mT.Handle=NULL;
  if(mT.Wait){S=gBS->CloseEvent(mT.Wait);mT.Wait=NULL;}
  return S;
}

STATIC EFI_STATUS ReleaseHardware(VOID) {
  if(mT.Released)return mT.PinsStatus!=EFI_SUCCESS?mT.PinsStatus:mT.ClocksStatus;
  mT.Released=TRUE;
  if(mT.Spi.Open){Drive(3,TRUE);Drive(2,FALSE);mT.Spi.Open=FALSE;}
  // No serial-engine registers or DMA state were changed by GPIO SPI.
  mT.SpiRestore=EFI_SUCCESS;mT.PinsStatus=EFI_SUCCESS;mT.ClocksStatus=EFI_SUCCESS;
  if(mT.IrqArmed) {
    MmioWrite32(INTR_CFG(mGpio[PIN_IRQ]),mT.Pins[PIN_IRQ].Intr);MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();
    if(MmioRead32(INTR_CFG(mGpio[PIN_IRQ]))!=mT.Pins[PIN_IRQ].Intr)mT.PinsStatus=EFI_DEVICE_ERROR;else mT.IrqArmed=FALSE;
  }
  for(UINTN P=PINS;P>0;--P){EFI_STATUS S=RestorePin(P-1);if(S!=EFI_SUCCESS)mT.PinsStatus=S;}
  if(mT.Clock)for(UINTN I=ARRAY_SIZE(mClockNames);I>0;--I)if(mT.ClockHeld&(1U<<(I-1))) {
    EFI_STATUS S=Exact(mT.Clock->DisableClock(mT.Clock,mT.ClockIds[I-1]));
    if(S==EFI_SUCCESS)mT.ClockHeld&=~(1U<<(I-1));else mT.ClocksStatus=S;
  }
  return mT.PinsStatus!=EFI_SUCCESS?mT.PinsStatus:mT.ClocksStatus;
}
STATIC VOID Unavailable(CONST CHAR8 *Reason,EFI_STATUS Status) {
  T_STEP Failed=mT.Step;if(Failed==TStopped)return;
  mT.InputStatus=Unpublish();EFI_STATUS Release=ReleaseHardware();
  mT.Step=TUnavailable;mT.LastStatus=Status;
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_UNAVAILABLE reason=%a step=%a status=%r se2_fw=%08x se2_if_disable=%08x lcd_id=%d firmware=%a release=%r\n",
    Reason,mStepNames[Failed],Status,mT.Spi.Firmware,mT.Spi.InterfaceDisable,mT.LcdId,mT.FwName?mT.FwName:"none",Release));
}

STATIC VOID ReportPointer(VOID) {
  UINT32 Buttons=mMatrix.Touching?EFI_ABSP_TouchActive:0;
  UINT64 X=mMatrix.Touching?(UINT64)mMatrix.X:mT.State.CurrentX;
  UINT64 Y=mMatrix.Touching?(UINT64)mMatrix.Y:mT.State.CurrentY;

  mT.Peak=MAX(mT.Peak,(UINT32)MAX(mMatrix.MaxDelta,0));
  if(Buttons==mT.State.ActiveButtons && X==mT.State.CurrentX && Y==mT.State.CurrentY)return;
  if(Buttons!=mT.State.ActiveButtons)DEBUG((DEBUG_VERBOSE,"PIANO_TOUCH_CONTACT active=%u x=%lu y=%lu peak=%d frame=%u\n",mMatrix.Touching,X,Y,mMatrix.MaxDelta,mMatrix.LastFrameNo));
  mT.State.CurrentX=X;mT.State.CurrentY=Y;mT.State.CurrentZ=0;mT.State.ActiveButtons=Buttons;
  mT.Pending=TRUE;++mT.StateChanges;
}

// One bring-up action per call, then the delay the driver uses before the next
STATIC VOID BringUp(UINT64 Now) {
  EFI_STATUS S=EFI_SUCCESS;UINT32 Wait=0;T_STEP Next=mT.Step+1;CONST CHAR8 *Reason=mStepNames[mT.Step];UINT8 B[48];
  switch(mT.Step) {
  case TClocks: {
    if((S=ClocksOn())!=EFI_SUCCESS){Reason="qup-clocks";break;}
    mT.Spi.Firmware=MmioRead32(TOUCH_SE+0x68);mT.Spi.InterfaceDisable=MmioRead32(TOUCH_SE+0x64);
    UINT32 Active=MmioRead32(TOUCH_SE+0x40);
    if(Active&(BIT0|BIT12)){S=EFI_ACCESS_DENIED;Reason="se2-active";break;}
    S=SpiPins();
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_SE2 fw=%08x if_disable=%08x active=%08x gpio_spi=1 dma=0 status=%r\n",
      mT.Spi.Firmware,mT.Spi.InterfaceDisable,Active,S));
    if(S!=EFI_SUCCESS){Reason="spi-pins";break;}
    // Touch lcd-id (0 CSOT, 1 BOE) picks the firmware, as in the driver
    if((S=InputPin(PIN_LCD_ID))!=EFI_SUCCESS){Reason="lcd-id-pin";break;}
    mT.LcdId=(INT32)(MmioRead32(IO(mGpio[PIN_LCD_ID]))&1);(VOID)RestorePin(PIN_LCD_ID);
    mT.Fw=mT.LcdId?mPianoTouchFirmwareBoe:mPianoTouchFirmwareCsot;
    mT.FwBytes=mT.LcdId?sizeof(mPianoTouchFirmwareBoe):sizeof(mPianoTouchFirmwareCsot);
    mT.FwName=mT.LcdId?"boe":"csot";
    S=PianoNvtParseImage(mT.Fw,mT.FwBytes,&mT.Image);
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_FIRMWARE lcd_id=%d panel=%a sha256=%a bytes=%u partitions=%u cascade_header=%u fw_ver=%02x status=%r\n",
      mT.LcdId,mT.FwName,mT.LcdId?PIANO_TOUCH_FIRMWARE_BOE_SHA256:PIANO_TOUCH_FIRMWARE_CSOT_SHA256,(UINT32)mT.FwBytes,
      mT.Image.Count,mT.Image.Cascade2ndHeader,mT.Image.FwVer,S));
    if(S!=EFI_SUCCESS)Reason="firmware-image";
    break;
  }
  case TEngReset:S=WriteAddr(PIANO_NVT_ENG_RST_ADDR,0x5A);Wait=1000+10000;mT.Table=mT.Retry=0;break;
  case TTrimReset:S=WriteAddr(gPianoNvtHwRegs[mT.Table].SwrstSif,0x69);Wait=5000;break;
  case TTrimRead: {
    UINT32 Trim=gPianoNvtHwRegs[mT.Table].ChipVerTrim;
    if((S=SetPage(Trim))!=EFI_SUCCESS)break;
    ZeroMem(B,7);B[0]=(UINT8)(Trim&0x7F);if((S=Write(B,7))!=EFI_SUCCESS)break;
    ZeroMem(B,7);B[0]=(UINT8)(Trim&0x7F);if((S=Read(B,7))!=EFI_SUCCESS)break;
    INTN Row=PianoNvtMatchTrim(B+1);
    if(Row<0) {
      if(++mT.Retry<5){Next=TTrimReset;Wait=10000;break;}
      mT.Retry=0;
      if(++mT.Table<3){Next=TTrimReset;Wait=10000;break;}
      DEBUG((DEBUG_WARN,"PIANO_TOUCH_CHIP_ID id=%02x%02x%02x%02x%02x%02x match=0\n",B[1],B[2],B[3],B[4],B[5],B[6]));
      S=EFI_NOT_FOUND;Reason="chip-id";break;
    }
    UINT8 Id[6];CopyMem(Id,B+1,6);
    if((S=SetPage(PIANO_NVT_ENB_CASC_ADDR))!=EFI_SUCCESS)break;
    B[0]=(UINT8)(PIANO_NVT_ENB_CASC_ADDR&0x7F);B[1]=0;if((S=Read(B,2))!=EFI_SUCCESS)break;
    // ENB_CASC bit0 set is a single chip; NT36532 uses hw_reg_addr_info after the match
    mT.Cascade=!(B[1]&1);PianoNvtMemoryMap(Row,mT.Cascade,&mT.Map);mT.Swrst=gPianoNvtHwRegs[0].SwrstSif;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_CHIP_ID id=%02x%02x%02x%02x%02x%02x row=%d cascade=%u table=%u\n",
      Id[0],Id[1],Id[2],Id[3],Id[4],Id[5],(INT32)Row,mT.Cascade,mT.Table));
    mT.Attempt=0;Next=TDownloadReset;break;
  }
  case TDownloadReset:++mT.Downloads;S=WriteAddr(mT.Swrst,0x69);Wait=5000;break;
  case TDownloadCrc:
    S=WriteBank(mT.Map.IlmDes,mT.Image.Part[0].SramAddr,mT.Map.IlmLength,mT.Image.Part[0].Size,mT.Map.GIlmChecksum,mT.Image.Part[0].Crc);
    if(S==EFI_SUCCESS)S=WriteBank(mT.Map.DlmDes,mT.Image.Part[1].SramAddr,mT.Map.DlmLength,mT.Image.Part[1].Size,mT.Map.GDlmChecksum,mT.Image.Part[1].Crc);
    mT.Part=mT.PartOffset=0;Next=mT.Image.Cascade2ndHeader?TAutoCopyOn:TWrite;break;
  case TAutoCopyOn:S=WriteAddr(mT.Map.TxAutoCopyEn,0x56);break;
  case TWrite: {
    while(mT.Part<mT.Image.Count && !mT.Image.Part[mT.Part].Size)++mT.Part;
    if(mT.Part>=mT.Image.Count){Next=mT.Image.Cascade2ndHeader?TAutoCopyWait:TCrcEnable;mT.Polls=0;break;}
    // Size + 1 bytes per partition, one firmware chunk per pump.
    CONST PIANO_NVT_PARTITION *P=&mT.Image.Part[mT.Part];
    UINT32 Total=P->Size+1,Len=MIN(Total-mT.PartOffset,FIRMWARE_CHUNK),Sram=P->SramAddr+mT.PartOffset,Bin=P->BinAddr+mT.PartOffset;
    if((S=SetPage(Sram))!=EFI_SUCCESS)break;
    mTx[0]=(UINT8)(Sram&0x7F);
    for(UINT32 I=0;I<Len;++I)mTx[1+I]=Bin+I<mT.FwBytes?mT.Fw[Bin+I]:0;
    if((S=Write(mTx,Len+1))!=EFI_SUCCESS)break;
    mT.PartOffset+=Len;
    if(mT.PartOffset>=Total){++mT.Part;mT.PartOffset=0;}
    Next=TWrite;break;
  }
  case TAutoCopyWait:
    if((S=SetPage(mT.Map.TxAutoCopyEn))!=EFI_SUCCESS)break;
    B[0]=(UINT8)(mT.Map.TxAutoCopyEn&0x7F);B[1]=0xFF;if((S=Read(B,2))!=EFI_SUCCESS)break;
    if(B[1]!=0){if(++mT.Polls<200){Next=TAutoCopyWait;Wait=1000;break;}S=EFI_TIMEOUT;Reason="auto-copy";}
    break;
  case TCrcEnable:
    if((S=SetPage(mT.Map.EventBuf))!=EFI_SUCCESS)break;
    ZeroMem(B,7);B[0]=PIANO_NVT_EVENT_RESET_COMPLETE;if((S=Write(B,7))!=EFI_SUCCESS)break;
    B[0]=PIANO_NVT_EVENT_HOST_CMD;B[1]=0xAE;B[2]=0;S=Write(B,3);break;
  case TBootReady:S=WriteAddr(mT.Map.BootRdy,1);Wait=5000;mT.Polls=0;break;
  case TResetState:
    if(!mT.Polls && (S=SetPage(mT.Map.EventBuf))!=EFI_SUCCESS)break;
    ZeroMem(B,6);B[0]=PIANO_NVT_EVENT_RESET_COMPLETE;if((S=Read(B,6))!=EFI_SUCCESS)break;
    if(B[1]>=0xA0 && B[1]<=0xAF){DEBUG((DEBUG_WARN,"PIANO_TOUCH_DOWNLOAD ok attempt=%u reset_state=%02x ms=%lu\n",mT.Attempt,B[1],(Now-mT.StartUs)/1000));break;}
    if(++mT.Polls<=10){Next=TResetState;Wait=10000;break;}
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_DOWNLOAD failed attempt=%u reset_state=%02x\n",mT.Attempt,B[1]));
    S=EFI_DEVICE_ERROR;Reason="firmware-start";break;
  case TInfo:
    mT.Payload=PIANO_NVT_DEFAULT_PAYLOAD;
    if(mT.Map.PollInfo) {
      if((S=SetPage(mT.Map.PollInfo))!=EFI_SUCCESS)break;
      ZeroMem(B,40);B[0]=(UINT8)(mT.Map.PollInfo&0x7F);if((S=Read(B,40))!=EFI_SUCCESS)break;
      UINT32 Len=B[3]|((UINT32)B[4]<<8);
      if(Len && Len<=PIANO_NVT_MAX_PAYLOAD)mT.Payload=Len;
    }
    if((S=SetPage(mT.Map.EventBuf))!=EFI_SUCCESS)break;
    ZeroMem(B,39);B[0]=PIANO_NVT_EVENT_FWINFO;
    if(Read(B,39)==EFI_SUCCESS)DEBUG((DEBUG_WARN,"PIANO_TOUCH_FW_INFO fw_ver=%02x valid=%u x_num=%u y_num=%u payload=%u\n",
      B[1],(UINT8)(B[1]+B[2])==0xFF,B[3],B[4],mT.Payload));
    mT.Polls=0;
    break;
  case TReportMode:
    if((S=SetPage(mT.Map.EventBuf))!=EFI_SUCCESS)break;
    ZeroMem(B,6);B[0]=PIANO_NVT_EVENT_RESET_COMPLETE;if((S=Read(B,6))!=EFI_SUCCESS)break;
    if(B[1]<0xA3 || B[1]>0xAF) {
      if(++mT.Polls<50){Next=TReportMode;Wait=10000;break;}
      S=EFI_TIMEOUT;Reason="firmware-calibration";break;
    }
    // Stock DATA_MODE_53 selects idle scans after calibration. Active scans
    // replace the matrix faster than the GPIO transport can copy it.
    B[0]=PIANO_NVT_EVENT_HOST_CMD;B[1]=0xBF;B[2]=0x01;B[3]=0;B[4]=2;B[5]=0;
    S=Write(B,6);Wait=20000;mT.Polls=0;break;
  case TReportReady:
    if((S=SetPage(mT.Map.EventBuf))!=EFI_SUCCESS)break;
    B[0]=PIANO_NVT_EVENT_HOST_CMD;B[1]=0xFF;if((S=Read(B,2))!=EFI_SUCCESS)break;
    if(B[1]) {
      if(++mT.Polls<10){Next=TReportReady;Wait=10000;break;}
      S=EFI_TIMEOUT;Reason="report-mode";break;
    }
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_REPORT_MODE idle=1 status=%02x\n",B[1]));
    break;
  case TArm: {
    // Rising edge latched in TLMM with delivery off; restored on release
    if((S=InputPin(PIN_IRQ))!=EFI_SUCCESS)break;
    UINT32 Cfg=(mT.Pins[PIN_IRQ].Intr&~(BIT0|BIT1|BIT2|BIT3|BIT4|(7U<<8)))|BIT4|BIT2|BIT1|(3U<<8);
    MmioWrite32(INTR_CFG(mGpio[PIN_IRQ]),Cfg);MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();
    mT.IrqArmed=TRUE;mT.EdgePending=FALSE;mT.LastReadUs=Now;PianoTouchMatrixReset(&mMatrix);
    if(!mT.Published && (S=Publish())!=EFI_SUCCESS){Reason="publish";break;}
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_READY bring_up_ms=%lu downloads=%u payload=%u\n",(Now-mT.StartUs)/1000,mT.Downloads,mT.Payload));
    break;
  }
  default:return;
  }
  if(mT.Step==TStopped)return;
  // As nvt_download_firmware_hw_crc: a failed step restarts at the bootloader reset
  if(S!=EFI_SUCCESS && mT.Step>=TDownloadReset && mT.Step<=TResetState && !mT.Spi.Broken && ++mT.Attempt<DOWNLOAD_ATTEMPTS) {
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_DOWNLOAD retry attempt=%u step=%a status=%r\n",mT.Attempt,mStepNames[mT.Step],S));
    S=EFI_SUCCESS;Next=TDownloadReset;Wait=0;
  }
  if(S!=EFI_SUCCESS){Unavailable(Reason,S);return;}
  mT.Step=Next;mT.NextUs=NowUs()+Wait;
}

STATIC VOID Runtime(UINT64 Now) {
  if(mT.IrqArmed && (MmioRead32(INTR_STATUS(mGpio[PIN_IRQ]))&BIT0)) {
    MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();mT.EdgePending=mT.EdgeSeen=TRUE;
  }
  // Frames are read on the IRQ edge like the driver; the slow poll covers a
  // latch that never fires or a line left high
  UINT64 Since=Now-mT.LastReadUs;
  if(Since<FRAME_MIN_GAP_US || (!mT.EdgePending && Since<(mT.EdgeSeen?POLL_IRQ_STALL_US:POLL_NO_IRQ_US)))return;
  mT.EdgePending=FALSE;mT.LastReadUs=Now;++mT.Reads;
  UINTN Len=PIANO_NVT_HEADER_LEN+mT.Payload;
  UINT64 Begin=NowUs();
  EFI_STATUS S=ReadFrame(Len);
  if(S!=EFI_SUCCESS) {
    ++mT.ReadErrors;mT.LastStatus=S;
    if(mT.Spi.Broken || ++mT.Consecutive>=MAX_READ_ERRORS)Unavailable("transport",S);
    return;
  }
  mT.Consecutive=0;
  // nvt_fw_recovery skips 0x77 handshakes; nvt_wdt_fw_recovery downloads
  // again after more than ten 0xFD/0xFE frames, idling the MCU first on 0xFE
  BOOLEAN Hand=TRUE,Wdt=TRUE;
  for(UINTN I=1;I<7;++I){if(mFrame[I]!=0x77)Hand=FALSE;if(mFrame[I]!=0xFD && mFrame[I]!=0xFE)Wdt=FALSE;}
  if(!Wdt)mT.WdtCount=0;
  else if(++mT.WdtCount>10) {
    mT.WdtCount=0;DEBUG((DEBUG_WARN,"PIANO_TOUCH_WDT_RECOVERY state=%02x\n",mFrame[1]));
    if(mFrame[1]==0xFE){(VOID)WriteAddr(mT.Swrst,0xAA);mT.NextUs=NowUs()+15000;}else mT.NextUs=Now;
    mMatrix.Touching=FALSE;ReportPointer();
    mT.Attempt=0;mT.Step=TDownloadReset;return;
  }
  if(Hand)return;
  EFI_STATUS Feed=PianoTouchMatrixFeed(&mMatrix,mFrame+PIANO_NVT_HEADER_LEN,mT.Payload);
  if(mT.Reads<=3){
    CONST UINT8 *P=mFrame+PIANO_NVT_HEADER_LEN;
    CopyMem(mFirstFrames[mT.Reads-1].Data,P,64);mFirstFrames[mT.Reads-1].Us=(UINT32)(NowUs()-Begin);mFirstFrames[mT.Reads-1].Status=Feed;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_FRAME read=%u us=%lu bytes=%u status=%r checksum=%d head=%02x%02x%02x%02x payload=%02x%02x%02x%02x crc=%02x%02x length=%02x%02x%02x%02x inverse=%02x%02x rows=%u cols=%u type=%u\n",
      mT.Reads,NowUs()-Begin,(UINT32)Len,Feed,(INT32)PianoTouchPayloadChecksum(P,mT.Payload),
      mFrame[1],mFrame[2],mFrame[3],mFrame[4],P[0],P[1],P[2],P[3],P[4],P[5],P[8],P[9],P[10],P[11],P[12],P[13],P[49],P[48],P[56]));
  }
  if(Feed==EFI_SUCCESS){++mT.Frames;ReportPointer();}
}

EFI_STATUS PianoTouchStart(CONST VOID *Fdt) {
  if(mT.Started)return EFI_ALREADY_STARTED;
  if(Fdt==NULL)return EFI_INVALID_PARAMETER;
  INT32 Bus=FdtPathOffset(Fdt,SE2_PATH),Length=0;
  CONST UINT8 *Reg=Bus<0?NULL:FdtGetProp(Fdt,Bus,"reg",&Length);
  BOOLEAN BusOk=Reg && Length==8 && ((UINT32)Reg[0]<<24|(UINT32)Reg[1]<<16|(UINT32)Reg[2]<<8|Reg[3])==TOUCH_SE,TsOk=FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(mTouchNodes) && BusOk && !TsOk;++I) {
    INT32 Ts=FdtPathOffset(Fdt,mTouchNodes[I]),StatusLength=0;
    CONST CHAR8 *Compatible=Ts<0?NULL:FdtGetProp(Fdt,Ts,"compatible",&Length);
    CONST CHAR8 *Status=Ts<0?NULL:FdtGetProp(Fdt,Ts,"status",&StatusLength);
    TsOk=Compatible && Length>=19 && AsciiStrnCmp(Compatible,"novatek,NVT-ts-spi",19)==0 &&
         (!Status || (StatusLength>=2 && AsciiStrnCmp(Status,"ok",2)==0));
  }
  if(!BusOk || !TsOk){DEBUG((DEBUG_WARN,"PIANO_TOUCH_START status=%r bus=%u touch=%u\n",EFI_NOT_FOUND,BusOk,TsOk));return EFI_NOT_FOUND;}
  mT.Frequency=GetPerformanceCounterProperties(&mT.CounterStart,&mT.CounterEnd);
  if(mT.Frequency<1000000ULL || mT.CounterStart==mT.CounterEnd)return EFI_UNSUPPORTED;
  mT.Started=TRUE;mT.Step=TClocks;mT.StartUs=mT.NextUs=NowUs();mT.LcdId=-1;mT.LastStatus=EFI_NOT_STARTED;
  return EFI_SUCCESS;
}

VOID PianoTouchPump(VOID) {
  if(!mT.Started || mT.Busy || mT.Step==TUnavailable || mT.Step==TStopped)return;
  mT.Busy=TRUE;UINT64 Now=NowUs();
  if(mT.Step==TRuntime)Runtime(Now);
  else if(Now>=mT.NextUs)BringUp(Now);
  mT.Busy=FALSE;
}

VOID PianoTouchReplay(VOID){
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_MATRIX step=%a frames=%u reads=%u errors=%u checksum_errors=%u skipped=%u probe=%u reference=%u ref_type=%d ref_frames=%u rows=%u cols=%u\n",
    mStepNames[mT.Step],mMatrix.Frames,mT.Reads,mT.ReadErrors,mMatrix.BadChecksum,mMatrix.Skipped,mMatrix.ProbeCount,mMatrix.HaveReference,mMatrix.RefType,mMatrix.RefCount,mMatrix.Rows,mMatrix.Cols));
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_INPUT frame=%u peak=%d max_peak=%u touching=%u x=%d y=%d changes=%u polls=%u consumed=%u pending=%u buttons=%u downloads=%u\n",
    mMatrix.LastFrameNo,mMatrix.MaxDelta,mT.Peak,mMatrix.Touching,mMatrix.X,mMatrix.Y,mT.StateChanges,mT.StatePolls,mT.StateReads,mT.Pending,mT.State.ActiveButtons,mT.Downloads));
  for(UINTN I=0;I<MIN(mT.Reads,ARRAY_SIZE(mFirstFrames));++I){CONST UINT8 *P=mFirstFrames[I].Data;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_FRAME_SAVED index=%u us=%u status=%r payload=%02x%02x%02x%02x crc=%02x%02x length=%02x%02x%02x%02x inverse=%02x%02x rows=%u cols=%u type=%u\n",
      (UINT32)I,mFirstFrames[I].Us,mFirstFrames[I].Status,P[0],P[1],P[2],P[3],P[4],P[5],P[8],P[9],P[10],P[11],P[12],P[13],P[49],P[48],P[56]));
  }
}

EFI_STATUS PianoTouchStop(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report) {
  (VOID)Context;if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->Started=mT.Started;
  if(!mT.Started || mT.InTransfer) {
    Report->Retained=mT.Started;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_RELEASE clean=0 not_restored=%a handoff=blocked\n",mT.Started?"all,transfer-in-flight":"none,not-started"));
    return Report->Status=EFI_NOT_READY;
  }
  EFI_STATUS Input=Unpublish();if(Input!=EFI_SUCCESS)mT.InputStatus=Input;
  EFI_STATUS Hardware=ReleaseHardware();
  mT.Step=TStopped;
  Report->Returned=TRUE;Report->Protocols=mT.Published?(mT.InputStatus!=EFI_SUCCESS?mT.InputStatus:EFI_DEVICE_ERROR):EFI_SUCCESS;
  Report->Timer=EFI_SUCCESS;
  Report->Clean=!mT.Published && Hardware==EFI_SUCCESS && mT.SpiRestore==EFI_SUCCESS;
  Report->Retained=!Report->Clean;
  Report->Status=Report->Clean?EFI_SUCCESS:EFI_DEVICE_ERROR;
  // No DMA is involved and, like the driver's shutdown, no IC command is sent
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_RELEASE clean=%u handoff=%a not_restored=%a%a%a%a%a input=%r se2=%r pins=%r clocks=%r frames=%u reads=%u read_errors=%u\n",
    Report->Clean,Report->Clean?"ready":"blocked",Report->Clean?"none":"",mT.Published?"input-protocols,":"",mT.SpiRestore==EFI_SUCCESS?"":"se2-registers,",
    mT.PinsStatus==EFI_SUCCESS?"":"pins,",mT.ClocksStatus==EFI_SUCCESS?"":"clock-references,",
    mT.InputStatus,mT.SpiRestore,mT.PinsStatus,mT.ClocksStatus,mT.Frames,mT.Reads,mT.ReadErrors));
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_MATRIX frames=%u checksum_errors=%u skipped=%u probe=%u reference=%u ref_type=%d ref_frames=%u rows=%u cols=%u\n",
    mMatrix.Frames,mMatrix.BadChecksum,mMatrix.Skipped,mMatrix.ProbeCount,mMatrix.HaveReference,mMatrix.RefType,mMatrix.RefCount,mMatrix.Rows,mMatrix.Cols));
  return Report->Status;
}
