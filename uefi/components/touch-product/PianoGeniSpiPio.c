// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offsets, opcodes and FIFO init order follow Linux 7.3 spi-geni-qcom.c
// (spi_geni_init, spi_setup_word_len, setup_se_xfer); written independently
#include "PianoGeniSpiPio.h"

#define GENI_OUTPUT_CTRL   0x024U
#define GENI_CGC_CTRL      0x028U
#define GENI_STATUS        0x040U
#define GENI_M_CLK_CFG     0x048U
#define GENI_IF_DISABLE_RO 0x064U
#define GENI_FW_REVISION   0x068U
#define GENI_CLK_SEL       0x07CU
#define SPI_CPHA           0x224U
#define SPI_LOOPBACK       0x22CU
#define SPI_CPOL           0x230U
#define SPI_DEMUX_INV      0x24CU
#define SPI_DEMUX_SEL      0x250U
#define GENI_BYTE_GRAN     0x254U
#define GENI_DMA_MODE_EN   0x258U
#define SPI_TRANS_CFG      0x25CU
#define GENI_TX_PACKING0   0x260U
#define GENI_TX_PACKING1   0x264U
#define SPI_WORD_LEN       0x268U
#define SPI_TX_TRANS_LEN   0x26CU
#define SPI_RX_TRANS_LEN   0x270U
#define GENI_RX_PACKING0   0x284U
#define GENI_RX_PACKING1   0x288U
#define GENI_M_CMD0        0x600U
#define GENI_M_CMD_CTRL    0x604U
#define GENI_M_IRQ_STATUS  0x610U
#define GENI_M_IRQ_EN      0x614U
#define GENI_M_IRQ_CLEAR   0x618U
#define GENI_TX_FIFO       0x700U
#define GENI_RX_FIFO       0x780U
#define GENI_TX_FIFO_STAT  0x800U
#define GENI_RX_FIFO_STAT  0x804U
#define GENI_TX_WATERMARK  0x80CU
#define GENI_RX_WATERMARK  0x810U
#define GENI_RX_RFR_WM     0x814U
#define GENI_GSI_EVENT_EN  0xE18U
#define GENI_HW_PARAM_0    0xE24U
#define GENI_HW_PARAM_1    0xE28U
#define GENI_SE_IRQ_EN     0xE1CU

#define M_ACTIVE      BIT0
#define S_ACTIVE      BIT12
#define IRQ_DONE      BIT0
#define IRQ_CANCEL    BIT4
#define IRQ_ABORT     BIT5
#define IRQ_RX_WM     BIT26
#define IRQ_RX_LAST   BIT27
#define IRQ_TX_WM     BIT30
#define IRQ_ERRORS    (BIT1|BIT2|BIT3|BIT24|BIT25|BIT28|BIT29)
#define IRQ_USED      (IRQ_DONE|IRQ_CANCEL|IRQ_ABORT|IRQ_ERRORS|IRQ_RX_WM|IRQ_RX_LAST|IRQ_TX_WM)
#define CMD_CANCEL    BIT2
#define CMD_ABORT     BIT1
#define OP_TX_ONLY    1U
#define OP_TX_RX      7U
#define CS_TOGGLE     BIT1
#define TX_WORDS      0x0FFFFFFFU
#define RX_WORDS      0x01FFFFFFU
#define PACKING0      0x0007F8FEU   // 8-bit words, MSB first, 4 per FIFO word
#define PACKING1      0x000FFEFEU
#define MAX_BYTES     0x10000U
#define RECOVER_US    2000U

STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS Rd(PIANO_GENI_SPI *I,UINT32 Offset,UINT32 *Value){return Exact(I->Io.Read32(I->Io.Context,Offset,Value));}
STATIC EFI_STATUS Wr(PIANO_GENI_SPI *I,UINT32 Offset,UINT32 Value){return Exact(I->Io.Write32(I->Io.Context,Offset,Value));}
STATIC BOOLEAN Late(PIANO_GENI_SPI *I,UINT64 Deadline){return I->Io.NowUs(I->Io.Context)>=Deadline;}

