// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register offsets, opcodes, packing and SCL timing follow Linux 7.3
// i2c-qcom-geni.c, qcom-geni-se.c and geni-se.h; written independently
#include "PianoGeniI2cPio.h"

#define GENI_OUTPUT_CTRL   0x024U
#define GENI_CGC_CTRL      0x028U
#define GENI_STATUS        0x040U
#define GENI_M_CLK_CFG     0x048U
#define GENI_IF_DISABLE_RO 0x064U
#define GENI_FW_REVISION   0x068U
#define GENI_CLK_SEL       0x07CU
#define GENI_BYTE_GRAN     0x254U
#define GENI_DMA_MODE_EN   0x258U
#define GENI_TX_PACKING0   0x260U
#define GENI_TX_PACKING1   0x264U
#define I2C_TX_TRANS_LEN   0x26CU
#define I2C_RX_TRANS_LEN   0x270U
#define I2C_SCL_COUNTERS   0x278U
#define GENI_RX_PACKING0   0x284U
#define GENI_RX_PACKING1   0x288U
#define GENI_M_CMD0        0x600U
#define GENI_M_CMD_CTRL    0x604U
#define GENI_M_IRQ_STATUS  0x610U
#define GENI_M_IRQ_EN      0x614U
#define GENI_M_IRQ_CLEAR   0x618U
#define GENI_S_IRQ_STATUS  0x640U
#define GENI_TX_FIFO       0x700U
#define GENI_RX_FIFO       0x780U
#define GENI_TX_FIFO_STAT  0x800U
#define GENI_RX_FIFO_STAT  0x804U
#define GENI_TX_WATERMARK  0x80CU
#define GENI_RX_WATERMARK  0x810U
#define GENI_RX_RFR_WM     0x814U
#define GENI_IOS           0x908U
#define GENI_GSI_EVENT_EN  0xE18U
#define GENI_SE_IRQ_EN     0xE1CU
#define GENI_HW_PARAM_0    0xE24U
#define GENI_HW_PARAM_1    0xE28U

#define M_ACTIVE      BIT0
#define S_ACTIVE      BIT12
#define IRQ_DONE      BIT0
#define IRQ_CANCEL    BIT4
#define IRQ_ABORT     BIT5
#define IRQ_NACK      BIT10
#define IRQ_RX_WM     BIT26
#define IRQ_RX_LAST   BIT27
#define IRQ_TX_WM     BIT30
// Overrun, illegal command, failure, GP IRQ0..5 (NACK, bus protocol,
// arbitration) and the four FIFO access errors
#define IRQ_ERRORS    (BIT1|BIT2|BIT3|BIT9|BIT10|BIT11|BIT12|BIT13|BIT14|BIT24|BIT25|BIT28|BIT29)
#define IRQ_USED      (IRQ_DONE|IRQ_CANCEL|IRQ_ABORT|IRQ_ERRORS|IRQ_RX_WM|IRQ_RX_LAST|IRQ_TX_WM)
#define CMD_CANCEL    BIT2
#define CMD_ABORT     BIT1
#define OP_WRITE      1U
#define OP_READ       2U
#define OP_STOP_ON_BUS 7U
#define STOP_STRETCH  BIT2
#define TX_WORDS      0x0FFFFFFFU
#define RX_WORDS      0x01FFFFFFU
#define IO_OUTPUT_ALL 0x7FU
#define CGC_ALL       0x7FU
#define PACKING0      0x0007F8FEU
#define PACKING1      0x000FFEFEU
#define MAX_PHASE     4096U
#define RECOVER_US    2000U

STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN ValidIo(CONST PIANO_GENI_PIO_IO *Io) {
  return Io!=NULL && Io->Base==PIANO_GENI_SE6_BASE && Io->Bytes==PIANO_GENI_SE6_BYTES && Io->Read32!=NULL;
}
STATIC EFI_STATUS Rd(PIANO_GENI_I2C *I,UINT32 Offset,UINT32 *Value){return Exact(I->Io.Read32(I->Io.Context,Offset,Value));}
STATIC EFI_STATUS Wr(PIANO_GENI_I2C *I,UINT32 Offset,UINT32 Value){return Exact(I->Io.Write32(I->Io.Context,Offset,Value));}

