// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register and TRE formats follow Linux qcom/gpi.c and spi-geni-qcom.c.
// GpiDxe confirms the EE register layout, channel commands and ring directions.
#include "PianoGeniSpiGpi.h"
#include "PianoGuardedRead.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/FdtLib.h>
#include <Library/DebugLib.h>

#define GPI_BASE 0xA00000U
#define GPI_BYTES 0x60000U
#define SE_BASE 0xA88000U
#define GPI_PATH "/soc/qcom,gpi-dma@a00000"
#define SE_PATH "/soc/qcom,qupv3_1_geni_se@ac0000/spi@a88000"
#define EE_OFFSET 0x10000U
#define INSTANCE_STRIDE 0x4000U
#define CHANNEL(K) ((K)*0x80U)
#define EVENT_CONTEXT 0x1000U
#define CHANNEL_DOORBELL(K) (0x2000U+(K)*8U)
#define EVENT_DOORBELL 0x2100U
#define CHANNEL_COMMAND 0x3008U
#define EVENT_COMMAND 0x3010U
#define CHANNEL_IRQ 0x3090U
#define EVENT_IRQ 0x3094U
#define CHANNEL_IRQ_CLEAR 0x30A0U
#define EVENT_IRQ_CLEAR 0x30A4U
#define STATE_MASK (0xFU<<20)
#define CHANNEL_READ_ONLY (STATE_MASK|(0x1FU<<8)|(0xFU<<4))
#define EVENT_READ_ONLY (STATE_MASK|(0xFFU<<8)|(0xFU<<4))
#define RING_BYTES 4096U
#define ELEMENT_BYTES 16U
#define RING_ELEMENTS (RING_BYTES/ELEMENT_BYTES)
#define COMMAND_TIMEOUT_US 10000U
#define SE_ACTIVE (BIT0|BIT12)
#define GENI_ERRORS (BIT1|BIT2|BIT3|BIT9|BIT10|BIT11|BIT12|BIT13|BIT14|BIT24|BIT25|BIT28|BIT29)

typedef struct {UINT32 Word[4];} TRE;

STATIC UINT32 Be32(CONST UINT8 *P) {
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}

STATIC UINT64 NowUs(PIANO_SPI_GPI *S) {
  UINT64 Counter=GetPerformanceCounter();
  UINT64 Ticks=S->CounterEnd>=S->CounterStart?Counter-S->CounterStart:S->CounterStart-Counter;
  return Ticks/S->Frequency*1000000ULL+(Ticks%S->Frequency)*1000000ULL/S->Frequency;
}

STATIC UINT32 Register(PIANO_SPI_GPI *S,UINT32 Offset) {
  return EE_OFFSET+S->Instance*INSTANCE_STRIDE+Offset;
}

STATIC UINT32 Read(PIANO_SPI_GPI *S,UINT32 Offset) {
  UINT32 At=Register(S,Offset);
  ASSERT(!(At&3) && At<=GPI_BYTES-4);
  return MmioRead32(GPI_BASE+At);
}

STATIC VOID Write(PIANO_SPI_GPI *S,UINT32 Offset,UINT32 Value) {
  UINT32 At=Register(S,Offset);
  ASSERT(!(At&3) && At<=GPI_BYTES-4);
  MmioWrite32(GPI_BASE+At,Value);MemoryFence();
}

STATIC EFI_STATUS Retain(PIANO_SPI_GPI *S,EFI_STATUS Status) {
  S->Retained=TRUE;S->Status=Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Status;
  DEBUG((DEBUG_ERROR,"PIANO_TOUCH_GPI_RETAIN status=%r instance=%u channels=%u/%u event=%u memory_kept=1 handoff=blocked\n",
    S->Status,S->Instance,S->ChannelAllocated[0],S->ChannelAllocated[1],S->EventAllocated));
  return S->Status;
}