STATIC EFI_STATUS Program(PIANO_GENI_SPI *I,UINT32 Offset,UINT32 Value) {
  UINT32 Old=0,Back=0;EFI_STATUS S=Rd(I,Offset,&Old);if(S!=EFI_SUCCESS)return S;
  BOOLEAN Known=FALSE;
  for(UINT32 N=0;N<I->SavedCount;++N)if(I->SavedOffset[N]==Offset)Known=TRUE;
  if(!Known) {
    if(I->SavedCount==PIANO_GENI_SAVED_REGISTERS)return EFI_OUT_OF_RESOURCES;
    I->SavedOffset[I->SavedCount]=Offset;I->SavedValue[I->SavedCount]=Old;++I->SavedCount;
  }
  if(Old==Value)return EFI_SUCCESS;
  if((S=Wr(I,Offset,Value))!=EFI_SUCCESS || (S=Rd(I,Offset,&Back))!=EFI_SUCCESS)return S;
  return Back==Value?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS Recover(PIANO_GENI_SPI *I) {
  UINT32 Status=0,Irq=0,Fifo=0,Word=0,Tx=0;EFI_STATUS S;
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
  if((S=Rd(I,GENI_RX_FIFO_STAT,&Fifo))!=EFI_SUCCESS || (S=Rd(I,GENI_TX_FIFO_STAT,&Tx))!=EFI_SUCCESS)return S;
  if((Fifo&RX_WORDS) || (Tx&TX_WORDS)){I->Broken=TRUE;return EFI_DEVICE_ERROR;}
  return Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32);
}

EFI_STATUS PianoGeniSpiOpen(PIANO_GENI_SPI *I,CONST PIANO_GENI_PIO_IO *Io,UINT32 SourceHz,UINT32 MaxHz) {
  if(I==NULL || Io==NULL || Io->Base!=PIANO_GENI_SE2_BASE || Io->Bytes!=PIANO_GENI_SE6_BYTES ||
     !Io->Read32 || !Io->Write32 || !Io->NowUs || SourceHz<1000000U || MaxHz<1000000U)return EFI_INVALID_PARAMETER;
  for(UINTN N=0;N<sizeof(*I);++N)((volatile UINT8 *)I)[N]=0;
  I->Io=*Io;I->SourceHz=SourceHz;I->LastStatus=EFI_NOT_STARTED;I->CloseStatus=EFI_NOT_STARTED;
  I->Divider=(SourceHz+MaxHz-1)/MaxHz;
  if(!I->Divider || I->Divider>0xFFFU)return EFI_UNSUPPORTED;
  UINT32 Status=0,Hw0=0,Hw1=0,Value=0;EFI_STATUS S;
  if((S=Rd(I,GENI_FW_REVISION,&I->Firmware))!=EFI_SUCCESS || (S=Rd(I,GENI_IF_DISABLE_RO,&I->InterfaceDisable))!=EFI_SUCCESS ||
     (S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS || (S=Rd(I,GENI_HW_PARAM_0,&Hw0))!=EFI_SUCCESS ||
     (S=Rd(I,GENI_HW_PARAM_1,&Hw1))!=EFI_SUCCESS)return S;
  if(((I->Firmware>>8)&0xFFU)!=PIANO_GENI_PROTOCOL_SPI || (I->InterfaceDisable&BIT0))return EFI_UNSUPPORTED;
  if(Status&(M_ACTIVE|S_ACTIVE))return EFI_NOT_READY;
  I->TxDepth=(Hw0>>16)&0xFFU;I->RxDepth=(Hw1>>16)&0xFFU;
  if(((Hw0>>24)&0x3FU)!=32 || ((Hw1>>24)&0x3FU)!=32 || I->TxDepth<4 || I->RxDepth<4)return EFI_UNSUPPORTED;
  if((S=Rd(I,GENI_OUTPUT_CTRL,&Value))!=EFI_SUCCESS)return S;
  if((S=Program(I,GENI_OUTPUT_CTRL,Value|0x7FU))!=EFI_SUCCESS)goto Fail;
  if((S=Rd(I,GENI_CGC_CTRL,&Value))!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_CGC_CTRL,Value|0x7FU))!=EFI_SUCCESS)goto Fail;
  if((S=Rd(I,GENI_DMA_MODE_EN,&Value))!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_DMA_MODE_EN,Value&~BIT0))!=EFI_SUCCESS)goto Fail;
  if((S=Rd(I,SPI_TRANS_CFG,&Value))!=EFI_SUCCESS)goto Fail;
  // Polled only: no M/S/DMA interrupt is routed to the GIC line
  if((S=Program(I,GENI_GSI_EVENT_EN,0))!=EFI_SUCCESS || (S=Program(I,GENI_SE_IRQ_EN,0))!=EFI_SUCCESS ||
     (S=Program(I,SPI_LOOPBACK,0))!=EFI_SUCCESS ||
     (S=Program(I,SPI_DEMUX_SEL,0))!=EFI_SUCCESS || (S=Program(I,SPI_CPHA,0))!=EFI_SUCCESS ||
     (S=Program(I,SPI_CPOL,0))!=EFI_SUCCESS || (S=Program(I,SPI_DEMUX_INV,0))!=EFI_SUCCESS ||
     (S=Program(I,SPI_TRANS_CFG,Value&~CS_TOGGLE))!=EFI_SUCCESS ||
     (S=Program(I,GENI_TX_PACKING0,PACKING0))!=EFI_SUCCESS || (S=Program(I,GENI_TX_PACKING1,PACKING1))!=EFI_SUCCESS ||
     (S=Program(I,GENI_RX_PACKING0,PACKING0))!=EFI_SUCCESS || (S=Program(I,GENI_RX_PACKING1,PACKING1))!=EFI_SUCCESS ||
     (S=Program(I,GENI_BYTE_GRAN,0))!=EFI_SUCCESS || (S=Program(I,SPI_WORD_LEN,8-4))!=EFI_SUCCESS ||
     (S=Program(I,GENI_CLK_SEL,0))!=EFI_SUCCESS || (S=Program(I,GENI_M_CLK_CFG,(I->Divider<<4)|BIT0))!=EFI_SUCCESS ||
     (S=Program(I,GENI_TX_WATERMARK,0))!=EFI_SUCCESS || (S=Program(I,GENI_RX_WATERMARK,I->RxDepth-3))!=EFI_SUCCESS ||
     (S=Program(I,GENI_RX_RFR_WM,I->RxDepth-2))!=EFI_SUCCESS ||
     (S=Program(I,SPI_TX_TRANS_LEN,0))!=EFI_SUCCESS || (S=Program(I,SPI_RX_TRANS_LEN,0))!=EFI_SUCCESS)goto Fail;
  if((S=Rd(I,GENI_M_IRQ_EN,&Value))!=EFI_SUCCESS)goto Fail;
  if((S=Program(I,GENI_M_IRQ_EN,Value|IRQ_USED))!=EFI_SUCCESS || (S=Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32))!=EFI_SUCCESS)goto Fail;
  I->Open=TRUE;I->LastStatus=EFI_SUCCESS;return EFI_SUCCESS;