EFI_STATUS PianoGeniI2cPioCapture(CONST PIANO_GENI_PIO_IO *Io,PIANO_GENI_PIO_SNAPSHOT *S) {
  STATIC CONST UINT32 Offset[]={0x68,0x64,0x40,0x258,0xe18,0xe1c,0x7c,0x48,0x278,0x254,
    0x260,0x264,0x284,0x288,0xe24,0xe28,0x908,0x610,0x640,0x614,0x604,0x634,0x800,0x804,0x80c,0x810,0x26c,0x270,0x600,0x630};
  if(!ValidIo(Io) || S==NULL)return EFI_INVALID_PARAMETER;
  UINT32 *Field[]={&S->Firmware,&S->InterfaceDisable,&S->Status,&S->DmaMode,&S->Events,&S->IrqRoute,
    &S->ClockSelect,&S->MasterClock,&S->SclCounters,&S->ByteGran,&S->TxPack[0],&S->TxPack[1],&S->RxPack[0],&S->RxPack[1],
    &S->HwTx,&S->HwRx,&S->IoLines,&S->MasterIrq,&S->SecondaryIrq,&S->MasterIrqEnable,&S->MasterControl,&S->SecondaryControl,
    &S->TxFifo,&S->RxFifo,&S->TxWatermark,&S->RxWatermark,&S->TxLength,&S->RxLength,&S->MasterCommand,&S->SecondaryCommand};
  STATIC_ASSERT(ARRAY_SIZE(Field)==ARRAY_SIZE(Offset),"snapshot/register count");
  for(UINTN I=0;I<ARRAY_SIZE(Offset);++I)*Field[I]=0;
  for(UINTN I=0;I<ARRAY_SIZE(Offset);++I) {
    EFI_STATUS R=Exact(Io->Read32(Io->Context,Offset[I],Field[I]));
    if(R!=EFI_SUCCESS)return R;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Program(PIANO_GENI_I2C *I,UINT32 Offset,UINT32 Value) {
  UINT32 Old=0,Back=0;EFI_STATUS S=Rd(I,Offset,&Old);
  if(S!=EFI_SUCCESS)return S;
  BOOLEAN Known=FALSE;
  for(UINT32 N=0;N<I->SavedCount;++N)if(I->SavedOffset[N]==Offset)Known=TRUE;
  if(!Known) {
    if(I->SavedCount==PIANO_GENI_SAVED_REGISTERS)return EFI_OUT_OF_RESOURCES;
    I->SavedOffset[I->SavedCount]=Offset;I->SavedValue[I->SavedCount]=Old;++I->SavedCount;
  }
  if(Old==Value)return EFI_SUCCESS;
  S=Wr(I,Offset,Value);if(S!=EFI_SUCCESS)return S;
  S=Rd(I,Offset,&Back);if(S!=EFI_SUCCESS)return S;
  return Back==Value?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

// Linux tables: 19.2 MHz {div 1, high 2, low 8, cycle 18} and 32 MHz
// {div 2, high 3, low 5, cycle 15} give 1 MHz; 400 kHz/100 kHz likewise
STATIC EFI_STATUS Timing(UINT32 SourceHz,UINT32 BusHz,UINT32 *Divider,UINT32 *Counters) {
  STATIC CONST struct {UINT32 Bus,Div,High,Low,Cycle;} T19[]={
    {100000,7,10,12,26},{400000,2,5,11,22},{1000000,1,2,8,18}};
  STATIC CONST struct {UINT32 Bus,Div,High,Low,Cycle;} T32[]={
    {100000,12,9,10,26},{400000,4,3,9,19},{1000000,2,3,5,15}};
  for(UINTN N=0;N<3;++N) {
    if(SourceHz==32000000U && T32[N].Bus==BusHz) {
      *Divider=T32[N].Div;*Counters=(T32[N].High<<20)|(T32[N].Low<<10)|T32[N].Cycle;return EFI_SUCCESS;
    }
    if(T19[N].Bus!=BusHz)continue;
    // Any other source is divided down to at most 19.2 MHz so the SCL stays
    // at or below the Linux 19.2 MHz timing for the same bus speed
    UINT32 Scale=(SourceHz+19200000U-1)/19200000U;
    if(!Scale || Scale*T19[N].Div>0xFFFU)return EFI_UNSUPPORTED;
    *Divider=Scale*T19[N].Div;*Counters=(T19[N].High<<20)|(T19[N].Low<<10)|T19[N].Cycle;return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

EFI_STATUS PianoGeniI2cOpen(PIANO_GENI_I2C *I,CONST PIANO_GENI_PIO_IO *Io,UINT32 SourceHz,UINT32 BusHz) {
  if(I==NULL || !ValidIo(Io) || Io->Write32==NULL || Io->NowUs==NULL || SourceHz<1000000U)return EFI_INVALID_PARAMETER;
  for(UINTN N=0;N<sizeof(*I);++N)((volatile UINT8 *)I)[N]=0;
  I->Io=*Io;I->SourceHz=SourceHz;I->LastStatus=EFI_NOT_STARTED;I->CloseStatus=EFI_NOT_STARTED;
  UINT32 Divider=0,Counters=0,Status=0,Hw0=0,Hw1=0,Value=0;
  EFI_STATUS S=Timing(SourceHz,BusHz,&Divider,&Counters);if(S!=EFI_SUCCESS)return S;
  // Identity checks are reads only. A GSI-only SE (FIFO interface disabled)
  // belongs to the GPI DMA path; it is reported, never reconfigured here
  if((S=Rd(I,GENI_FW_REVISION,&I->Firmware))!=EFI_SUCCESS ||
     (S=Rd(I,GENI_IF_DISABLE_RO,&I->InterfaceDisable))!=EFI_SUCCESS ||
     (S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS ||
     (S=Rd(I,GENI_HW_PARAM_0,&Hw0))!=EFI_SUCCESS || (S=Rd(I,GENI_HW_PARAM_1,&Hw1))!=EFI_SUCCESS)return S;
  if(((I->Firmware>>8)&0xFFU)!=PIANO_GENI_PROTOCOL_I2C || (I->InterfaceDisable&BIT0))return EFI_UNSUPPORTED;
  if(Status&(M_ACTIVE|S_ACTIVE))return EFI_NOT_READY;
  I->TxDepth=(Hw0>>16)&0xFFU;I->RxDepth=(Hw1>>16)&0xFFU;
  if(((Hw0>>24)&0x3FU)!=32 || ((Hw1>>24)&0x3FU)!=32 || I->TxDepth<4 || I->RxDepth<4)return EFI_UNSUPPORTED;
  S=Rd(I,GENI_OUTPUT_CTRL,&Value);if(S!=EFI_SUCCESS)return S;
  if((S=Program(I,GENI_OUTPUT_CTRL,Value|IO_OUTPUT_ALL))!=EFI_SUCCESS)goto Fail;
  S=Rd(I,GENI_CGC_CTRL,&Value);if(S!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_CGC_CTRL,Value|CGC_ALL))!=EFI_SUCCESS)goto Fail;
  S=Rd(I,GENI_DMA_MODE_EN,&Value);if(S!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_DMA_MODE_EN,Value&~BIT0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_GSI_EVENT_EN,0))!=EFI_SUCCESS ||
     // Polled only: no M/S/DMA interrupt is routed to the GIC line
     (S=Program(I,GENI_SE_IRQ_EN,0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_CLK_SEL,0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_M_CLK_CFG,(Divider<<4)|BIT0))!=EFI_SUCCESS ||
     (S=Program(I,I2C_SCL_COUNTERS,Counters))!=EFI_SUCCESS ||
     (S=Program(I,GENI_TX_PACKING0,PACKING0))!=EFI_SUCCESS || (S=Program(I,GENI_TX_PACKING1,PACKING1))!=EFI_SUCCESS ||
     (S=Program(I,GENI_RX_PACKING0,PACKING0))!=EFI_SUCCESS || (S=Program(I,GENI_RX_PACKING1,PACKING1))!=EFI_SUCCESS ||
     (S=Program(I,GENI_BYTE_GRAN,0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_TX_WATERMARK,0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_RX_WATERMARK,I->RxDepth-1))!=EFI_SUCCESS ||
     (S=Program(I,GENI_RX_RFR_WM,I->RxDepth))!=EFI_SUCCESS)goto Fail;
  S=Rd(I,GENI_M_IRQ_EN,&Value);if(S!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_M_IRQ_EN,Value|IRQ_USED))!=EFI_SUCCESS)goto Fail;
  // Per-transfer lengths are changed later; save their originals now
  if((S=Program(I,I2C_TX_TRANS_LEN,0))!=EFI_SUCCESS || (S=Program(I,I2C_RX_TRANS_LEN,0))!=EFI_SUCCESS)goto Fail;
  if((S=Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32))!=EFI_SUCCESS)goto Fail;
  I->Open=TRUE;I->LastStatus=EFI_SUCCESS;return EFI_SUCCESS;
