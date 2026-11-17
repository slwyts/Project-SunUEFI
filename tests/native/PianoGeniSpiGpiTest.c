// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production stop and completion checks without programming DMA.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/components/touch-product/PianoGeniSpiGpi.c"

EFI_BOOT_SERVICES *gBS;
EFI_DXE_SERVICES *gDS;
EFI_SYSTEM_TABLE *gST;
STATIC UINT32 Registers[GPI_BYTES/4];
STATIC UINT64 Clock;
STATIC UINT32 StopFailure,RouteFailure,FreeCount,DomainClose,CommandCount,RouteChecks;
STATIC UINT32 EventStorage[RING_BYTES/4];

BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(VOID)Level;return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Description){(VOID)File;(VOID)Line;(VOID)Description;abort();}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID EFIAPI MemoryFence(VOID){}
VOID EFIAPI CpuPause(VOID){}
UINT64 EFIAPI GetPerformanceCounter(VOID){Clock+=100;return Clock;}
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address>=SE_BASE && Address<SE_BASE+0x4000)return 0;
  assert(Address>=GPI_BASE && Address<GPI_BASE+GPI_BYTES && !(Address&3));
  return Registers[(Address-GPI_BASE)/4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(Address>=GPI_BASE && Address<GPI_BASE+GPI_BYTES && !(Address&3));
  UINT32 Offset=(UINT32)(Address-GPI_BASE),Local=Offset-EE_OFFSET-INSTANCE_STRIDE;
  if(Local==CHANNEL_IRQ_CLEAR)Registers[(Offset-0x10)/4]&=~Value;
  else if(Local==EVENT_IRQ_CLEAR)Registers[(Offset-0x10)/4]&=~Value;
  else if(Local==CHANNEL_COMMAND || Local==EVENT_COMMAND) {
    ++CommandCount;
    UINT32 Op=Value>>24,K=Local==EVENT_COMMAND?2:Value&255;
    if(StopFailure && Op==2 && K==0)return Value;
    UINT32 Context=Offset-Local+(K==2?EVENT_CONTEXT:CHANNEL(K));
    UINT32 State=Op==1?2:Op==2?3:Op==10?0:1;
    Registers[Context/4]=(Registers[Context/4]&~STATE_MASK)|(State<<20);
    Registers[(Offset-Local+(K==2?EVENT_IRQ:CHANNEL_IRQ))/4]|=K==2?1U:1U<<K;
  } else {
    UINT32 ReadOnly=Local==EVENT_CONTEXT?EVENT_READ_ONLY:
      (Local==CHANNEL(0) || Local==CHANNEL(1))?CHANNEL_READ_ONLY:0;
    Registers[Offset/4]=(Value&~ReadOnly)|(Registers[Offset/4]&ReadOnly);
  }
  return Value;
}
EFI_STATUS PianoDmaSyncForCpuQuiet(PIANO_DMA_BUFFER *B){assert(B->Active);return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active && Quiet);B->Active=FALSE;return Status;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!B->Active);++FreeCount;ZeroMem(B,sizeof(*B));return EFI_SUCCESS;}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){assert(FreeCount==5);++DomainClose;C->Domain=NULL;return EFI_SUCCESS;}
EFI_STATUS PianoOwnedSmmuCheckGpi(PIANO_OWNED_SMMU *C){assert(C->Attached && !FreeCount);++RouteChecks;return RouteFailure?EFI_COMPROMISED_DATA:EFI_SUCCESS;}

