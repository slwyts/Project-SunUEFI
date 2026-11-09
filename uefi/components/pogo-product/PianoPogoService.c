// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bring-up follows Linux hid-nanosic-wn8030.c probe for the Piano variant:
// power on (reset pulse 2 ms, bootrom 500 ms, wake 50 ms), bootrom ID 0xC8 and
// state 0, header, 503-byte RAM code packets with 3 ms after every write,
// verify (state 3), start, then 68-byte reports on each rising edge of GPIO97
#include "PianoPogoService.h"
#include "PianoPogoBoard.h"
#include "PianoPogoMcu.h"
#include "PianoPogoDxe.h"
#include "PianoGeniI2cPio.h"
#include "PianoPogoMcuFirmware.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>

#define BUS_HZ 1000000U
#define WRITE_GAP_US 3000U
#define POLL_FALLBACK_US 100000U
#define KEEPALIVE_US 12000000ULL
#define READ_TIMEOUT_US 4000U
#define MAX_READ_ERRORS 50U

typedef enum {
  StepOff,StepClocks,StepPower,StepResetRelease,StepWake,StepBootId,StepBootIdRead,StepBootState,
  StepHeaderState,StepHeaderStateRead,StepData,StepVerify,StepVerifyState,StepVerifyStateRead,StepArm,
  StepRuntime,StepUnavailable,StepStopped
} POGO_STEP;
STATIC CONST CHAR8 *mStepNames[]={"off","clocks","power","reset","wake","boot-id","boot-id-read","boot-state",
  "header-state","header-state-read","data","verify","verify-state","verify-state-read","arm","runtime","unavailable","stopped"};

STATIC struct {
  BOOLEAN Started,Busy,InTransfer,DxeStarted,DxeStopped,Released,ReleaseClean,Attached,CapsLed,ReadAgain,Speculative;
  UINT8 LastFrame[PIANO_POGO_REPORT_BYTES];
  POGO_STEP Step;
  UINT64 Frequency,CounterStart,CounterEnd,StartUs,NextUs,LastReadUs,KeepaliveUs;
  UINT32 Offset,Reads,Frames,Empty,ReadErrors,Consecutive,Commands,CommandErrors;
  EFI_STATUS LastStatus,DxeStatus,ReleaseStatus,SeRestore;
  PIANO_POGO_BOARD Board;
  PIANO_POGO_BOARD_RELEASE BoardRelease;
  PIANO_GENI_I2C I2c;
} mPogo;
STATIC PIANO_POGO_DXE mDxe;