Fail:
  I->Open=TRUE;I->LastStatus=S;
  if(PianoGeniI2cClose(I)!=EFI_SUCCESS)I->Broken=TRUE;
  return S;
}

STATIC BOOLEAN Late(PIANO_GENI_I2C *I,UINT64 Deadline){return I->Io.NowUs(I->Io.Context)>=Deadline;}

// Cancel, then abort, only a command this master started. No bus clear or
// secondary sequencer reset is attempted; a stuck engine marks Broken
STATIC EFI_STATUS Recover(PIANO_GENI_I2C *I) {
  UINT32 Status=0,Irq=0,Fifo=0,Word=0;EFI_STATUS S;
  for(UINTN Phase=0;Phase<3;++Phase) {
    if((S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS)return S;
    if(!(Status&M_ACTIVE))break;
    if(Phase==2){I->Broken=TRUE;return EFI_DEVICE_ERROR;}
    if((S=Wr(I,GENI_M_CMD_CTRL,Phase==0?CMD_CANCEL:CMD_ABORT))!=EFI_SUCCESS)return S;
    UINT64 Deadline=I->Io.NowUs(I->Io.Context)+RECOVER_US;
    for(;;) {
      if((S=Rd(I,GENI_M_IRQ_STATUS,&Irq))!=EFI_SUCCESS || (S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS)return S;
      if((Irq&(Phase==0?IRQ_CANCEL:IRQ_ABORT)) || !(Status&M_ACTIVE) || Late(I,Deadline))break;
    }
  }
  if((S=Wr(I,GENI_TX_WATERMARK,0))!=EFI_SUCCESS)return S;
  for(UINT32 N=0;N<2*I->RxDepth+2;++N) {
    if((S=Rd(I,GENI_RX_FIFO_STAT,&Fifo))!=EFI_SUCCESS)return S;
    if(!(Fifo&RX_WORDS))break;
    if((S=Rd(I,GENI_RX_FIFO,&Word))!=EFI_SUCCESS)return S;
  }
  if((S=Rd(I,GENI_RX_FIFO_STAT,&Fifo))!=EFI_SUCCESS)return S;
  UINT32 Tx=0;if((S=Rd(I,GENI_TX_FIFO_STAT,&Tx))!=EFI_SUCCESS)return S;
  if((Fifo&RX_WORDS) || (Tx&TX_WORDS)){I->Broken=TRUE;return EFI_DEVICE_ERROR;}
  return Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32);
}

// I2C_STOP_ON_BUS from i2c-qcom-geni.c: ends a bus left held after a
// STOP_STRETCH write whose repeated-start read failed
STATIC EFI_STATUS StopOnBus(PIANO_GENI_I2C *I) {
  UINT32 Irq=0;EFI_STATUS S;
  if((S=Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32))!=EFI_SUCCESS || (S=Wr(I,GENI_M_CMD0,OP_STOP_ON_BUS<<27))!=EFI_SUCCESS)return S;
  UINT64 Deadline=I->Io.NowUs(I->Io.Context)+RECOVER_US;
  for(;;) {
    if((S=Rd(I,GENI_M_IRQ_STATUS,&Irq))!=EFI_SUCCESS)return S;
    if(Irq&IRQ_DONE)return Wr(I,GENI_M_IRQ_CLEAR,Irq);
    if(Irq&(IRQ_ERRORS|IRQ_CANCEL|IRQ_ABORT) || Late(I,Deadline))return EFI_DEVICE_ERROR;
  }
}