STATIC VOID Fresh(PIANO_SPI_GPI *S) {
  ZeroMem(S,sizeof(*S));ZeroMem(Registers,sizeof(Registers));ZeroMem(EventStorage,sizeof(EventStorage));
  Clock=StopFailure=RouteFailure=FreeCount=DomainClose=CommandCount=RouteChecks=0;
  S->Started=S->Open=S->EventAllocated=TRUE;S->Instance=1;S->Frequency=1000000;S->CounterEnd=MAX_UINT64;
  S->CloseStatus=EFI_NOT_STARTED;
  S->ChannelAllocated[0]=S->ChannelAllocated[1]=TRUE;
  S->ChannelStarted[0]=S->ChannelStarted[1]=TRUE;
  S->TxRing.Signature=S->RxRing.Signature=S->EventRing.Signature=S->Tx.Signature=S->Rx.Signature=1;
  S->TxRing.Active=S->RxRing.Active=S->EventRing.Active=S->Tx.Active=S->Rx.Active=TRUE;
  S->Smmu.Domain=(VOID *)1;S->Smmu.Attached=TRUE;
  S->EventRing.Cpu=EventStorage;S->EventRing.DeviceAddress=0x40002000;
  S->TxRing.DeviceAddress=0x40000000;S->RxRing.DeviceAddress=0x40001000;
}

STATIC VOID ReadOnlyContextFields(VOID) {
  PIANO_SPI_GPI S;Fresh(&S);
  UINT32 Value=0x10010002,ReadOnly=(1U<<20)|(1U<<4);
  Registers[Register(&S,EVENT_CONTEXT)/4]=Value|ReadOnly;
  assert(Program(&S,EVENT_CONTEXT,Value,EVENT_READ_ONLY)==EFI_SUCCESS);
  assert(Registers[Register(&S,EVENT_CONTEXT)/4]==(Value|ReadOnly));
  Value=0x10000002;ReadOnly=(1U<<20)|(1U<<8)|(1U<<4);
  Registers[Register(&S,CHANNEL(1))/4]=Value|ReadOnly;
  assert(Program(&S,CHANNEL(1),Value,CHANNEL_READ_ONLY)==EFI_SUCCESS);
  assert(Registers[Register(&S,CHANNEL(1))/4]==(Value|ReadOnly));
  assert(!S.SavedCount);
  Registers[Register(&S,0x3098)/4]=2;
  assert(Program(&S,0x3098,3,0)==EFI_SUCCESS && S.SavedCount==1);
  assert(S.Saved[0].Offset==0x3098 && S.Saved[0].Value==2);
  assert(Program(&S,CHANNEL(1),Value,0)==EFI_DEVICE_ERROR);
}

