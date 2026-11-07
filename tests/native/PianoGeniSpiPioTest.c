// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host model of a GENI SPI serial engine driving the real PianoGeniSpiPio.c.
// The shifter moves one TX word per status read and stalls while RX is full,
// so FIFO flow control and full-duplex byte alignment are exercised
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "PianoGeniSpiPio.h"

#define REGS (PIANO_GENI_SE6_BYTES/4)
#define DEPTH 16U
static UINT32 Reg[REGS],Original[REGS];
static UINT64 Clock;
static unsigned Writes,Cancels;
static int Op,Stuck,NoCancel;
static UINT32 Words,Shifted,TxUsed,RxFifo[DEPTH],RxCount,TxFifo[DEPTH];
static UINT8 Sent[0x10000];static UINT32 SentBytes,Length;

static UINT32 *R(UINT32 Offset){assert(Offset<PIANO_GENI_SE6_BYTES && !(Offset&3));return &Reg[Offset/4];}
static void Shift(void){
  if(!(*R(0x40)&BIT0) || Stuck)return;
  if(TxUsed && (Op==1 || RxCount<DEPTH)) {
    UINT32 Word=TxFifo[0];memmove(TxFifo,TxFifo+1,--TxUsed*4);
    for(UINT32 B=0;B<4 && SentBytes<Length;++B)Sent[SentBytes++]=(UINT8)(Word>>(8*B));
    if(Op==7)RxFifo[RxCount++]=Word^0xA5A5A5A5U;
    ++Shifted;
  }
  if(Shifted==Words){*R(0x40)&=~BIT0;*R(0x610)|=BIT0;}
}
static EFI_STATUS EFIAPI Read32(VOID *Context,UINT32 Offset,UINT32 *Value){
  assert(Context==(VOID *)Reg);Clock+=2;
  switch(Offset){
  case 0x800:Shift();*Value=TxUsed;return EFI_SUCCESS;
  case 0x804:Shift();*Value=RxCount;return EFI_SUCCESS;
  case 0x780:assert(RxCount);*Value=RxFifo[0];memmove(RxFifo,RxFifo+1,--RxCount*4);return EFI_SUCCESS;
  case 0x610:Shift();*Value=*R(0x610);return EFI_SUCCESS;
  default:*Value=*R(Offset);return EFI_SUCCESS;
  }
}
static EFI_STATUS EFIAPI Write32(VOID *Context,UINT32 Offset,UINT32 Value){
  assert(Context==(VOID *)Reg);++Writes;
  switch(Offset){
  case 0x618:*R(0x610)&=~Value;return EFI_SUCCESS;
  case 0x600:
    assert(!(*R(0x40)&BIT0) && !(Value&BIT2));Op=(int)(Value>>27);assert(Op==1 || Op==7);
    Length=*R(0x26C);assert(Op==1 || *R(0x270)==Length);
    Words=(Length+3)/4;Shifted=0;SentBytes=0;*R(0x40)|=BIT0;return EFI_SUCCESS;
  case 0x700:assert((*R(0x40)&BIT0) && TxUsed<DEPTH);TxFifo[TxUsed++]=Value;return EFI_SUCCESS;
  case 0x604:if((Value&BIT2) && ++Cancels && !NoCancel){*R(0x40)&=~BIT0;*R(0x610)|=BIT4;TxUsed=RxCount=0;}return EFI_SUCCESS;
  default:*R(Offset)=Value;return EFI_SUCCESS;
  }
}
static UINT64 EFIAPI Now(VOID *Context){assert(Context==(VOID *)Reg);return Clock++;}
static void Reset(UINT32 InterfaceDisable,UINT32 Protocol){
  memset(Reg,0,sizeof(Reg));*R(0x68)=(Protocol<<8)|0x05;*R(0x64)=InterfaceDisable;
  *R(0xE24)=(32U<<24)|(DEPTH<<16);*R(0xE28)=(32U<<24)|(DEPTH<<16);
  *R(0x258)=1;*R(0x25C)=BIT1|BIT0;*R(0x224)=1;*R(0x48)=0x50;*R(0x614)=0x1234;*R(0x260)=0xDEAD;
  memcpy(Original,Reg,sizeof(Reg));Writes=Cancels=0;Stuck=NoCancel=0;TxUsed=RxCount=0;Clock=0;
}
static PIANO_GENI_PIO_IO Io={PIANO_GENI_SE2_BASE,PIANO_GENI_SE6_BYTES,(VOID *)Reg,Read32,Write32,Now};
static UINT8 Tx[0x10000],Rx[0x10000];