STATIC EFI_STATUS WritePhase(PIANO_GENI_I2C *I,UINT8 Address,CONST UINT8 *Data,UINTN Bytes,BOOLEAN Stretch,UINT64 Deadline) {
  UINTN Sent=0;UINT32 Irq=0,Tx=0;EFI_STATUS S;
  if((S=Wr(I,I2C_TX_TRANS_LEN,(UINT32)Bytes))!=EFI_SUCCESS ||
     (S=Wr(I,GENI_M_CMD0,(OP_WRITE<<27)|((UINT32)Address<<9)|(Stretch?STOP_STRETCH:0)))!=EFI_SUCCESS ||
     (S=Wr(I,GENI_TX_WATERMARK,1))!=EFI_SUCCESS)return S;
  for(;;) {
    if(Late(I,Deadline))return EFI_TIMEOUT;
    if((S=Rd(I,GENI_M_IRQ_STATUS,&Irq))!=EFI_SUCCESS)return S;
    I->LastIrq=Irq;
    if(Irq&(IRQ_ERRORS|IRQ_CANCEL|IRQ_ABORT))return (Irq&IRQ_NACK)?EFI_NO_RESPONSE:EFI_DEVICE_ERROR;
    if(Sent<Bytes) {
      if((S=Rd(I,GENI_TX_FIFO_STAT,&Tx))!=EFI_SUCCESS)return S;
      UINT32 Used=Tx&TX_WORDS;
      if(Used>I->TxDepth)return EFI_DEVICE_ERROR;
      for(UINT32 Free=I->TxDepth-Used;Free && Sent<Bytes;--Free) {
        UINT32 Word=0;
        for(UINT32 B=0;B<4 && Sent<Bytes;++B)Word|=(UINT32)Data[Sent++]<<(8*B);
        if((S=Wr(I,GENI_TX_FIFO,Word))!=EFI_SUCCESS)return S;
      }
      if(Sent==Bytes && (S=Wr(I,GENI_TX_WATERMARK,0))!=EFI_SUCCESS)return S;
    }
    if(Irq&IRQ_DONE) {
      if((S=Wr(I,GENI_M_IRQ_CLEAR,Irq))!=EFI_SUCCESS)return S;
      return Sent==Bytes?EFI_SUCCESS:EFI_DEVICE_ERROR;
    }
    if(Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST) && (S=Wr(I,GENI_M_IRQ_CLEAR,Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST)))!=EFI_SUCCESS)return S;
  }
}