int main(VOID) {
  ReadOnlyContextFields();
  PIANO_SPI_GPI S;
  UINT32 TxStorage[RING_BYTES/4],RxStorage[RING_BYTES/4];
  CONST UINT32 Lengths[]={3,8,513,5449,PIANO_SPI_GPI_MAX_BYTES};
  for(UINTN I=0;I<ARRAY_SIZE(Lengths);++I)for(UINTN Receive=0;Receive<2;++Receive) {
    Fresh(&S);S.TxRing.Cpu=TxStorage;S.RxRing.Cpu=RxStorage;
    S.Tx.DeviceAddress=0x41000000;S.Rx.DeviceAddress=0x42000000;S.Divider=2;
    TransferDescriptors(&S,(BOOLEAN)Receive,Lengths[I]);
    TRE *Tx=(TRE *)TxStorage,*Rx=(TRE *)RxStorage;
    assert(Tx[0].Word[0]==0x03000004 && Tx[0].Word[2]==2 && Tx[0].Word[3]==0x00220001);
    assert(Tx[1].Word[0]==(Receive?3U:1U) && Tx[1].Word[1]==0);
    assert(Tx[1].Word[2]==(Receive?Lengths[I]:0) && Tx[1].Word[3]==(Receive?0x00200801U:0x00200001U));
    assert(Tx[2].Word[0]==0x41000000 && !Tx[2].Word[1] && Tx[2].Word[2]==Lengths[I] && Tx[2].Word[3]==0x00100200);
    assert(Rx[0].Word[0]==(Receive?0x42000000U:0) && !Rx[0].Word[1]);
    assert(Rx[0].Word[2]==(Receive?Lengths[I]:0) && Rx[0].Word[3]==(Receive?0x00100000U:0));
  }
  Fresh(&S);StopFailure=1;
  assert(PianoGeniSpiGpiClose(&S)!=EFI_SUCCESS && S.Retained && !FreeCount && !DomainClose);
  assert(S.Tx.Active && S.Rx.Active && S.EventRing.Active && S.Smmu.Domain);
  UINT32 Calls=CommandCount;
  assert(PianoGeniSpiGpiClose(&S)==EFI_ACCESS_DENIED && CommandCount==Calls);

  Fresh(&S);
  assert(PianoGeniSpiGpiClose(&S)==EFI_SUCCESS && FreeCount==5 && DomainClose==1);
  assert(!S.Open && !S.ChannelAllocated[0] && !S.ChannelAllocated[1] && !S.EventAllocated);
  assert(RouteChecks==1);
  Calls=CommandCount;assert(PianoGeniSpiGpiClose(&S)==EFI_SUCCESS && Calls==CommandCount);

  Fresh(&S);RouteFailure=1;
  assert(PianoGeniSpiGpiClose(&S)==EFI_COMPROMISED_DATA && S.Retained);
  assert(RouteChecks==1 && !FreeCount && !DomainClose && S.Smmu.Domain);
  assert(S.Tx.Active && S.Rx.Active && S.EventRing.Active);

  Fresh(&S);UINT32 Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  TRE *Event=(TRE *)EventStorage;
  *Event=(TRE){{0x40000020,0,(2U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,1,&Done)==EFI_SUCCESS && Done==1 && S.Events==1 && S.EventRead==1);
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000020,0,(2U<<24)|5448,0x22U<<16}};
  assert(Events(&S,5449,1,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000030,0,(2U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,1,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40003000;
  assert(Events(&S,5449,1,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000020,0,(16U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,1,&Done)==EFI_DEVICE_ERROR && !Done);
  CONST UINT32 Errors[]={0x3100,0x3118};
  for(UINTN I=0;I<ARRAY_SIZE(Errors);++I) {
    Fresh(&S);Done=0;
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
    *Event=(TRE){{0x40000020,0,(2U<<24)|5449,0x22U<<16}};
    Registers[Register(&S,Errors[I])/4]=1;
    assert(Events(&S,5449,1,&Done)==EFI_DEVICE_ERROR && !Done && !S.Events);
  }
  Fresh(&S);Done=0;
  Registers[Register(&S,0x3200)/4]=0xB767BB55;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000020,0,(2U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,1,&Done)==EFI_SUCCESS && Done==1);
  for(UINT32 First=0;First<2;++First) {
    Fresh(&S);Done=0;
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
    Event[0]=(TRE){{First?0x40001000:0x40000020,0,(2U<<24)|5449,(First<<24)|(0x22U<<16)}};
    assert(Events(&S,5449,3,&Done)==EFI_SUCCESS && Done==(1U<<First) && Done!=3);
    // Hardware can fetch the RX descriptor before it writes the payload.
    Registers[Register(&S,CHANNEL(1)+0x10)/4]=0x40001010;
    assert(Events(&S,5449,3,&Done)==EFI_SUCCESS && Done!=(UINT32)3);
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002020;
    Event[1]=(TRE){{First?0x40000020:0x40001000,0,(2U<<24)|5449,((!First)<<24)|(0x22U<<16)}};
    assert(Events(&S,5449,3,&Done)==EFI_SUCCESS && Done==3);
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002030;
    Event[2]=Event[1];
    assert(Events(&S,5449,3,&Done)==EFI_COMPROMISED_DATA);
  }
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40001000,0,(2U<<24)|5448,(1U<<24)|(0x22U<<16)}};
  assert(Events(&S,5449,3,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=0;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40001000,0,(2U<<24)|5449,(1U<<24)|(0x22U<<16)}};
  assert(Events(&S,5449,1,&Done)==EFI_COMPROMISED_DATA && !Done);
  for(UINT32 Type=0;Type<2;++Type) {
    Fresh(&S);Done=0;
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
    *Event=(TRE){{0x40001000,0,(2U<<24)|5449,(1U<<24)|((Type?0xFFU:0x31U)<<16)}};
    assert(Events(&S,5449,3,&Done)==EFI_SUCCESS && !Done);
  }
  puts("GPI SPI: stop failure retains DMA; exact completions, lengths and ring bounds are required.");
  return 0;
}