STATIC EFI_STATUS Program(PIANO_SPI_GPI *S,UINT32 Offset,UINT32 Value,UINT32 Ignore) {
  // Channel/event contexts and doorbells belong to the allocated rings.
  // DEALLOC releases them; writing their allocated values back afterwards is
  // neither supported nor a restoration of the original unallocated state.
  BOOLEAN Saved=Offset<CHANNEL_COMMAND;
  for(UINT32 I=0;I<S->SavedCount;++I)if(S->Saved[I].Offset==Offset)Saved=TRUE;
  if(!Saved) {
    if(S->SavedCount==ARRAY_SIZE(S->Saved))return EFI_OUT_OF_RESOURCES;
    S->Saved[S->SavedCount].Offset=Offset;
    S->Saved[S->SavedCount].Value=Read(S,Offset);
    S->Saved[S->SavedCount].Ignore=Ignore;++S->SavedCount;
  }
  Write(S,Offset,Value);
  UINT32 Readback=Read(S,Offset);
  if((Readback^Value)&~Ignore) {
    if(!S->FailureRecorded) {
      S->FailureRecorded=TRUE;S->FailureOffset=Register(S,Offset);
      S->FailureExpected=Value;S->FailureReadback=Readback;
    }
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS SeProgram(PIANO_SPI_GPI *S,UINT32 Offset,UINT32 Value) {
  BOOLEAN Saved=FALSE;
  for(UINT32 I=0;I<S->SeSavedCount;++I)if(S->SeSaved[I].Offset==Offset)Saved=TRUE;
  if(!Saved) {
    if(S->SeSavedCount==ARRAY_SIZE(S->SeSaved))return EFI_OUT_OF_RESOURCES;
    S->SeSaved[S->SeSavedCount].Offset=Offset;
    S->SeSaved[S->SeSavedCount].Value=MmioRead32(SE_BASE+Offset);++S->SeSavedCount;
  }
  S->SerialChanged=TRUE;
  MmioWrite32(SE_BASE+Offset,Value);MemoryFence();
  UINT32 Readback=MmioRead32(SE_BASE+Offset);
  if(Readback!=Value) {
    if(!S->FailureRecorded) {
      S->FailureRecorded=TRUE;S->FailureOffset=SE_BASE+Offset;
      S->FailureExpected=Value;S->FailureReadback=Readback;
    }
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Command(PIANO_SPI_GPI *S,UINT32 Channel,UINT32 Opcode,UINT32 State) {
  BOOLEAN Event=Channel==2;
  UINT32 Flag=Event?1U:1U<<Channel;
  UINT32 Irq=Event?EVENT_IRQ:CHANNEL_IRQ;
  UINT32 Clear=Event?EVENT_IRQ_CLEAR:CHANNEL_IRQ_CLEAR;
  UINT32 Context=Event?EVENT_CONTEXT:CHANNEL(Channel);
  Write(S,Clear,Flag);
  Write(S,Event?EVENT_COMMAND:CHANNEL_COMMAND,(Opcode<<24)|(Event?0:Channel));
  UINT64 Deadline=NowUs(S)+COMMAND_TIMEOUT_US;
  do {
    if(Read(S,Irq)&Flag) {
      Write(S,Clear,Flag);
      UINT32 Current=(Read(S,Context)>>20)&15U;
      // DEALLOC acknowledges completion without updating the context state.
      if(Opcode==10 || Current==State)return EFI_SUCCESS;
      if(Current!=4)return EFI_DEVICE_ERROR;
    }
  } while(NowUs(S)<Deadline);
  DEBUG((DEBUG_ERROR,"PIANO_TOUCH_GPI_COMMAND channel=%u opcode=%u expected=%u actual=%u irq=%08x status=Timeout\n",
    Channel,Opcode,State,(Read(S,Context)>>20)&15U,Read(S,Irq)));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS StopChannels(PIANO_SPI_GPI *S,BOOLEAN Reset) {
  for(UINT32 K=0;K<2;++K)if(S->ChannelStarted[K]) {
    EFI_STATUS E=Command(S,K,2,3);
    if(E!=EFI_SUCCESS)return Retain(S,E);
    S->ChannelStarted[K]=FALSE;
  }
  if(Reset)for(UINT32 K=0;K<2;++K)if(S->ChannelAllocated[K]) {
    EFI_STATUS E=Command(S,K,9,1);
    if(E!=EFI_SUCCESS)return Retain(S,E);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS StopSerial(PIANO_SPI_GPI *S) {
  for(UINT32 Phase=0;Phase<3;++Phase) {
    UINT32 Active=MmioRead32(SE_BASE+0x40);
    if(!(Active&SE_ACTIVE))return EFI_SUCCESS;
    // SE2 is exclusively ours. Never reset the secondary sequencer.
    if((Active&BIT12) || Phase==2)return Retain(S,EFI_DEVICE_ERROR);
    MmioWrite32(SE_BASE+0x604,Phase?BIT1:BIT2);MemoryFence();
    UINT64 Deadline=NowUs(S)+COMMAND_TIMEOUT_US;
    while((MmioRead32(SE_BASE+0x40)&BIT0) && NowUs(S)<Deadline)CpuPause();
  }
  return Retain(S,EFI_TIMEOUT);
}

STATIC EFI_STATUS Allocate(PIANO_SPI_GPI *S,PIANO_DMA_BUFFER *B,UINTN Bytes,PIANO_DMA_DIRECTION Direction) {
  EFI_STATUS E=PianoDmaAllocate(&S->Device,Bytes,4096,32,Direction,B);
  if(E==EFI_SUCCESS)E=PianoDmaMap(B);
  return E;
}

STATIC EFI_STATUS ConfigureChannel(PIANO_SPI_GPI *S,UINT32 K,PIANO_DMA_BUFFER *Ring) {
  UINT32 Context=CHANNEL(K);
  UINT64 Address=Ring->DeviceAddress;
  EFI_STATUS E=Program(S,Context,(ELEMENT_BYTES<<24)|(K==0?BIT3:0)|2U,CHANNEL_READ_ONLY);
  if(E==EFI_SUCCESS)E=Program(S,Context+4,RING_BYTES,0);
  if(E==EFI_SUCCESS)E=Program(S,Context+8,(UINT32)Address,0);
  if(E==EFI_SUCCESS)E=Program(S,Context+12,(UINT32)(Address>>32),0);
  if(E==EFI_SUCCESS)E=Program(S,CHANNEL_DOORBELL(K)+4,(UINT32)(Address>>32),0);
  // Pair the channels, SPI protocol 1, serial engine 2.
  if(E==EFI_SUCCESS)E=Program(S,Context+0x60,((!K)<<16)|(1U<<4)|2U,0);
  for(UINT32 I=0;E==EFI_SUCCESS && I<3;++I)E=Program(S,Context+0x64+I*4,0,0);
  if(E==EFI_SUCCESS)E=Program(S,Context+0x5C,1,0);
  return E;
}

STATIC EFI_STATUS ConfigureEvent(PIANO_SPI_GPI *S) {
  UINT64 Address=S->EventRing.DeviceAddress;
  // Channel state, EE and channel ID are hardware-owned read-only fields.
  EFI_STATUS E=Program(S,EVENT_CONTEXT,(ELEMENT_BYTES<<24)|BIT16|2U,EVENT_READ_ONLY);
  if(E==EFI_SUCCESS)E=Program(S,EVENT_CONTEXT+4,RING_BYTES,0);
  if(E==EFI_SUCCESS)E=Program(S,EVENT_CONTEXT+8,(UINT32)Address,0);
  if(E==EFI_SUCCESS)E=Program(S,EVENT_CONTEXT+12,(UINT32)(Address>>32),0);
  if(E==EFI_SUCCESS)E=Program(S,EVENT_DOORBELL+4,(UINT32)(Address>>32),0);
  CONST UINT32 Offsets[]={0x20,0x28,0x2C,0x30,0x34};
  for(UINT32 I=0;E==EFI_SUCCESS && I<ARRAY_SIZE(Offsets);++I)
    E=Program(S,EVENT_CONTEXT+Offsets[I],0,0);
  if(E==EFI_SUCCESS)E=PianoDmaBegin(&S->EventRing,"TOUCH_SPI_EVENTS");
  if(E==EFI_SUCCESS) {
    S->EventRead=0;S->EventWrite=RING_ELEMENTS-1;
    Write(S,EVENT_DOORBELL,(UINT32)(Address+S->EventWrite*ELEMENT_BYTES));
  }
  return E;
}

STATIC EFI_STATUS ValidateDt(CONST VOID *Fdt,UINT32 *Mask) {
  INT32 Node=FdtPathOffset(Fdt,GPI_PATH),Length=0;
  CONST UINT8 *P=Node<0?NULL:FdtGetProp(Fdt,Node,"reg",&Length);
  if(!P || Length!=8 || Be32(P)!=GPI_BASE || Be32(P+4)!=GPI_BYTES)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"qcom,gpi-ee-offset",&Length);
  if(!P || Length!=4 || Be32(P)!=0x10000)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"qcom,gpii-mask",&Length);
  if(!P || Length!=4 || !(Be32(P)&0x1E) || (Be32(P)&~0x1EU))return EFI_UNSUPPORTED;
  *Mask=Be32(P);
  UINT32 Phandle=FdtGetPhandle(Fdt,Node);
  Node=FdtPathOffset(Fdt,SE_PATH);
  P=Node<0?NULL:FdtGetProp(Fdt,Node,"dmas",&Length);
  if(!P || Length!=48)return EFI_UNSUPPORTED;
  for(UINT32 K=0;K<2;++K) {
    CONST UINT8 *D=P+K*24;
    if(Be32(D)!=Phandle || Be32(D+4)!=K || Be32(D+8)!=2 || Be32(D+12)!=1 ||
       Be32(D+16)!=64 || Be32(D+20))return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC BOOLEAN GuardAlive(VOID *Context) {
  return Context==gBS && gST && gST->BootServices==gBS && gBS &&
    gBS->Hdr.Signature==EFI_BOOT_SERVICES_SIGNATURE;
}

STATIC EFI_STATUS ProbeIdle(PIANO_SPI_GPI *S,UINT32 Mask) {
  PIANO_GUARDED_CONFIG Config={.Context=gBS,.Services=gBS,.DxeServices=gDS,.BootServicesAlive=GuardAlive,
    .Ranges={{GPI_BASE,GPI_BYTES,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},
    .RangeCount=1,.MaxReads=32,.MaxUsecs=100000};
  VOID *Token=NULL;
  EFI_STATUS E=PianoGuardedReadBegin(&Config,&Token);
  if(E!=EFI_SUCCESS) {
    CONST PIANO_GUARDED_REPORT *R=PianoGuardedReadReport();
    if(R)DEBUG((DEBUG_ERROR,"PIANO_TOUCH_GPI_PROBE_BEGIN status=%r mapping=%r page=%lx par=%lx gcd_type=%u gcd_attrs=%lx cleanup=%r retained=%u\n",
      E,R->MappingStatus,R->LastMappingPage,R->LastPar,R->LastGcdType,R->LastGcdAttributes,R->CleanupStatus,R->Retained));
    return R && (R->Retained || R->Active || R->Fatal || R->ServicesLost)?Retain(S,E):E;
  }
  // The DT mask names the AP-owned instances sharing SID 0xB6. Other EEs
  // must not be probed; the SMMU adapter also refuses an existing SID route.
  for(UINT32 I=1;E==EFI_SUCCESS && I<5;++I)if(Mask&(1U<<I)) {
    S->Instance=I;
    CONST UINT32 Offsets[]={CHANNEL(0),CHANNEL(1),EVENT_CONTEXT};
    for(UINT32 K=0;K<ARRAY_SIZE(Offsets);++K) {
      UINT32 Value=0;
      S->ProbeOffset=Register(S,Offsets[K]);
      E=PianoGuardedRead32(Token,GPI_BASE+Register(S,Offsets[K]),&Value);
      S->ProbeValue=Value;
      DEBUG((DEBUG_WARN,"PIANO_TOUCH_GPI_PROBE instance=%u offset=%x value=%08x status=%r\n",I,S->ProbeOffset,Value,E));
      if(E!=EFI_SUCCESS)break;
      if(Value&STATE_MASK){E=EFI_ALREADY_STARTED;break;}
    }
  }
  for(S->Instance=1;S->Instance<5;++S->Instance)if(Mask&(1U<<S->Instance))break;
  UINT32 Enabled=0;
  if(E==EFI_SUCCESS) {
    S->ProbeOffset=Register(S,0x3000);
    E=PianoGuardedRead32(Token,GPI_BASE+S->ProbeOffset,&Enabled);S->ProbeValue=Enabled;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_GPI_PROBE instance=%u offset=%x value=%08x status=%r\n",S->Instance,S->ProbeOffset,Enabled,E));
  }
  CONST UINT32 Errors[]={0x3100,0x3118,0x3200};
  UINT32 *Values[]={&S->LastGlobalIrq,&S->LastGeneralIrq,&S->LastErrorLog};
  for(UINT32 I=0;E==EFI_SUCCESS && I<ARRAY_SIZE(Errors);++I) {
    UINT32 Value=0;
    S->ProbeOffset=Register(S,Errors[I]);
    E=PianoGuardedRead32(Token,GPI_BASE+Register(S,Errors[I]),&Value);
    S->ProbeValue=Value;*Values[I]=Value;
    DEBUG((DEBUG_WARN,"PIANO_TOUCH_GPI_PROBE instance=%u offset=%x value=%08x status=%r\n",S->Instance,S->ProbeOffset,Value,E));
    // ERROR_LOG retains payload even without a pending global error interrupt.
    if(E==EFI_SUCCESS && I<2 && Value)E=EFI_DEVICE_ERROR;
  }
  EFI_STATUS End=PianoGuardedReadEnd(Token);
  CONST PIANO_GUARDED_REPORT *Report=PianoGuardedReadReport();
  if(End!=EFI_SUCCESS || !Report || Report->Retained || Report->Fatal || Report->ServicesLost)
    return Retain(S,End==EFI_SUCCESS?EFI_ACCESS_DENIED:End);
  if(E!=EFI_SUCCESS)return E;
  return Enabled&1U?EFI_SUCCESS:EFI_NOT_READY;
}

EFI_STATUS PianoGeniSpiGpiOpen(PIANO_SPI_GPI *S,CONST VOID *Fdt,UINT32 SourceHz,UINT32 MaxHz) {
  if(!S || !Fdt || SourceHz<1000000U || MaxHz<1000000U || MaxHz>19200000U)return EFI_INVALID_PARAMETER;
  if(S->Started)return EFI_ALREADY_STARTED;
  ZeroMem(S,sizeof(*S));S->Started=TRUE;S->CloseStatus=EFI_NOT_STARTED;S->Stage="validate";
  S->Frequency=GetPerformanceCounterProperties(&S->CounterStart,&S->CounterEnd);
  if(S->Frequency<1000000ULL || S->CounterStart==S->CounterEnd)return S->Status=EFI_UNSUPPORTED;
  S->Divider=(SourceHz+MaxHz-1)/MaxHz;
  if(!S->Divider || S->Divider>4095)return S->Status=EFI_UNSUPPORTED;
  UINT32 Mask=0;
  EFI_STATUS E=ValidateDt(Fdt,&Mask);
  if(E!=EFI_SUCCESS)return S->Status=E;
  if((MmioRead32(SE_BASE+0x68)&0xFF00U)!=0x100U || !(MmioRead32(SE_BASE+0x64)&1) ||
     (MmioRead32(SE_BASE+0x40)&SE_ACTIVE))return S->Status=EFI_NOT_READY;
  E=ProbeIdle(S,Mask);
  S->ProbeStatus=E;
  if(E!=EFI_SUCCESS)return S->Status=E;
  // GPI CONFIG/GO descriptors also change these SE registers. Save the whole
  // transfer configuration before a descriptor can execute, not after it.
  CONST UINT32 SeOffsets[]={0x24,0x28,0x48,0x7C,0x224,0x22C,0x230,0x24C,0x250,
    0x254,0x258,0x25C,0x260,0x264,0x268,0x26C,0x270,0x278,0x284,0x288,0x614,0xE18,0xE1C};
  for(UINT32 I=0;I<ARRAY_SIZE(SeOffsets);++I) {
    S->SeSaved[I].Offset=SeOffsets[I];S->SeSaved[I].Value=MmioRead32(SE_BASE+SeOffsets[I]);
  }
  S->SeSavedCount=ARRAY_SIZE(SeOffsets);
  S->Device=(PIANO_DMA_DEVICE){.Name="gpi1",.StreamId=0xB6,.AddressBits=32,.CacheLine=64,.ReserveAcrossExit=TRUE};
  S->Stage="smmu-open";
  E=PianoOwnedSmmuOpenGpi(Fdt,&S->Smmu,&S->Device);
  if(E!=EFI_SUCCESS)goto Failed;
  S->Stage="dma-allocate";
  E=Allocate(S,&S->TxRing,RING_BYTES,PianoDmaToDevice);
  if(E==EFI_SUCCESS)E=Allocate(S,&S->RxRing,RING_BYTES,PianoDmaToDevice);
  if(E==EFI_SUCCESS)E=Allocate(S,&S->EventRing,RING_BYTES,PianoDmaFromDevice);
  if(E==EFI_SUCCESS)E=Allocate(S,&S->Tx,PIANO_SPI_GPI_MAX_BYTES,PianoDmaToDevice);
  if(E==EFI_SUCCESS)E=Allocate(S,&S->Rx,PIANO_SPI_GPI_MAX_BYTES,PianoDmaFromDevice);
  if(E!=EFI_SUCCESS)goto Failed;
  // Like qcom/gpi.c, discard an old error payload only after idle checks and
  // attaching our DMA domain. Pending errors are never cleared to continue.
  Write(S,0x3200,0);
  // Poll raw completion sources. Keep the top-level IRQ mask zero so no GIC
  // handler is required, and never change the secure FIFO/GSI selection.
  CONST struct {UINT32 Offset,Value;} Irqs[]={
    {0x3088,0},{0x3098,3},{0x309C,1},{0x30B8,1},{0x3108,1},{0x3120,15},
    {0x3180,1},{0x3188,0},{0x318C,0}};
  S->Stage="irq-config";
  for(UINT32 I=0;E==EFI_SUCCESS && I<ARRAY_SIZE(Irqs);++I)
    E=Program(S,Irqs[I].Offset,Irqs[I].Value,0);
  if(E!=EFI_SUCCESS)goto Failed;
  S->Stage="event-allocate";S->EventAllocated=TRUE;E=Command(S,2,0,1);
  if(E==EFI_SUCCESS)S->Stage="event-config";
  if(E==EFI_SUCCESS)E=ConfigureEvent(S);
  for(UINT32 K=0;E==EFI_SUCCESS && K<2;++K) {
    S->Stage=K?"rx-allocate":"tx-allocate";
    S->ChannelAllocated[K]=TRUE;E=Command(S,K,0,1);
    if(E==EFI_SUCCESS)S->Stage=K?"rx-config":"tx-config";
    if(E==EFI_SUCCESS)E=ConfigureChannel(S,K,K?&S->RxRing:&S->TxRing);
  }
  if(E!=EFI_SUCCESS)goto Failed;
  S->Stage="se-config";E=SeProgram(S,0xE1C,0);
  if(E==EFI_SUCCESS)E=SeProgram(S,0x614,MmioRead32(SE_BASE+0x614)&~(BIT0|BIT26|BIT27|BIT30));
  if(E==EFI_SUCCESS)E=SeProgram(S,0x258,1);
  if(E==EFI_SUCCESS)E=SeProgram(S,0xE18,MmioRead32(SE_BASE+0xE18)|15U);
  if(E==EFI_SUCCESS)E=SeProgram(S,0x7C,0);
  if(E==EFI_SUCCESS)E=SeProgram(S,0x24,MmioRead32(SE_BASE+0x24)|0x7FU);
  if(E==EFI_SUCCESS)E=SeProgram(S,0x28,MmioRead32(SE_BASE+0x28)|0x7FU);
  if(E!=EFI_SUCCESS)goto Failed;
  S->Stage="ready";S->Open=TRUE;
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_GPI_READY instance=%u se=2 sid=B6 source_hz=%u divider=%u irq=polled dma_buffers=5\n",
    S->Instance,SourceHz,S->Divider));
  return S->Status=EFI_SUCCESS;
Failed:
  S->Status=E;
  if(PianoGeniSpiGpiClose(S)!=EFI_SUCCESS)return Retain(S,E);
  return E;
}

STATIC EFI_STATUS Events(PIANO_SPI_GPI *S,UINTN Bytes,UINT32 Expected,UINT32 *Done) {
  S->LastGlobalIrq=Read(S,0x3100);S->LastGeneralIrq=Read(S,0x3118);S->LastErrorLog=Read(S,0x3200);
  if(S->LastGlobalIrq || S->LastGeneralIrq)return EFI_DEVICE_ERROR;
  UINT64 Producer=Read(S,EVENT_CONTEXT+0x10)|((UINT64)Read(S,EVENT_CONTEXT+0x14)<<32);
  EFI_STATUS E=PianoDmaSyncForCpuQuiet(&S->EventRing);
  if(E!=EFI_SUCCESS)return E;
  UINT64 Base=S->EventRing.DeviceAddress;
  if(Producer<Base || Producer>=Base+RING_BYTES || ((Producer-Base)&15))return EFI_COMPROMISED_DATA;
  UINT32 End=(UINT32)((Producer-Base)/ELEMENT_BYTES),Count=0;
  while(S->EventRead!=End && Count++<RING_ELEMENTS) {
    TRE Event=((CONST TRE *)S->EventRing.Cpu)[S->EventRead];
    CopyMem(S->LastEvent,Event.Word,sizeof(Event));++S->Events;
    UINT32 Type=(Event.Word[3]>>16)&255U,Channel=Event.Word[3]>>24;
    if(Type==0x22) {
      UINT32 Code=Event.Word[2]>>24;
      UINT64 Pointer=Event.Word[0]|((UINT64)Event.Word[1]<<32);
      if(Channel>1 || (Code!=1 && Code!=2 && Code!=4))return EFI_DEVICE_ERROR;
      UINT32 Flag=1U<<Channel;
      UINT64 Descriptor=(Channel?S->RxRing.DeviceAddress:S->TxRing.DeviceAddress)+(Channel?0:32);
      if(!(Expected&Flag) || Pointer!=Descriptor || (Event.Word[2]&0xFFFFFFU)!=Bytes)return EFI_COMPROMISED_DATA;
      if(Code==2) {
        if(*Done&Flag)return EFI_COMPROMISED_DATA;
        *Done|=Flag;
      }
    } else if(Type!=0x31 && Type!=0xFF) {
      return EFI_UNSUPPORTED;
    }
    S->EventRead=(S->EventRead+1)%RING_ELEMENTS;
    S->EventWrite=(S->EventWrite+1)%RING_ELEMENTS;
  }
  Write(S,EVENT_DOORBELL,(UINT32)(Base+S->EventWrite*ELEMENT_BYTES));
  Write(S,0x30C0,1);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Complete(PIANO_SPI_GPI *S,PIANO_DMA_BUFFER *Buffer,EFI_STATUS Status) {
  if(!Buffer->Active)return EFI_SUCCESS;
  EFI_STATUS E=PianoDmaComplete(Buffer,Status,TRUE);
  return E==Status?EFI_SUCCESS:Retain(S,E);
}

STATIC VOID TransferDescriptors(PIANO_SPI_GPI *S,BOOLEAN Receive,UINTN Bytes) {
  ZeroMem(S->TxRing.Cpu,RING_BYTES);ZeroMem(S->RxRing.Cpu,RING_BYTES);
  TRE *T=S->TxRing.Cpu,*R=S->RxRing.Cpu;
  T[0]=(TRE){{4U|BIT24|BIT25,0,S->Divider,(0x22U<<16)|BIT0}};
  // Stock spi-msm-geni supplies RX words for full duplex as well as RX-only.
  // All transfers here use eight-bit words, so the word count equals Bytes.
  T[1]=(TRE){{Receive?3U:1U,0,Receive?(UINT32)Bytes:0,(0x20U<<16)|BIT0|(Receive?BIT11:0)}};
  T[2]=(TRE){{(UINT32)S->Tx.DeviceAddress,(UINT32)(S->Tx.DeviceAddress>>32),(UINT32)Bytes,(0x10U<<16)|BIT9}};
  if(Receive)R[0]=(TRE){{(UINT32)S->Rx.DeviceAddress,(UINT32)(S->Rx.DeviceAddress>>32),(UINT32)Bytes,0x10U<<16}};
}

EFI_STATUS PianoGeniSpiGpiTransfer(PIANO_SPI_GPI *S,CONST UINT8 *Tx,UINT8 *Rx,UINTN Bytes,UINT32 TimeoutUs) {
  if(!S || !Tx || !Bytes || Bytes>PIANO_SPI_GPI_MAX_BYTES || !TimeoutUs || TimeoutUs>1000000U)return EFI_INVALID_PARAMETER;
  if(!S->Open || S->Busy || S->Retained || S->Failures)return EFI_NOT_READY;
  if(MmioRead32(SE_BASE+0x40)&SE_ACTIVE)return EFI_NOT_READY;
  S->Busy=TRUE;++S->Transfers;
  MmioWrite32(SE_BASE+0x618,MAX_UINT32);MemoryFence();
  CopyMem(S->Tx.Cpu,Tx,Bytes);
  TransferDescriptors(S,Rx!=NULL,Bytes);
  EFI_STATUS E=ConfigureChannel(S,0,&S->TxRing);
  if(E==EFI_SUCCESS)E=ConfigureChannel(S,1,&S->RxRing);
  if(E==EFI_SUCCESS)E=PianoDmaBegin(&S->Tx,"TOUCH_SPI_TX");
  if(E==EFI_SUCCESS && Rx)E=PianoDmaBegin(&S->Rx,"TOUCH_SPI_RX");
  if(E==EFI_SUCCESS)E=PianoDmaBegin(&S->TxRing,"TOUCH_SPI_TX_RING");
  if(E==EFI_SUCCESS && Rx)E=PianoDmaBegin(&S->RxRing,"TOUCH_SPI_RX_RING");
  for(UINT32 K=0;E==EFI_SUCCESS && K<2;++K) {
    // A command that times out may already have started the channel.
    S->ChannelStarted[K]=TRUE;E=Command(S,K,1,2);
  }
  UINT32 Done=0,Expected=Rx?3U:1U;
  if(E==EFI_SUCCESS) {
    if(Rx)Write(S,CHANNEL_DOORBELL(1),(UINT32)(S->RxRing.DeviceAddress+16));
    Write(S,CHANNEL_DOORBELL(0),(UINT32)(S->TxRing.DeviceAddress+48));
    UINT64 Deadline=NowUs(S)+TimeoutUs;
    // Like stock spi-msm-geni, a full-duplex read needs both callbacks with
    // the exact length. An advanced RX ring pointer only proves TRE fetch.
    while(Done!=Expected) {
      S->LastStatusReg=MmioRead32(SE_BASE+0x610);
      if(S->LastStatusReg&GENI_ERRORS){E=EFI_DEVICE_ERROR;break;}
      E=Events(S,Bytes,Expected,&Done);
      if(E!=EFI_SUCCESS || Done==Expected)break;
      if(NowUs(S)>=Deadline){E=EFI_TIMEOUT;break;}
    }
  }
  EFI_STATUS Stop=StopChannels(S,TRUE);
  if(Stop==EFI_SUCCESS)Stop=StopSerial(S);
  if(Stop!=EFI_SUCCESS) {
    S->Busy=FALSE;return Retain(S,Stop);
  }
  if(E==EFI_SUCCESS)E=Events(S,Bytes,Expected,&Done);
  PIANO_DMA_BUFFER *Buffers[]={&S->Tx,&S->Rx,&S->TxRing,&S->RxRing};
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I) {
    EFI_STATUS C=Complete(S,Buffers[I],E);
    if(C!=EFI_SUCCESS){S->Busy=FALSE;return C;}
  }
  if(E==EFI_SUCCESS && Rx)CopyMem(Rx,S->Rx.Cpu,Bytes);
  if(E!=EFI_SUCCESS) {
    ++S->Failures;
    DEBUG((DEBUG_ERROR,"PIANO_TOUCH_GPI_TRANSFER status=%r bytes=%u geni=%08x event=%08x/%08x/%08x/%08x global=%08x general=%08x error=%08x\n",
      E,(UINT32)Bytes,S->LastStatusReg,S->LastEvent[0],S->LastEvent[1],S->LastEvent[2],S->LastEvent[3],
      S->LastGlobalIrq,S->LastGeneralIrq,S->LastErrorLog));
  }
  S->Busy=FALSE;return S->Status=E;
}

EFI_STATUS PianoGeniSpiGpiClose(PIANO_SPI_GPI *S) {
  if(!S)return EFI_INVALID_PARAMETER;
  if(!S->Started)return EFI_SUCCESS;
  if(S->Retained || S->Busy)return EFI_ACCESS_DENIED;
  if(S->CloseStatus!=EFI_NOT_STARTED)return S->CloseStatus;
  EFI_STATUS E=StopChannels(S,FALSE);
  if(E==EFI_SUCCESS && S->SerialChanged)E=StopSerial(S);
  if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
  for(UINT32 K=0;K<2;++K)if(S->ChannelAllocated[K]) {
    E=Command(S,K,9,1);
    if(E==EFI_SUCCESS)E=Command(S,K,10,0);
    if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
    S->ChannelAllocated[K]=FALSE;
  }
  if(S->EventAllocated) {
    E=Command(S,2,9,1);
    if(E==EFI_SUCCESS)E=Command(S,2,10,0);
    if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
    S->EventAllocated=FALSE;
  }
  if(S->Smmu.Attached) {
    E=PianoOwnedSmmuCheckGpi(&S->Smmu);
    if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
  }
  while(S->SerialChanged && S->SeSavedCount) {
    UINT32 I=S->SeSavedCount-1,Offset=S->SeSaved[I].Offset,Value=S->SeSaved[I].Value;
    MmioWrite32(SE_BASE+Offset,Value);MemoryFence();
    UINT32 Readback=MmioRead32(SE_BASE+Offset);
    if(Readback!=Value) {
      S->FailureRecorded=TRUE;S->FailureOffset=SE_BASE+Offset;
      S->FailureExpected=Value;S->FailureReadback=Readback;
      DEBUG((DEBUG_ERROR,"PIANO_TOUCH_RESTORE offset=%x expected=%08x readback=%08x\n",
        S->FailureOffset,Value,Readback));
      return S->CloseStatus=Retain(S,EFI_DEVICE_ERROR);
    }
    --S->SeSavedCount;
  }
  S->SerialChanged=FALSE;
  while(S->SavedCount) {
    UINT32 I=S->SavedCount-1;
    Write(S,S->Saved[I].Offset,S->Saved[I].Value);
    UINT32 Readback=Read(S,S->Saved[I].Offset);
    if((Readback^S->Saved[I].Value)&~S->Saved[I].Ignore) {
      S->FailureRecorded=TRUE;S->FailureOffset=GPI_BASE+Register(S,S->Saved[I].Offset);
      S->FailureExpected=S->Saved[I].Value;S->FailureReadback=Readback;
      DEBUG((DEBUG_ERROR,"PIANO_TOUCH_RESTORE offset=%x expected=%08x readback=%08x\n",
        S->FailureOffset,S->FailureExpected,Readback));
      return S->CloseStatus=Retain(S,EFI_DEVICE_ERROR);
    }
    --S->SavedCount;
  }
  PIANO_DMA_BUFFER *Buffers[]={&S->Rx,&S->Tx,&S->RxRing,&S->TxRing,&S->EventRing};
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Signature) {
    E=Complete(S,Buffers[I],EFI_SUCCESS);
    if(E==EFI_SUCCESS)E=PianoDmaFree(Buffers[I]);
    if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
  }
  if(S->Smmu.Domain || S->Smmu.TableMemory.Signature) {
    E=PianoOwnedSmmuClose(&S->Smmu);
    if(E!=EFI_SUCCESS)return S->CloseStatus=Retain(S,E);
  }
  S->Open=FALSE;
  DEBUG((DEBUG_WARN,"PIANO_TOUCH_GPI_RELEASE status=Success transfers=%u failures=%u events=%u dma_freed=1 domain_freed=1\n",
    S->Transfers,S->Failures,S->Events));
  return S->CloseStatus=EFI_SUCCESS;
}