STATIC EFI_STATUS Drain(PIANO_GENI_I2C *I,UINT8 *Data,UINTN Bytes,UINTN *Received) {
  UINT32 Fifo=0,Word=0;EFI_STATUS S=Rd(I,GENI_RX_FIFO_STAT,&Fifo);if(S!=EFI_SUCCESS)return S;
  UINT32 Words=Fifo&RX_WORDS;if(Words>I->RxDepth)return EFI_DEVICE_ERROR;
  for(UINT32 N=0;N<Words;++N) {
    if((S=Rd(I,GENI_RX_FIFO,&Word))!=EFI_SUCCESS)return S;
    if(*Received>=Bytes)return EFI_BUFFER_TOO_SMALL; // more data than requested
    for(UINT32 B=0;B<4 && *Received<Bytes;++B)Data[(*Received)++]=(UINT8)(Word>>(8*B));
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadPhase(PIANO_GENI_I2C *I,UINT8 Address,UINT8 *Data,UINTN Bytes,UINT64 Deadline) {
  UINTN Received=0;UINT32 Irq=0;EFI_STATUS S;
  if((S=Wr(I,I2C_RX_TRANS_LEN,(UINT32)Bytes))!=EFI_SUCCESS ||
     (S=Wr(I,GENI_M_CMD0,(OP_READ<<27)|((UINT32)Address<<9)))!=EFI_SUCCESS)return S;
  for(;;) {
    if(Late(I,Deadline))return EFI_TIMEOUT;
    if((S=Rd(I,GENI_M_IRQ_STATUS,&Irq))!=EFI_SUCCESS)return S;
    I->LastIrq=Irq;
    if(Irq&(IRQ_ERRORS|IRQ_CANCEL|IRQ_ABORT))return (Irq&IRQ_NACK)?EFI_NO_RESPONSE:EFI_DEVICE_ERROR;
    if((S=Drain(I,Data,Bytes,&Received))!=EFI_SUCCESS)return S;
    if(Irq&IRQ_DONE) {
      // Data words can still sit in the FIFO when DONE is raised
      if((S=Drain(I,Data,Bytes,&Received))!=EFI_SUCCESS)return S;
      if((S=Wr(I,GENI_M_IRQ_CLEAR,Irq))!=EFI_SUCCESS)return S;
      return Received==Bytes?EFI_SUCCESS:EFI_END_OF_FILE;
    }
    if(Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST) && (S=Wr(I,GENI_M_IRQ_CLEAR,Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST)))!=EFI_SUCCESS)return S;
  }
}

