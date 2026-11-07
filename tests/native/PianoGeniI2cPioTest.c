// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host model of a GENI I2C serial engine driving the real PianoGeniI2cPio.c:
// FIFO words, command completion, NACK, a stuck command and register restore
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "PianoGeniI2cPio.h"

#define REGS (PIANO_GENI_SE6_BYTES/4)
static UINT32 Reg[REGS],Original[REGS];
static UINT64 Clock;
static unsigned Writes,Cancels,Aborts;
// Slave model at 0x4C: records writes, answers reads from Answer
static UINT8 Received[1024],Answer[128];static UINTN ReceivedBytes,AnswerBytes,AnswerPos;
static int Nack,NackRead,Stuck,CancelWorks,CommandOp,CommandStretch,WriteLeft,ReadLeft;static unsigned Stops;
static UINT32 RxFifo[64];static unsigned RxCount;

static UINT32 *R(UINT32 Offset){assert(Offset<PIANO_GENI_SE6_BYTES && !(Offset&3));return &Reg[Offset/4];}
static void Finish(void){*R(0x40)&=~BIT0;*R(0x610)|=BIT0;}
static void FillRx(void){
  while(ReadLeft>0 && RxCount<16){
    UINT32 Word=0;for(int B=0;B<4 && ReadLeft>0;++B,--ReadLeft)Word|=(UINT32)Answer[AnswerPos++%AnswerBytes]<<(8*B);
    RxFifo[RxCount++]=Word;
  }
  if(!ReadLeft && CommandOp==2 && !Stuck)Finish();
}
static EFI_STATUS EFIAPI Read32(VOID *Context,UINT32 Offset,UINT32 *Value){
  assert(Context==(VOID *)Reg);
  if(Offset==0x804){*Value=RxCount;return EFI_SUCCESS;}
  if(Offset==0x780){assert(RxCount);*Value=RxFifo[0];memmove(RxFifo,RxFifo+1,--RxCount*4);FillRx();return EFI_SUCCESS;}
  if(Offset==0x800){*Value=0;return EFI_SUCCESS;}
  Clock+=3;*Value=*R(Offset);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write32(VOID *Context,UINT32 Offset,UINT32 Value){
  assert(Context==(VOID *)Reg);++Writes;
  switch(Offset){
  case 0x618:*R(0x610)&=~Value;return EFI_SUCCESS;
  case 0x600:
    assert(!(*R(0x40)&BIT0));CommandOp=(int)(Value>>27);CommandStretch=(Value&BIT2)!=0;
    if(CommandOp==7){++Stops;*R(0x40)|=BIT0;Finish();return EFI_SUCCESS;}
    assert(((Value>>9)&0x7F)==0x4C || Nack);
    *R(0x600)=Value;*R(0x40)|=BIT0;
    if(Nack || (NackRead && CommandOp==2)){*R(0x610)|=BIT10|BIT3;if(!Stuck)Finish();return EFI_SUCCESS;}
    if(CommandOp==1){WriteLeft=(int)*R(0x26C);if(!WriteLeft && !Stuck)Finish();}
    else {ReadLeft=(int)*R(0x270);AnswerPos=0;FillRx();}
    return EFI_SUCCESS;
  case 0x700:
    assert(CommandOp==1 && WriteLeft>0);
    for(int B=0;B<4 && WriteLeft>0;++B,--WriteLeft)Received[ReceivedBytes++]=(UINT8)(Value>>(8*B));
    if(!WriteLeft && !Stuck)Finish();
    return EFI_SUCCESS;
  case 0x604:
    if(Value&BIT2){++Cancels;if(CancelWorks){*R(0x40)&=~BIT0;*R(0x610)|=BIT4;}}
    if(Value&BIT1){++Aborts;if(CancelWorks>1){*R(0x40)&=~BIT0;*R(0x610)|=BIT5;}}
    return EFI_SUCCESS;
  default:*R(Offset)=Value;return EFI_SUCCESS;
  }
}
static UINT64 EFIAPI Now(VOID *Context){assert(Context==(VOID *)Reg);return Clock++;}
static void Reset(UINT32 InterfaceDisable,UINT32 Protocol){
  memset(Reg,0,sizeof(Reg));*R(0x68)=(Protocol<<8)|0x04;*R(0x64)=InterfaceDisable;
  *R(0xE24)=(32U<<24)|(16U<<16);*R(0xE28)=(32U<<24)|(16U<<16);*R(0x908)=3;
  *R(0x258)=1;*R(0x614)=0x1234;*R(0x260)=0xDEAD;*R(0x48)=0x70;*R(0x24)=0;
  memcpy(Original,Reg,sizeof(Reg));Writes=Cancels=Aborts=0;ReceivedBytes=0;Nack=Stuck=0;CancelWorks=1;RxCount=0;Clock=0;
}
static PIANO_GENI_PIO_IO Io={PIANO_GENI_SE6_BASE,PIANO_GENI_SE6_BYTES,(VOID *)Reg,Read32,Write32,Now};

int main(void){
  PIANO_GENI_I2C I;UINT8 Data[600],In[68];
  // GSI-only and non-I2C engines are reported without any register write
  Reset(BIT0,3);assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_UNSUPPORTED && Writes==0);
  Reset(0,2);assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_UNSUPPORTED && Writes==0);
  Reset(0,3);*R(0x40)=BIT0;assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_NOT_READY && Writes==0);
  // FIFO mode, Linux 8-bit packing and the 19.2 MHz 1 MHz timing
  Reset(0,3);assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_SUCCESS);
  assert(*R(0x258)==0 && *R(0x260)==0x7F8FE && *R(0x264)==0xFFEFE && *R(0x284)==0x7F8FE && *R(0x288)==0xFFEFE);
  assert(*R(0x48)==((1U<<4)|1) && *R(0x278)==((2U<<20)|(8U<<10)|18) && *R(0x24)==0x7F && I.TxDepth==16);
  // A 513-byte bootrom packet goes out byte for byte
  for(unsigned N=0;N<513;++N)Data[N]=(UINT8)(N*7+1);
  assert(PianoGeniI2cTransfer(&I,0x4C,Data,513,NULL,0,20000)==EFI_SUCCESS);
  assert(ReceivedBytes==513 && !memcmp(Received,Data,513) && CommandOp==1 && !CommandStretch);
  // Register write, repeated start, 68-byte report read
  for(unsigned N=0;N<68;++N){Answer[N]=(UINT8)(0x57+N);}
  AnswerBytes=68;ReceivedBytes=0;
  UINT8 Register=0x4C;
  assert(PianoGeniI2cTransfer(&I,0x4C,&Register,1,In,68,4000)==EFI_SUCCESS);
  assert(ReceivedBytes==1 && Received[0]==0x4C && !memcmp(In,Answer,68) && CommandOp==2);
  // NACK: reported, engine recovered, next transfer works
  Nack=1;assert(PianoGeniI2cTransfer(&I,0x4C,Data,2,NULL,0,5000)==EFI_NO_RESPONSE && !I.Broken);
  Nack=0;ReceivedBytes=0;assert(PianoGeniI2cTransfer(&I,0x4C,Data,2,NULL,0,5000)==EFI_SUCCESS && ReceivedBytes==2);
  // A repeated-start read that fails after the stretched write ends with a bus STOP
  NackRead=1;assert(PianoGeniI2cTransfer(&I,0x4C,&Register,1,In,68,5000)==EFI_NO_RESPONSE && Stops==1 && !I.Broken);
  NackRead=0;assert(PianoGeniI2cTransfer(&I,0x4C,Data,2,NULL,0,5000)==EFI_SUCCESS && Stops==1);
  // A command that never completes times out and is cancelled
  Stuck=1;assert(PianoGeniI2cTransfer(&I,0x4C,Data,8,NULL,0,500)==EFI_TIMEOUT && Cancels==1 && !I.Broken);
  // If neither cancel nor abort stops it, the engine is marked broken
  CancelWorks=0;assert(PianoGeniI2cTransfer(&I,0x4C,Data,8,NULL,0,500)==EFI_TIMEOUT && Aborts==1 && I.Broken);
  assert(PianoGeniI2cTransfer(&I,0x4C,Data,8,NULL,0,500)==EFI_NOT_READY);
  // Close restores every register Open changed
  CancelWorks=2;*R(0x40)&=~BIT0;Stuck=0;
  assert(PianoGeniI2cClose(&I)==EFI_SUCCESS);
  for(unsigned N=0;N<REGS;++N){
    UINT32 Offset=N*4;
    if(Offset==0x40||Offset==0x600||Offset==0x604||Offset==0x610||Offset==0x80C)continue;
    if(Reg[N]!=Original[N]){printf("offset %x %x!=%x\n",Offset,Reg[N],Original[N]);assert(0);}
  }
  // Bad arguments never reach the engine
  Reset(0,3);assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_SUCCESS);
  assert(PianoGeniI2cTransfer(&I,0x80,Data,1,NULL,0,500)==EFI_INVALID_PARAMETER);
  assert(PianoGeniI2cTransfer(&I,0x4C,NULL,1,NULL,0,500)==EFI_INVALID_PARAMETER);
  assert(PianoGeniI2cOpen(&I,&Io,19200000,3400000)==EFI_UNSUPPORTED);
  // Close leaves the configuration alone while a command cannot be stopped
  Reset(0,3);assert(PianoGeniI2cOpen(&I,&Io,19200000,1000000)==EFI_SUCCESS);
  Stuck=1;CancelWorks=0;assert(PianoGeniI2cTransfer(&I,0x4C,Data,8,NULL,0,500)==EFI_TIMEOUT && I.Broken);
  UINT32 Clock48=*R(0x48);assert(Clock48!=Original[0x48/4]);
  assert(PianoGeniI2cClose(&I)==EFI_DEVICE_ERROR && *R(0x48)==Clock48 && *R(0x258)==0 && !I.Open);
  assert(PianoGeniI2cClose(&I)==EFI_DEVICE_ERROR);
  puts("Piano GENI I2C FIFO engine: open/identity/packing/write513/write-read/NACK/timeout/abort/restore PASS");
  return 0;
}
