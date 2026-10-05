// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independently written bounded fixed transaction, based on GENI register and
// I2C command facts. No Linux driver implementation or OEM function is called.
#include "PianoGeniI2cPio.h"

#define SIGNATURE SIGNATURE_32('P','G','I','P')
#define ACTIVE (BIT0|BIT12)
#define DONE BIT0
#define CANCEL BIT4
#define ABORT BIT5
#define RX_READY (BIT26|BIT27)
#define TX_READY BIT30
#define ERRORS (BIT1|BIT2|BIT3|BIT9|BIT10|BIT11|BIT12|BIT13|BIT14|BIT24|BIT25|BIT28|BIT29)
#define OWN_IRQ (DONE|CANCEL|ABORT|RX_READY|TX_READY|ERRORS)
#define TX_WORDS 0x0FFFFFFFU
#define RX_WORDS 0x01FFFFFFU
#define PACK0 0x0007F8FEU
#define PACK1 0x000FFEFEU

STATIC VOID Clear(VOID *Pointer,UINTN Bytes) {
  for(UINTN I=0;I<Bytes;++I)((volatile UINT8 *)Pointer)[I]=0;
}
EFI_STATUS PianoGeniI2cPioBuildRuntimePacket(PIANO_GENI_RUNTIME_PACKET *P) {
  if(P==NULL)return EFI_INVALID_PARAMETER;
  *P=(PIANO_GENI_RUNTIME_PACKET){1,0x4c,(1U<<27)|(0x4cU<<9)|BIT2,68,(2U<<27)|(0x4cU<<9)};
  return EFI_SUCCESS;
}
STATIC BOOLEAN ValidIo(CONST PIANO_GENI_PIO_IO *Io) {
  return Io!=NULL && Io->Base==PIANO_GENI_SE6_BASE && Io->Bytes==PIANO_GENI_SE6_BYTES && Io->Read32!=NULL;
}
EFI_STATUS PianoGeniI2cPioCapture(CONST PIANO_GENI_PIO_IO *Io,PIANO_GENI_PIO_SNAPSHOT *S) {
  if(!ValidIo(Io) || S==NULL)return EFI_INVALID_PARAMETER;
  Clear(S,sizeof(*S));
  STATIC CONST UINT32 Offset[]={0x68,0x64,0x40,0x258,0xe18,0xe1c,0x7c,0x48,0x278,0x254,
    0x260,0x264,0x284,0x288,0xe24,0xe28,0x908,0x610,0x640,0x614,0x604,0x634,0x800,0x804,0x80c,0x810,0x26c,0x270,0x600,0x630};
  UINT32 *Field[]={&S->Firmware,&S->InterfaceDisable,&S->Status,&S->DmaMode,&S->Events,&S->IrqRoute,
    &S->ClockSelect,&S->MasterClock,&S->SclCounters,&S->ByteGran,&S->TxPack[0],&S->TxPack[1],&S->RxPack[0],&S->RxPack[1],
    &S->HwTx,&S->HwRx,&S->IoLines,&S->MasterIrq,&S->SecondaryIrq,&S->MasterIrqEnable,&S->MasterControl,&S->SecondaryControl,
    &S->TxFifo,&S->RxFifo,&S->TxWatermark,&S->RxWatermark,&S->TxLength,&S->RxLength,&S->MasterCommand,&S->SecondaryCommand};
  STATIC_ASSERT(ARRAY_SIZE(Field)==ARRAY_SIZE(Offset),"snapshot/register count");
  for(UINTN I=0;I<ARRAY_SIZE(Offset);++I) {
    EFI_STATUS R=Io->Read32(Io->Context,Offset[I],Field[I]);
    if(R!=EFI_SUCCESS){Clear(S,sizeof(*S));return EFI_ERROR(R)?R:EFI_DEVICE_ERROR;}
  }
  return EFI_SUCCESS;
}
STATIC UINT32 Depth(UINT32 Hw){return (Hw>>16)&0xff;}
STATIC EFI_STATUS Configuration(CONST PIANO_GENI_PIO_CONTRACT *C,CONST PIANO_GENI_PIO_SNAPSHOT *S) {
  if(C==NULL || S==NULL)return EFI_INVALID_PARAMETER;
  if(C->ExplicitEnable!=TRUE || C->MappedDeviceMemoryVerified!=TRUE || C->ExclusiveOwnerVerified!=TRUE ||
     C->ExistingRuntimeVerified!=TRUE || C->ClockRateAndMuxVerified!=TRUE || C->SupplyAndGpioVerified!=TRUE ||
     C->FirmwareAndPackingVerified!=TRUE || C->BoundedCallbacksVerified!=TRUE)return EFI_NOT_READY;
  if(!C->Firmware || C->Firmware==MAX_UINT32 || ((C->Firmware>>8)&0xff)!=3 || S->Firmware!=C->Firmware ||
     S->ClockSelect!=C->ClockSelect || C->ClockSelect>7 || S->MasterClock!=C->MasterClock || !(C->MasterClock&BIT0) ||
     !C->SclCounters || S->SclCounters!=C->SclCounters)return EFI_COMPROMISED_DATA;
  if(S->InterfaceDisable&BIT0 || S->DmaMode || S->Events || S->IrqRoute ||
     S->ByteGran || S->TxPack[0]!=PACK0 || S->RxPack[0]!=PACK0 || S->TxPack[1]!=PACK1 || S->RxPack[1]!=PACK1 ||
     ((S->HwTx>>24)&0x3f)!=32 || ((S->HwRx>>24)&0x3f)!=32 || !Depth(S->HwTx) || !Depth(S->HwRx))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
EFI_STATUS PianoGeniI2cPioValidate(CONST PIANO_GENI_PIO_CONTRACT *C,CONST PIANO_GENI_PIO_SNAPSHOT *S) {
  EFI_STATUS R=Configuration(C,S);if(R!=EFI_SUCCESS)return R;
  if((S->Status&ACTIVE) || S->MasterControl || S->SecondaryControl || S->MasterIrq || S->SecondaryIrq ||
     (S->TxFifo&TX_WORDS) || (S->RxFifo&RX_WORDS) || (S->IoLines&3)!=3)return EFI_NOT_READY;
  return EFI_SUCCESS;
}
EFI_STATUS PianoGeniI2cPioInitialize(PIANO_GENI_I2C_PIO *P,CONST PIANO_GENI_PIO_IO *Io,CONST PIANO_GENI_PIO_CONTRACT *C) {
  if(P==NULL || Io==NULL || C==NULL)return EFI_INVALID_PARAMETER;
  if(P->Signature)return EFI_ALREADY_STARTED;
  if(!PIANO_GENI_I2C_PIO_EXPERIMENT)return EFI_UNSUPPORTED;
  if(!ValidIo(Io) || Io->Write32==NULL || Io->NowUs==NULL)return EFI_INVALID_PARAMETER;
  // Do not even touch registers until the caller has supplied all attestations.
  if(C->ExplicitEnable!=TRUE || C->MappedDeviceMemoryVerified!=TRUE || C->ExclusiveOwnerVerified!=TRUE ||
     C->ExistingRuntimeVerified!=TRUE || C->ClockRateAndMuxVerified!=TRUE || C->SupplyAndGpioVerified!=TRUE ||
     C->FirmwareAndPackingVerified!=TRUE || C->BoundedCallbacksVerified!=TRUE)return EFI_NOT_READY;
  PIANO_GENI_PIO_SNAPSHOT S;EFI_STATUS R=PianoGeniI2cPioCapture(Io,&S);
  if(R==EFI_SUCCESS)R=PianoGeniI2cPioValidate(C,&S);
  if(R!=EFI_SUCCESS)return R;
  Clear(P,sizeof(*P));P->Signature=SIGNATURE;P->Io=*Io;P->Contract=*C;P->Before=S;
  P->LastTime=Io->NowUs(Io->Context);P->Clean=TRUE;P->LastStatus=EFI_SUCCESS;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Budget(PIANO_GENI_I2C_PIO *P) {
  UINT64 Now=P->Io.NowUs(P->Io.Context);
  if(Now<P->LastTime){P->ClockBroken=TRUE;return EFI_COMPROMISED_DATA;}
  P->LastTime=Now;return Now>=P->Deadline?EFI_TIMEOUT:EFI_SUCCESS;
}
STATIC EFI_STATUS Read(PIANO_GENI_I2C_PIO *P,UINT32 Offset,UINT32 *Value) {
  EFI_STATUS Limit=Budget(P);if(Limit!=EFI_SUCCESS)return Limit;
  EFI_STATUS R=P->Io.Read32(P->Io.Context,Offset,Value);
  Limit=Budget(P);if(Limit!=EFI_SUCCESS)return Limit;
  return R==EFI_SUCCESS?R:EFI_ERROR(R)?R:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS Write(PIANO_GENI_I2C_PIO *P,UINT32 Offset,UINT32 Value) {
  EFI_STATUS Limit=Budget(P);if(Limit!=EFI_SUCCESS)return Limit;
  P->Touched=TRUE;P->Clean=FALSE;
  if(Offset==0x600){++P->CommandsAttempted;P->CurrentCommand=Value;P->CommandAttempted=TRUE;P->BusMayBeHeld=TRUE;}
  if(Offset==0x700)++P->TxWordsAttempted;
  EFI_STATUS R=P->Io.Write32(P->Io.Context,Offset,Value);
  Limit=Budget(P);if(Limit!=EFI_SUCCESS)return Limit;
  return R==EFI_SUCCESS?R:EFI_ERROR(R)?R:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS EFIAPI SessionRead(VOID *Context,UINT32 Offset,UINT32 *Value){return Read(Context,Offset,Value);}
STATIC EFI_STATUS CaptureSession(PIANO_GENI_I2C_PIO *P,PIANO_GENI_PIO_SNAPSHOT *S) {
  PIANO_GENI_PIO_IO Io=P->Io;Io.Context=P;Io.Read32=SessionRead;
  return PianoGeniI2cPioCapture(&Io,S);
}
STATIC EFI_STATUS Time(PIANO_GENI_I2C_PIO *P,UINT64 Limit,BOOLEAN Cleanup) {
  UINT32 *Count=Cleanup?&P->CleanupPolls:&P->Polls;
  if(++*Count>(Cleanup?PIANO_GENI_CLEANUP_LIMIT:PIANO_GENI_POLL_LIMIT))return EFI_TIMEOUT;
  UINT64 Now=P->Io.NowUs(P->Io.Context);
  if(Now<P->LastTime){P->ClockBroken=TRUE;return EFI_COMPROMISED_DATA;}
  P->LastTime=Now;return Now>=Limit?EFI_TIMEOUT:EFI_SUCCESS;
}
STATIC EFI_STATUS ClearIrq(PIANO_GENI_I2C_PIO *P,UINT32 Irq) {
  if(Irq&~OWN_IRQ)return EFI_COMPROMISED_DATA;
  return Irq?Write(P,0x618,Irq):EFI_SUCCESS;
}
STATIC EFI_STATUS Drain(PIANO_GENI_I2C_PIO *P,UINT32 Fifo,UINT8 *Frame,BOOLEAN Discard) {
  UINT32 Words=Fifo&RX_WORDS,Bytes=Words*4;
  if(Words>Depth(P->Before.HwRx))return EFI_COMPROMISED_DATA;
  if(!Words)return EFI_SUCCESS; // Empty FIFO metadata cannot authorize a pop.
  if(Fifo&BIT31) {
    UINT32 Last=(Fifo>>28)&7;
    // Native RVA6FD0 explicitly uses wc*4+last_valid-4. Zero/out-of-range
    // encodings are not guessed to mean a full word.
    if(Last<1 || Last>4)return EFI_COMPROMISED_DATA;
    Bytes-=4-Last;
  }
  if(!Discard && Bytes>PIANO_GENI_RUNTIME_BYTES-P->Received)return EFI_BAD_BUFFER_SIZE;
  for(UINT32 I=0;I<Words;++I) {
    UINT32 Word;EFI_STATUS R=Read(P,0x780,&Word);if(R!=EFI_SUCCESS)return R;
    UINT32 Count=MIN(Bytes,4U);
    for(UINT32 J=0;J<Count;++J)if(!Discard)Frame[P->Received++]=(UINT8)(Word>>(8*J));
    Bytes-=Count;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Poll(PIANO_GENI_I2C_PIO *P,BOOLEAN Rx,UINT8 *Frame) {
  BOOLEAN Sent=FALSE;
  for(;;) {
    EFI_STATUS R=Time(P,P->Deadline-PIANO_GENI_CLEANUP_US,FALSE);if(R!=EFI_SUCCESS)return R;
    UINT32 Irq,Status,Fifo,Tx,Firmware,Dma,Events,Route,Command,Secondary;
    if((R=Read(P,0x610,&Irq))!=EFI_SUCCESS || (R=Read(P,0x40,&Status))!=EFI_SUCCESS ||
       (R=Read(P,0x804,&Fifo))!=EFI_SUCCESS || (R=Read(P,0x800,&Tx))!=EFI_SUCCESS ||
       (R=Read(P,0x68,&Firmware))!=EFI_SUCCESS || (R=Read(P,0x258,&Dma))!=EFI_SUCCESS ||
       (R=Read(P,0xe18,&Events))!=EFI_SUCCESS || (R=Read(P,0xe1c,&Route))!=EFI_SUCCESS ||
       (R=Read(P,0x600,&Command))!=EFI_SUCCESS || (R=Read(P,0x630,&Secondary))!=EFI_SUCCESS)return R;
    UINT32 Expected=(Rx?2U:1U)<<27;Expected|=0x4cU<<9;if(!Rx)Expected|=BIT2;
    if(Status&BIT12 || Irq&~OWN_IRQ || Firmware!=P->Contract.Firmware || Dma || Events || Route ||
       Command!=Expected || Secondary!=P->Before.SecondaryCommand){P->OwnershipLost=TRUE;return EFI_COMPROMISED_DATA;}
    if(Irq&(ERRORS|CANCEL|ABORT))return EFI_DEVICE_ERROR;
    if(!Rx) {
      if(Fifo&RX_WORDS)return EFI_COMPROMISED_DATA;
      if((Irq&DONE) && !Sent)return EFI_BAD_BUFFER_SIZE;
      if((Irq&TX_READY) && !Sent) {
        if(Tx&TX_WORDS)return EFI_COMPROMISED_DATA;
        R=Write(P,0x700,0x4c);if(R!=EFI_SUCCESS)return R;Sent=TRUE;
      }
    } else if((Irq&(RX_READY|DONE)) || (Fifo&RX_WORDS)) {
      R=Drain(P,Fifo,Frame,FALSE);if(R!=EFI_SUCCESS)return R;
    }
    R=ClearIrq(P,Irq);if(R!=EFI_SUCCESS)return R;
    if(Irq&DONE) {
      if((Status&ACTIVE) || (Tx&TX_WORDS))return EFI_COMPROMISED_DATA;
      return Rx && P->Received!=PIANO_GENI_RUNTIME_BYTES?EFI_BAD_BUFFER_SIZE:EFI_SUCCESS;
    }
  }
}
STATIC EFI_STATUS Quiet(PIANO_GENI_I2C_PIO *P,BOOLEAN *IsQuiet) {
  UINT32 Status,Lines;EFI_STATUS R;
  *IsQuiet=FALSE;
  if((R=Read(P,0x40,&Status))!=EFI_SUCCESS || (R=Read(P,0x908,&Lines))!=EFI_SUCCESS)return R;
  *IsQuiet=!(Status&ACTIVE) && (Lines&3)==3;return EFI_SUCCESS;
}
STATIC EFI_STATUS StopCommand(PIANO_GENI_I2C_PIO *P) {
  BOOLEAN IsQuiet;EFI_STATUS R=Quiet(P,&IsQuiet);
  if(R!=EFI_SUCCESS)return R;
  if(IsQuiet)return EFI_SUCCESS;
  // Only cancel/abort the command this session attempted; no bus-clear,
  // STOP opcode, secondary sequencer reset or force-default/GPIO operation.
  if(!P->CommandAttempted)return EFI_ACCESS_DENIED;
  for(UINTN Phase=0;Phase<2;++Phase) {
    R=Write(P,0x604,Phase==0?BIT2:BIT1);if(R!=EFI_SUCCESS)return R;
    UINT64 Limit=Phase==0?P->Deadline-PIANO_GENI_CLEANUP_US/2:P->Deadline;
    for(UINTN I=0;I<PIANO_GENI_CLEANUP_LIMIT/2;++I) {
      R=Quiet(P,&IsQuiet);if(R!=EFI_SUCCESS)return R;
      if(IsQuiet)return EFI_SUCCESS;
      R=Time(P,Limit,TRUE);if(R!=EFI_SUCCESS)break;
    }
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Restore(PIANO_GENI_I2C_PIO *P,UINT32 Offset,UINT32 Value) {
  EFI_STATUS R=Write(P,Offset,Value);UINT32 Actual;
  if(R!=EFI_SUCCESS)return R;
  R=Read(P,Offset,&Actual);return R!=EFI_SUCCESS?R:Actual!=Value?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS Cleanup(PIANO_GENI_I2C_PIO *P) {
  PIANO_GENI_PIO_SNAPSHOT Current;
  EFI_STATUS R=CaptureSession(P,&Current);if(R!=EFI_SUCCESS)return R;
  R=Configuration(&P->Contract,&Current);
  if(R!=EFI_SUCCESS || (Current.Status&BIT12) || Current.SecondaryCommand!=P->Before.SecondaryCommand ||
     ((Current.Status&BIT0 || (Current.IoLines&3)!=3) && Current.MasterCommand!=P->CurrentCommand)) {
    P->OwnershipLost=TRUE;return EFI_ACCESS_DENIED;
  }
  R=StopCommand(P);if(R!=EFI_SUCCESS)return R;
  UINT32 Fifo,Irq;
  if((R=Read(P,0x804,&Fifo))!=EFI_SUCCESS || (R=Drain(P,Fifo,NULL,TRUE))!=EFI_SUCCESS ||
     (R=Read(P,0x610,&Irq))!=EFI_SUCCESS || (R=ClearIrq(P,Irq))!=EFI_SUCCESS)return R;
  // Restore only the local per-transfer registers we changed, after quiet.
  if((R=Restore(P,0x80c,P->Before.TxWatermark))!=EFI_SUCCESS ||
     (R=Restore(P,0x810,P->Before.RxWatermark))!=EFI_SUCCESS ||
     (R=Restore(P,0x26c,P->Before.TxLength))!=EFI_SUCCESS ||
     (R=Restore(P,0x270,P->Before.RxLength))!=EFI_SUCCESS ||
     (R=Restore(P,0x614,P->Before.MasterIrqEnable))!=EFI_SUCCESS)return R;
  R=CaptureSession(P,&P->After);if(R!=EFI_SUCCESS)return R;
  R=PianoGeniI2cPioValidate(&P->Contract,&P->After);
  if(R==EFI_SUCCESS){P->BusMayBeHeld=FALSE;P->Clean=TRUE;}return R;
}
EFI_STATUS EFIAPI PianoGeniI2cPioReadRuntime(VOID *Context,UINT64 Deadline,UINT8 Frame[68],UINTN *BytesRead) {
  PIANO_GENI_I2C_PIO *P=Context;
  if(P==NULL || Frame==NULL || BytesRead==NULL)return EFI_INVALID_PARAMETER;
  *BytesRead=0;Clear(Frame,68);
  if(!PIANO_GENI_I2C_PIO_EXPERIMENT)return EFI_UNSUPPORTED;
  if(P->Signature!=SIGNATURE)return EFI_NOT_READY;
  if(P->InRead)return EFI_NOT_READY;
  if(P->Failed || P->Quarantined)return EFI_ABORTED;
  if(P->Attempts>=64)return EFI_OUT_OF_RESOURCES;
  if(!ValidIo(&P->Io) || P->Io.Write32==NULL || P->Io.NowUs==NULL){
    P->Failed=P->Quarantined=TRUE;P->Clean=FALSE;P->LastStatus=EFI_COMPROMISED_DATA;return EFI_COMPROMISED_DATA;
  }
  P->InRead=TRUE;P->Received=P->Polls=P->CleanupPolls=0;P->Touched=FALSE;P->CommandAttempted=FALSE;P->OwnershipLost=FALSE;
  UINT8 Work[68]={0};EFI_STATUS R;
  UINT64 Now=P->Io.NowUs(P->Io.Context);
  if(Now<P->LastTime){P->ClockBroken=P->Quarantined=TRUE;P->Clean=FALSE;R=EFI_COMPROMISED_DATA;goto Done;}
  if(Deadline<=Now || Deadline-Now>PIANO_GENI_TOTAL_US || Deadline-Now<=PIANO_GENI_CLEANUP_US) {
    R=EFI_INVALID_PARAMETER;goto Done;
  }
  P->LastTime=Now;P->Deadline=Deadline;
  R=CaptureSession(P,&P->Before);
  if(R==EFI_SUCCESS)R=PianoGeniI2cPioValidate(&P->Contract,&P->Before);
  if(R!=EFI_SUCCESS){P->Quarantined=TRUE;P->Clean=FALSE;goto Done;}
  ++P->Attempts;PIANO_GENI_RUNTIME_PACKET Packet;PianoGeniI2cPioBuildRuntimePacket(&Packet);
  R=Time(P,P->Deadline-PIANO_GENI_CLEANUP_US,FALSE);if(R!=EFI_SUCCESS)goto Done;
  if((R=Write(P,0x614,OWN_IRQ))!=EFI_SUCCESS || (R=Write(P,0x80c,1))!=EFI_SUCCESS ||
     (R=Write(P,0x810,1))!=EFI_SUCCESS || (R=Write(P,0x26c,Packet.TxLength))!=EFI_SUCCESS)goto Stop;
  R=Time(P,P->Deadline-PIANO_GENI_CLEANUP_US,FALSE);if(R!=EFI_SUCCESS)goto Stop;
  R=Write(P,0x600,Packet.TxCommand);if(R!=EFI_SUCCESS)goto Stop;
  R=Poll(P,FALSE,Work);if(R!=EFI_SUCCESS)goto Stop;
  // STOP_STRETCH intentionally leaves the bus held. Recheck configuration and
  // ownership without requiring idle-high lines before the repeated START.
  R=CaptureSession(P,&P->After);
  if(R==EFI_SUCCESS)R=Configuration(&P->Contract,&P->After);
  if(R!=EFI_SUCCESS || (P->After.Status&ACTIVE) || P->After.MasterIrq || P->After.SecondaryIrq ||
     P->After.MasterControl || P->After.SecondaryControl || P->After.MasterCommand!=Packet.TxCommand ||
     P->After.SecondaryCommand!=P->Before.SecondaryCommand || (P->After.TxFifo&TX_WORDS) || (P->After.RxFifo&RX_WORDS)) {
    P->OwnershipLost=TRUE;if(R==EFI_SUCCESS)R=EFI_COMPROMISED_DATA;goto Stop;
  }
  if((R=Write(P,0x80c,0))!=EFI_SUCCESS || (R=Write(P,0x270,Packet.RxLength))!=EFI_SUCCESS)goto Stop;
  R=Time(P,P->Deadline-PIANO_GENI_CLEANUP_US,FALSE);if(R!=EFI_SUCCESS)goto Stop;
  R=Write(P,0x600,Packet.RxCommand);if(R!=EFI_SUCCESS)goto Stop;
  R=Poll(P,TRUE,Work);
Stop:
  if(P->OwnershipLost){P->CleanupStatus=EFI_ACCESS_DENIED;P->Quarantined=TRUE;P->Clean=FALSE;}
  else if(P->Touched) {
    P->CleanupStatus=Cleanup(P);
    if(P->CleanupStatus!=EFI_SUCCESS){P->Quarantined=TRUE;P->Clean=FALSE;}
    if(R==EFI_SUCCESS && P->CleanupStatus!=EFI_SUCCESS)R=P->CleanupStatus;
  }
  Now=P->Io.NowUs(P->Io.Context);
  if(Now<P->LastTime){P->ClockBroken=TRUE;R=EFI_COMPROMISED_DATA;}
  else if(Now>Deadline)R=EFI_TIMEOUT;
  P->LastTime=Now;
  if(P->ClockBroken){P->Quarantined=TRUE;P->Clean=FALSE;}
  if(R==EFI_SUCCESS){for(UINTN I=0;I<68;++I)Frame[I]=Work[I];*BytesRead=68;}
Done:
  Clear(Work,sizeof(Work));P->LastStatus=R;
  if(R!=EFI_SUCCESS)P->Failed=TRUE;
  P->InRead=FALSE;return R;
}