STATIC UINT64 NowUs(VOID) {
  UINT64 Counter=GetPerformanceCounter();
  UINT64 Ticks=mPogo.CounterEnd>=mPogo.CounterStart?Counter-mPogo.CounterStart:mPogo.CounterStart-Counter;
  return (Ticks/mPogo.Frequency)*1000000ULL+((Ticks%mPogo.Frequency)*1000000ULL)/mPogo.Frequency;
}
STATIC EFI_STATUS EFIAPI SeRead(VOID *Context,UINT32 Offset,UINT32 *Value) {
  (VOID)Context;if(Offset>=PIANO_GENI_SE6_BYTES || (Offset&3) || !Value)return EFI_INVALID_PARAMETER;
  *Value=MmioRead32(PIANO_GENI_SE6_BASE+Offset);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SeWrite(VOID *Context,UINT32 Offset,UINT32 Value) {
  (VOID)Context;if(Offset>=PIANO_GENI_SE6_BYTES || (Offset&3))return EFI_INVALID_PARAMETER;
  MmioWrite32(PIANO_GENI_SE6_BASE+Offset,Value);MemoryFence();return EFI_SUCCESS;
}
STATIC UINT64 EFIAPI SeNow(VOID *Context){(VOID)Context;return NowUs();}

STATIC EFI_STATUS ReleaseHardware(VOID) {
  if(mPogo.Released)return mPogo.ReleaseStatus;
  mPogo.SeRestore=PianoGeniI2cClose(&mPogo.I2c);
  EFI_STATUS Board=PianoPogoBoardRelease(&mPogo.Board,&mPogo.BoardRelease);
  mPogo.Released=TRUE;
  mPogo.ReleaseClean=mPogo.SeRestore==EFI_SUCCESS && Board==EFI_SUCCESS && mPogo.BoardRelease.Clean;
  mPogo.ReleaseStatus=mPogo.ReleaseClean?EFI_SUCCESS:Board!=EFI_SUCCESS?Board:EFI_DEVICE_ERROR;
  return mPogo.ReleaseStatus;
}

STATIC VOID Unavailable(CONST CHAR8 *Reason,EFI_STATUS Status) {
  POGO_STEP Failed=mPogo.Step;if(Failed==StepStopped)return;
  if(mPogo.DxeStarted && !mPogo.DxeStopped){mPogo.DxeStatus=PianoPogoDxeStop(&mDxe);mPogo.DxeStopped=TRUE;}
  EFI_STATUS Release=ReleaseHardware();
  mPogo.Step=StepUnavailable;mPogo.LastStatus=Status;
  DEBUG((DEBUG_WARN,"PIANO_POGO_UNAVAILABLE reason=%a step=%a status=%r se6_fw=%08x se6_if_disable=%08x i2c_irq=%08x i2c_ios=%08x release=%r input=volume-power-keys\n",
    Reason,mStepNames[Failed],Status,mPogo.I2c.Firmware,mPogo.I2c.InterfaceDisable,mPogo.I2c.LastIrq,mPogo.I2c.LastIos,Release));
}

// Transfers are synchronous; InTransfer only guards a callback re-entering
STATIC EFI_STATUS Xfer(CONST UINT8 *Write,UINTN WriteBytes,UINT8 *Read,UINTN ReadBytes,UINT32 TimeoutUs) {
  mPogo.InTransfer=TRUE;
  EFI_STATUS S=PianoGeniI2cTransfer(&mPogo.I2c,PIANO_POGO_MCU_ADDRESS,Write,WriteBytes,Read,ReadBytes,TimeoutUs);
  mPogo.InTransfer=FALSE;return S;
}
STATIC EFI_STATUS Send(CONST UINT8 *Wire,UINTN Bytes){return Xfer(Wire,Bytes,NULL,0,20000);}
STATIC EFI_STATUS BootCommand(UINT8 Command){UINT8 Wire[2];PianoPogoMcuBootCommand(Command,Wire);return Send(Wire,sizeof(Wire));}
STATIC EFI_STATUS BootRead(UINT8 *Value) {
  UINT8 Register=PIANO_POGO_BOOT_REGISTER;
  return Xfer(&Register,1,Value,1,5000);
}
STATIC VOID Command(UINT8 Code,UINT8 Value) {
  UINT8 Wire[PIANO_POGO_COMMAND_WIRE_BYTES];PianoPogoMcuRuntimeCommand(Code,Value,Wire);
  EFI_STATUS S=Send(Wire,sizeof(Wire));++mPogo.Commands;
  if(S!=EFI_SUCCESS){++mPogo.CommandErrors;DEBUG((DEBUG_WARN,"PIANO_POGO_COMMAND code=%02x value=%02x status=%r\n",Code,Value,S));}
}

// One bring-up action, then the wait Linux uses before the next one
STATIC VOID BringUp(UINT64 Now) {
  EFI_STATUS S=EFI_SUCCESS;UINT8 Value=0;UINT32 Wait=WRITE_GAP_US;POGO_STEP Next=mPogo.Step+1;
  CONST CHAR8 *Reason=mStepNames[mPogo.Step];
  switch(mPogo.Step) {
  case StepClocks: {
    S=PianoPogoBoardClocksOn(&mPogo.Board);if(S!=EFI_SUCCESS){Reason="qup-clocks";break;}
    PIANO_GENI_PIO_IO Io={PIANO_GENI_SE6_BASE,PIANO_GENI_SE6_BYTES,NULL,SeRead,SeWrite,SeNow};
    S=PianoGeniI2cOpen(&mPogo.I2c,&Io,mPogo.Board.SeSourceHz,BUS_HZ);
    DEBUG((DEBUG_WARN,"PIANO_POGO_SE6 fw=%08x protocol=%u if_disable=%08x tx_depth=%u rx_depth=%u source_hz=%u status=%r\n",
      mPogo.I2c.Firmware,(mPogo.I2c.Firmware>>8)&0xFF,mPogo.I2c.InterfaceDisable,mPogo.I2c.TxDepth,mPogo.I2c.RxDepth,mPogo.Board.SeSourceHz,S));
    // A GSI-only SE6 would need GPI DMA, which this product does not do
    if(S==EFI_UNSUPPORTED){Reason=(mPogo.I2c.InterfaceDisable&BIT0)?"se6-gsi-only":"se6-not-i2c";break;}
    if(S!=EFI_SUCCESS){Reason="se6-open";break;}
    S=PianoPogoBoardI2cPins(&mPogo.Board);if(S!=EFI_SUCCESS)Reason="i2c-pins";
    Wait=0;break;
  }
  case StepPower:S=PianoPogoBoardPowerOn(&mPogo.Board);Wait=5000;break;
  case StepResetRelease:S=PianoPogoBoardSetReset(&mPogo.Board,FALSE);Wait=500000;break;
  case StepWake:S=PianoPogoBoardSetSleep(&mPogo.Board,FALSE);Wait=50000;break;
  case StepBootId:S=BootCommand(0x04);break;
  case StepBootIdRead:
    S=BootRead(&Value);
    if(S==EFI_SUCCESS && Value!=PIANO_POGO_BOOT_ID){S=EFI_INCOMPATIBLE_VERSION;Reason="boot-id";}
    if(S==EFI_SUCCESS)S=BootCommand(0x05);
    DEBUG((DEBUG_WARN,"PIANO_POGO_BOOT_ID value=%02x status=%r\n",Value,S));
    break;
  case StepBootState: {
    S=BootRead(&Value);
    if(S==EFI_SUCCESS && Value!=0){S=EFI_DEVICE_ERROR;Reason="boot-state";}
    UINT8 Wire[PIANO_POGO_HEADER_WIRE_BYTES];
    if(S==EFI_SUCCESS)S=PianoPogoMcuHeaderCommand(mPianoPogoMcuImageHeader,mPianoPogoMcuCode,sizeof(mPianoPogoMcuCode),Wire);
    if(S==EFI_SUCCESS)S=Send(Wire,sizeof(Wire));
    DEBUG((DEBUG_WARN,"PIANO_POGO_BOOT_HEADER state=%02x code_bytes=%u status=%r\n",Value,(UINT32)sizeof(mPianoPogoMcuCode),S));
    break;
  }
  case StepHeaderState:S=BootCommand(0x05);break;
  case StepHeaderStateRead:
    S=BootRead(&Value);if(S==EFI_SUCCESS && Value!=1){S=EFI_DEVICE_ERROR;Reason="header-rejected";}
    mPogo.Offset=0;Wait=0;break;
  case StepData: {
    UINT8 Wire[PIANO_POGO_DATA_WIRE_BYTES];
    UINT32 Bytes=PianoPogoMcuDataCommand(mPianoPogoMcuCode,sizeof(mPianoPogoMcuCode),mPogo.Offset,Wire);
    S=Bytes?Send(Wire,sizeof(Wire)):EFI_DEVICE_ERROR;
    if(S==EFI_SUCCESS){mPogo.Offset+=Bytes;if(mPogo.Offset<sizeof(mPianoPogoMcuCode))Next=StepData;}
    break;
  }
  case StepVerify:S=BootCommand(0x08);Wait=WRITE_GAP_US+10000;break;
  case StepVerifyState:S=BootCommand(0x05);break;
  case StepVerifyStateRead:
    S=BootRead(&Value);if(S==EFI_SUCCESS && Value!=3){S=EFI_CRC_ERROR;Reason="verify";}
    if(S==EFI_SUCCESS)S=BootCommand(0x09);
    DEBUG((DEBUG_WARN,"PIANO_POGO_BOOT_VERIFY state=%02x status=%r\n",Value,S));
    Wait=WRITE_GAP_US+50000;break;
  case StepArm:
    S=PianoPogoBoardArmIrq(&mPogo.Board);Wait=0;
    if(S==EFI_SUCCESS)DEBUG((DEBUG_WARN,"PIANO_POGO_READY bring_up_ms=%lu firmware_bytes=%u\n",(Now-mPogo.StartUs)/1000,(UINT32)sizeof(mPianoPogoMcuCode)));
    break;
  default:return;
  }
  if(mPogo.Step==StepStopped)return;
  if(S!=EFI_SUCCESS){Unavailable(Reason,S);return;}
  mPogo.Step=Next;mPogo.NextUs=Now+Wait;
}

STATIC EFI_STATUS EFIAPI BackendReady(VOID *Context) {
  (VOID)Context;return mPogo.Step==StepRuntime && !mPogo.Released && !mPogo.I2c.Broken?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI BackendDataReady(VOID *Context,BOOLEAN *Ready) {
  (VOID)Context;if(!Ready)return EFI_INVALID_PARAMETER;
  BOOLEAN Edge=PianoPogoBoardIrqPending(&mPogo.Board);
  // Edges can coalesce between two polls, so after each new frame read once
  // more until the MCU has nothing new. Until the first latched edge proves
  // the TLMM path, also read at a slow fixed rate. Such speculative reads
  // drop a frame identical to the previous one
  *Ready=Edge || mPogo.ReadAgain || (!mPogo.Board.EdgeSeen && NowUs()-mPogo.LastReadUs>=POLL_FALLBACK_US);
  mPogo.Speculative=!Edge;
  return EFI_SUCCESS;
}
STATIC UINT64 EFIAPI BackendNow(VOID *Context){(VOID)Context;return NowUs();}
STATIC EFI_STATUS EFIAPI BackendRead(VOID *Context,UINT64 Deadline,UINT8 Frame[PIANO_POGO_FRAME_BYTES],UINTN *Bytes) {
  (VOID)Context;if(!Frame || !Bytes)return EFI_INVALID_PARAMETER;
  *Bytes=0;UINT64 Now=NowUs();
  if(Deadline<=Now+1000)return EFI_NOT_READY;
  UINT32 Timeout=(UINT32)MIN(Deadline-Now-1000,(UINT64)READ_TIMEOUT_US);
  UINT8 Register=PIANO_POGO_RUNTIME_REGISTER;mPogo.LastReadUs=Now;++mPogo.Reads;
  EFI_STATUS S=Xfer(&Register,1,Frame,PIANO_POGO_FRAME_BYTES,Timeout);
  // The edge that announced this report is already acknowledged, so a failed
  // read is retried a few times instead of leaving the report in the MCU
  mPogo.ReadAgain=FALSE;
  if(S!=EFI_SUCCESS){++mPogo.ReadErrors;++mPogo.Consecutive;mPogo.LastStatus=S;mPogo.ReadAgain=mPogo.Consecutive<=3;return EFI_NOT_READY;}
  mPogo.Consecutive=0;
  if(!PianoPogoMcuFrameHasReports(Frame)){++mPogo.Empty;return EFI_NOT_READY;}
  if(mPogo.Speculative && CompareMem(Frame,mPogo.LastFrame,PIANO_POGO_REPORT_BYTES)==0){++mPogo.Empty;return EFI_NOT_READY;}
  CopyMem(mPogo.LastFrame,Frame,PIANO_POGO_REPORT_BYTES);mPogo.ReadAgain=TRUE;
  ++mPogo.Frames;*Bytes=PIANO_POGO_FRAME_BYTES;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BackendStop(VOID *Context,PIANO_POGO_BACKEND_STOP *Stop) {
  (VOID)Context;if(!Stop)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=ReleaseHardware();
  Stop->Status=S;Stop->Quiet=TRUE;Stop->Clean=mPogo.ReleaseClean;Stop->Retained=!mPogo.ReleaseClean;
  return S;
}

STATIC VOID Runtime(UINT64 Now) {
  if(!mPogo.DxeStarted) {
    // SimpleInit moves its cursor by motion*gui.mouse_scale/Resolution pixels
    // (default scale 1). Resolution 1 makes one touchpad count one pixel,
    // about 34 px per mm, so a full-width swipe crosses the 3200 px screen
    STATIC CONST EFI_SIMPLE_POINTER_MODE Mode={1,1,1,TRUE,TRUE};
    STATIC CONST PIANO_POGO_DXE_BACKEND Backend={1,NULL,BackendReady,BackendDataReady,BackendNow,BackendRead,BackendStop};
    mPogo.DxeStarted=TRUE;mPogo.DxeStatus=PianoPogoDxeStart(&mDxe,&Backend,&Mode);
    DEBUG((DEBUG_WARN,"PIANO_POGO_INPUT_PUBLISHED status=%r text_in=1 pointer=1\n",mPogo.DxeStatus));
    if(mPogo.DxeStatus!=EFI_SUCCESS){mPogo.DxeStopped=PianoPogoDxeReport(&mDxe)==NULL;Unavailable("publish",mPogo.DxeStatus);return;}
    return;
  }
  (VOID)PianoPogoDxePump(&mDxe);
  if(mPogo.Step==StepStopped)return;
  CONST PIANO_POGO_DXE_REPORT *Report=PianoPogoDxeReport(&mDxe);
  if(Report==NULL || Report->Retained){Unavailable("input-producer",Report?Report->Status:EFI_COMPROMISED_DATA);return;}
  PIANO_POGO_CONTROL_EVENT Event;
  while(PianoPogoReadControl(&mDxe.Adapter.Core,&Event)==EFI_SUCCESS) {
    if(Event.Kind!=PianoPogoAttachment)continue;
    mPogo.Attached=Event.Value!=0;
    DEBUG((DEBUG_WARN,"PIANO_POGO_COVER attached=%u frames=%u reads=%u irq_mode=%a\n",mPogo.Attached,mPogo.Frames,mPogo.Reads,mPogo.Board.EdgeSeen?"edge":"poll"));
    if(mPogo.Attached) {
      // Linux attach handling: touchpad on, keyboard power on, Caps LED
      Command(PIANO_POGO_CMD_TOUCHPAD,1);Command(PIANO_POGO_CMD_KEYBOARD_POWER,1);
      mPogo.CapsLed=(mDxe.Adapter.Core.Toggle&EFI_CAPS_LOCK_ACTIVE)!=0;
      Command(PIANO_POGO_CMD_INDICATOR,mPogo.CapsLed?PIANO_POGO_LED_CAPS_ON:PIANO_POGO_LED_CAPS_OFF);
      mPogo.KeepaliveUs=Now+KEEPALIVE_US;
    }
  }
  if(mPogo.Attached) {
    BOOLEAN Caps=(mDxe.Adapter.Core.Toggle&EFI_CAPS_LOCK_ACTIVE)!=0;
    if(Caps!=mPogo.CapsLed){mPogo.CapsLed=Caps;Command(PIANO_POGO_CMD_INDICATOR,Caps?PIANO_POGO_LED_CAPS_ON:PIANO_POGO_LED_CAPS_OFF);}
    // The cover powers its keyboard down without this 12 s request
    if(Now>=mPogo.KeepaliveUs){mPogo.KeepaliveUs=Now+KEEPALIVE_US;Command(PIANO_POGO_CMD_KEYBOARD_POWER,1);}
  }
  if(mPogo.I2c.Broken || mPogo.Consecutive>=MAX_READ_ERRORS)Unavailable("transport",mPogo.LastStatus);
}

EFI_STATUS PianoPogoStart(CONST VOID *Fdt) {
  if(mPogo.Started)return EFI_ALREADY_STARTED;
  if(Fdt==NULL)return EFI_INVALID_PARAMETER;
  INT32 Bus=FdtPathOffset(Fdt,"/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000");
  INT32 Mcu=FdtPathOffset(Fdt,"/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000/nanosic@4c");
  INT32 Length=0;CONST UINT8 *Reg=Bus<0?NULL:FdtGetProp(Fdt,Bus,"reg",&Length);
  BOOLEAN BusOk=Reg && Length==8 && ((UINT32)Reg[0]<<24|(UINT32)Reg[1]<<16|(UINT32)Reg[2]<<8|Reg[3])==PIANO_GENI_SE6_BASE &&
    ((UINT32)Reg[4]<<24|(UINT32)Reg[5]<<16|(UINT32)Reg[6]<<8|Reg[7])==PIANO_GENI_SE6_BYTES;
  Reg=Mcu<0?NULL:FdtGetProp(Fdt,Mcu,"reg",&Length);
  BOOLEAN McuOk=Reg && Length==4 && Reg[0]==0 && Reg[1]==0 && Reg[2]==0 && Reg[3]==PIANO_POGO_MCU_ADDRESS;
  if(!BusOk || !McuOk){DEBUG((DEBUG_WARN,"PIANO_POGO_START status=%r bus=%u mcu=%u\n",EFI_NOT_FOUND,BusOk,McuOk));return EFI_NOT_FOUND;}
  mPogo.Frequency=GetPerformanceCounterProperties(&mPogo.CounterStart,&mPogo.CounterEnd);
  if(mPogo.Frequency<1000000ULL || mPogo.CounterStart==mPogo.CounterEnd)return EFI_UNSUPPORTED;
  mPogo.Started=TRUE;mPogo.Step=StepClocks;mPogo.StartUs=mPogo.NextUs=NowUs();mPogo.LastStatus=EFI_NOT_STARTED;
  DEBUG((DEBUG_WARN,"PIANO_POGO_START status=%r firmware=%a code_bytes=%u\n",EFI_SUCCESS,PIANO_POGO_MCU_FIRMWARE_SHA256,(UINT32)sizeof(mPianoPogoMcuCode)));
  return EFI_SUCCESS;
}

VOID PianoPogoPump(VOID) {
  if(!mPogo.Started || mPogo.Busy || mPogo.Step==StepUnavailable || mPogo.Step==StepStopped)return;
  mPogo.Busy=TRUE;UINT64 Now=NowUs();
  if(mPogo.Step==StepRuntime)Runtime(Now);
  else if(Now>=mPogo.NextUs)BringUp(Now);
  // A retire that ran inside the input producer could not stop it there
  if(mPogo.Step==StepStopped && mPogo.DxeStarted && !mPogo.DxeStopped){mPogo.DxeStatus=PianoPogoDxeStop(&mDxe);mPogo.DxeStopped=mPogo.DxeStatus!=EFI_NOT_READY;}
  mPogo.Busy=FALSE;
}

VOID PianoPogoReplay(VOID) {
  CONST PIANO_POGO_DXE_REPORT *Dxe=PianoPogoDxeReport(&mDxe);
  CONST PIANO_POGO_INPUT *Input=&mDxe.Adapter.Core;
  DEBUG((DEBUG_WARN,"PIANO_POGO_INPUT step=%a attached=%u known=%u frames=%u reads=%u empty=%u read_errors=%u commands=%u command_errors=%u irq_seen=%u accepted=%u rejected=%u keys=%u pointer=%u status=%r work=%u\n",
    mStepNames[mPogo.Step],mPogo.Attached,Input->AttachKnown,mPogo.Frames,mPogo.Reads,mPogo.Empty,mPogo.ReadErrors,mPogo.Commands,mPogo.CommandErrors,
    mPogo.Board.EdgeSeen,Input->AcceptedFrames,Input->RejectedFrames,(UINT32)Input->KeyCount,Input->RelativePending,Dxe?Dxe->Status:mPogo.DxeStatus,Dxe?Dxe->WorkPending:0));
}

EFI_STATUS PianoPogoStop(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report) {
  (VOID)Context;if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->Started=mPogo.Started;
  Report->Timer=Report->Protocols=EFI_SUCCESS;
  if(!mPogo.Started || mPogo.InTransfer) {
    Report->Retained=mPogo.Started;
    DEBUG((DEBUG_WARN,"PIANO_POGO_RELEASE clean=0 not_restored=%a handoff=blocked\n",mPogo.Started?"all,transfer-in-flight":"none,not-started"));
    return Report->Status=EFI_NOT_READY;
  }
  // Re-entered from a pump callback this runs between two synchronous
  // transfers, so the hardware is released now; the producer may stop later
  if(mPogo.DxeStarted && !mPogo.DxeStopped){mPogo.DxeStatus=PianoPogoDxeStop(&mDxe);mPogo.DxeStopped=mPogo.DxeStatus!=EFI_NOT_READY;}
  // A producer that failed before it was registered installed nothing
  CONST PIANO_POGO_DXE_REPORT *Dxe=PianoPogoDxeReport(&mDxe);
  BOOLEAN InputClean=!mPogo.DxeStarted || Dxe==NULL || Dxe->Stopped;
  if(!InputClean)Report->Protocols=mPogo.DxeStatus==EFI_SUCCESS?EFI_DEVICE_ERROR:mPogo.DxeStatus;
  EFI_STATUS Hardware=ReleaseHardware();
  Report->Returned=TRUE;Report->ServicesLost=Dxe!=NULL && Dxe->ServicesLost;
  Report->Clean=InputClean && Hardware==EFI_SUCCESS && mPogo.ReleaseClean && !Report->ServicesLost;
  Report->Retained=!Report->Clean;
  Report->Status=Report->Clean?EFI_SUCCESS:Hardware!=EFI_SUCCESS?Hardware:EFI_DEVICE_ERROR;
  mPogo.Step=StepStopped;
  CONST PIANO_POGO_BOARD_RELEASE *B=&mPogo.BoardRelease;
  DEBUG((DEBUG_WARN,"PIANO_POGO_RELEASE clean=%u handoff=%a not_restored=%a%a%a%a%a%a input=%r se6=%r pins=%r rails=%r clocks=%r frames=%u reads=%u read_errors=%u commands=%u\n",
    Report->Clean,Report->Clean?"ready":"blocked",Report->Clean?"none":"",InputClean?"":"input-protocols,",mPogo.SeRestore==EFI_SUCCESS?"":"se6-registers,",
    B->Pins==EFI_SUCCESS?"":"pins,",B->Rails==EFI_SUCCESS?"":"rpmh-votes,",B->Clocks==EFI_SUCCESS?"":"clock-references,",
    mPogo.DxeStatus,mPogo.SeRestore,B->Pins,B->Rails,B->Clocks,mPogo.Frames,mPogo.Reads,mPogo.ReadErrors,mPogo.Commands));
  return Report->Status;
}