int main(void){
  PIANO_GENI_SPI S;
  // GSI-only or non-SPI engines are reported without any register write
  Reset(BIT0,1);assert(PianoGeniSpiOpen(&S,&Io,100000000,19200000)==EFI_UNSUPPORTED && Writes==0);
  Reset(0,3);assert(PianoGeniSpiOpen(&S,&Io,100000000,19200000)==EFI_UNSUPPORTED && Writes==0);
  Reset(0,1);*R(0x40)=BIT0;assert(PianoGeniSpiOpen(&S,&Io,100000000,19200000)==EFI_NOT_READY && Writes==0);
  // FIFO mode, mode 0, 8-bit words, no CS toggling, SCLK 100 MHz / 6
  Reset(0,1);assert(PianoGeniSpiOpen(&S,&Io,100000000,19200000)==EFI_SUCCESS && S.Divider==6);
  assert(*R(0x258)==0 && *R(0x224)==0 && *R(0x230)==0 && (*R(0x25C)&BIT1)==0 && (*R(0x25C)&BIT0));
  assert(*R(0x268)==4 && *R(0x260)==0x7F8FE && *R(0x264)==0xFFEFE && *R(0x48)==((6U<<4)|1) && S.TxDepth==DEPTH);
  // The largest firmware chunk goes out byte for byte
  for(UINT32 N=0;N<63U*1024U+1;++N)Tx[N]=(UINT8)(N*7+3);
  assert(PianoGeniSpiTransfer(&S,Tx,NULL,63U*1024U+1,1000000)==EFI_SUCCESS && SentBytes==63U*1024U+1 && !memcmp(Sent,Tx,SentBytes));
  // Full duplex keeps RX aligned with TX, including a partial last word
  assert(PianoGeniSpiTransfer(&S,Tx,Rx,5418,1000000)==EFI_SUCCESS);
  for(UINT32 N=0;N<5418;++N)assert(Rx[N]==(UINT8)(Tx[N]^0xA5));
  // A stuck command times out, is cancelled and leaves the engine usable
  Stuck=1;assert(PianoGeniSpiTransfer(&S,Tx,Rx,64,200)==EFI_TIMEOUT && Cancels==1 && !S.Broken && Rx[0]==0);
  Stuck=0;assert(PianoGeniSpiTransfer(&S,Tx,Rx,3,1000)==EFI_SUCCESS && Rx[2]==(UINT8)(Tx[2]^0xA5));
  assert(PianoGeniSpiTransfer(&S,Tx,NULL,0x10001,1000)==EFI_INVALID_PARAMETER);
  // Close restores every register Open programmed
  assert(PianoGeniSpiClose(&S)==EFI_SUCCESS && !S.Open);
  STATIC CONST UINT32 Volatile[]={0x40,0x600,0x610};
  for(UINT32 O=0;O<PIANO_GENI_SE6_BYTES;O+=4){
    BOOLEAN Skip=FALSE;for(UINT32 V=0;V<3;++V)Skip|=O==Volatile[V];
    if(!Skip && *R(O)!=Original[O/4]){printf("register %x %x != %x\n",O,*R(O),Original[O/4]);assert(0);}
  }
  assert(PianoGeniSpiTransfer(&S,Tx,NULL,4,1000)==EFI_NOT_READY);
  // A command another owner started is left alone; Close keeps the
  // configuration while a command cannot be stopped
  Reset(0,1);assert(PianoGeniSpiOpen(&S,&Io,100000000,19200000)==EFI_SUCCESS && *R(0xE1C)==0);
  *R(0x40)|=BIT0;Words=Shifted+1;unsigned Before=Cancels;assert(PianoGeniSpiTransfer(&S,Tx,NULL,4,1000)==EFI_NOT_READY && Cancels==Before);
  NoCancel=1;assert(PianoGeniSpiClose(&S)==EFI_DEVICE_ERROR && *R(0x48)==((6U<<4)|1) && !S.Open && S.Broken);
  assert(PianoGeniSpiClose(&S)==EFI_DEVICE_ERROR);
  printf("GENI SPI PIO: open/refuse, 64513-byte TX, 5418-byte full duplex, timeout recovery, restore\n");
  return 0;
}