EFI_STATUS PianoGeniI2cTransfer(PIANO_GENI_I2C *I,UINT8 Address,CONST UINT8 *Write,UINTN WriteBytes,
                                UINT8 *Read,UINTN ReadBytes,UINT32 TimeoutUs) {
  if(I==NULL || Address>0x7F || (!WriteBytes && !ReadBytes) || WriteBytes>MAX_PHASE || ReadBytes>MAX_PHASE ||
     (WriteBytes && Write==NULL) || (ReadBytes && Read==NULL) || !TimeoutUs || TimeoutUs>1000000U)return EFI_INVALID_PARAMETER;
  if(!I->Open || I->Broken)return EFI_NOT_READY;
  UINT32 Status=0,Ios=0;EFI_STATUS S;BOOLEAN Started=FALSE,Stretched=FALSE;
  ++I->Transfers;
  if((S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS || (S=Rd(I,GENI_IOS,&Ios))!=EFI_SUCCESS)goto Done;
  I->LastGeniStatus=Status;I->LastIos=Ios;
  // Never start on an active engine or a bus another master holds low; such
  // a command is not ours, so it is neither cancelled nor drained below
  if(Status&(M_ACTIVE|S_ACTIVE) || (Ios&3)!=3){S=EFI_NOT_READY;goto Done;}
  Started=TRUE;
  if((S=Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32))!=EFI_SUCCESS)goto Done;
  UINT64 Deadline=I->Io.NowUs(I->Io.Context)+TimeoutUs;
  if(WriteBytes){S=WritePhase(I,Address,Write,WriteBytes,ReadBytes!=0,Deadline);Stretched=S==EFI_SUCCESS && ReadBytes;}
  if(S==EFI_SUCCESS && ReadBytes)S=ReadPhase(I,Address,Read,ReadBytes,Deadline);
Done:
  if(S!=EFI_SUCCESS) {
    ++I->Failures;
    if(Started && Recover(I)!=EFI_SUCCESS)I->Broken=TRUE;
    if(Stretched && !I->Broken && StopOnBus(I)!=EFI_SUCCESS && Recover(I)!=EFI_SUCCESS)I->Broken=TRUE;
    if(Read!=NULL)for(UINTN N=0;N<ReadBytes;++N)((volatile UINT8 *)Read)[N]=0;
  }
  return I->LastStatus=S;
}

EFI_STATUS PianoGeniI2cClose(PIANO_GENI_I2C *I) {
  if(I==NULL)return EFI_INVALID_PARAMETER;
  // A closed handle keeps the result of its last restore (also the one made
  // by a failed Open), so a later Close never reports a clean SE by default
  if(!I->Open)return I->CloseStatus==EFI_NOT_STARTED?EFI_SUCCESS:I->CloseStatus;
  EFI_STATUS Result=Recover(I);
  // Never reprogram clock, DMA mode or packing under a command that cancel
  // and abort could not stop
  if(Result!=EFI_SUCCESS){I->Broken=TRUE;I->Open=FALSE;return I->CloseStatus=Result;}
  for(UINT32 N=I->SavedCount;N>0;--N) {
    UINT32 Back=0;EFI_STATUS S=Wr(I,I->SavedOffset[N-1],I->SavedValue[N-1]);
    if(S==EFI_SUCCESS)S=Rd(I,I->SavedOffset[N-1],&Back);
    if(S==EFI_SUCCESS && Back!=I->SavedValue[N-1])S=EFI_DEVICE_ERROR;
    if(S!=EFI_SUCCESS && Result==EFI_SUCCESS)Result=S;
  }
  I->Open=FALSE;I->SavedCount=0;
  return I->CloseStatus=Result;
}