Fail:
  I->Open=TRUE;I->LastStatus=S;
  if(PianoGeniSpiClose(I)!=EFI_SUCCESS)I->Broken=TRUE;
  return S;
}

EFI_STATUS PianoGeniSpiTransfer(PIANO_GENI_SPI *I,CONST UINT8 *Tx,UINT8 *Rx,UINTN Bytes,UINT32 TimeoutUs) {
  if(I==NULL || Tx==NULL || !Bytes || Bytes>MAX_BYTES || !TimeoutUs || TimeoutUs>1000000U)return EFI_INVALID_PARAMETER;
  if(!I->Open || I->Broken)return EFI_NOT_READY;
  UINT32 Status=0,Irq=0,Fifo=0,Word=0;UINTN Sent=0,Received=0;EFI_STATUS S;BOOLEAN Started=FALSE;
  ++I->Transfers;
  if((S=Rd(I,GENI_STATUS,&Status))!=EFI_SUCCESS)goto Done;
  // A command already running is not ours, so it is neither cancelled nor drained
  if(Status&(M_ACTIVE|S_ACTIVE)){S=EFI_NOT_READY;goto Done;}
  Started=TRUE;
  if((S=Wr(I,GENI_M_IRQ_CLEAR,MAX_UINT32))!=EFI_SUCCESS ||
     (S=Wr(I,SPI_TX_TRANS_LEN,(UINT32)Bytes))!=EFI_SUCCESS ||
     (Rx!=NULL && (S=Wr(I,SPI_RX_TRANS_LEN,(UINT32)Bytes))!=EFI_SUCCESS))goto Done;
  // No FRAGMENTATION, so CS drops at the end of the command
  if((S=Wr(I,GENI_M_CMD0,(Rx!=NULL?OP_TX_RX:OP_TX_ONLY)<<27))!=EFI_SUCCESS ||
     (S=Wr(I,GENI_TX_WATERMARK,1))!=EFI_SUCCESS)goto Done;
  UINT64 Deadline=I->Io.NowUs(I->Io.Context)+TimeoutUs;
  for(;;) {
    if(Late(I,Deadline)){S=EFI_TIMEOUT;break;}
    if((S=Rd(I,GENI_M_IRQ_STATUS,&Irq))!=EFI_SUCCESS)break;
    I->LastIrq=Irq;
    if(Irq&(IRQ_ERRORS|IRQ_CANCEL|IRQ_ABORT)){S=EFI_DEVICE_ERROR;break;}
    if(Sent<Bytes) {
      UINT32 TxStat=0;if((S=Rd(I,GENI_TX_FIFO_STAT,&TxStat))!=EFI_SUCCESS)break;
      UINT32 Used=TxStat&TX_WORDS;if(Used>I->TxDepth){S=EFI_DEVICE_ERROR;break;}
      for(UINT32 Free=I->TxDepth-Used;Free && Sent<Bytes;--Free) {
        UINT32 Value=0;
        for(UINT32 B=0;B<4 && Sent<Bytes;++B)Value|=(UINT32)Tx[Sent++]<<(8*B);
        if((S=Wr(I,GENI_TX_FIFO,Value))!=EFI_SUCCESS)goto Done;
      }
      if(Sent==Bytes && (S=Wr(I,GENI_TX_WATERMARK,0))!=EFI_SUCCESS)break;
    }
    if(Rx!=NULL) {
      if((S=Rd(I,GENI_RX_FIFO_STAT,&Fifo))!=EFI_SUCCESS)break;
      UINT32 Words=Fifo&RX_WORDS;if(Words>I->RxDepth){S=EFI_DEVICE_ERROR;break;}
      for(UINT32 N=0;N<Words;++N) {
        if((S=Rd(I,GENI_RX_FIFO,&Word))!=EFI_SUCCESS)goto Done;
        if(Received>=Bytes){S=EFI_BUFFER_TOO_SMALL;goto Done;}
        for(UINT32 B=0;B<4 && Received<Bytes;++B)Rx[Received++]=(UINT8)(Word>>(8*B));
      }
    }
    if(Irq&IRQ_DONE) {
      if(Rx!=NULL && Received<Bytes) {
        // Words can still be in the FIFO when DONE is raised
        if((S=Rd(I,GENI_RX_FIFO_STAT,&Fifo))!=EFI_SUCCESS)break;
        UINT32 Words=Fifo&RX_WORDS;
        for(UINT32 N=0;N<Words && Received<Bytes;++N) {
          if((S=Rd(I,GENI_RX_FIFO,&Word))!=EFI_SUCCESS)goto Done;
          for(UINT32 B=0;B<4 && Received<Bytes;++B)Rx[Received++]=(UINT8)(Word>>(8*B));
        }
      }
      if((S=Wr(I,GENI_M_IRQ_CLEAR,Irq))!=EFI_SUCCESS)break;
      S=Sent==Bytes && (Rx==NULL || Received==Bytes)?EFI_SUCCESS:EFI_END_OF_FILE;
      break;
    }
    if(Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST) && (S=Wr(I,GENI_M_IRQ_CLEAR,Irq&(IRQ_TX_WM|IRQ_RX_WM|IRQ_RX_LAST)))!=EFI_SUCCESS)break;
  }
Done:
  if(S!=EFI_SUCCESS) {
    ++I->Failures;
    if(Started && Recover(I)!=EFI_SUCCESS)I->Broken=TRUE;
    if(Rx!=NULL)for(UINTN N=0;N<Bytes;++N)((volatile UINT8 *)Rx)[N]=0;
  }
  return I->LastStatus=S;
}

EFI_STATUS PianoGeniSpiClose(PIANO_GENI_SPI *I) {
  if(I==NULL)return EFI_INVALID_PARAMETER;
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
