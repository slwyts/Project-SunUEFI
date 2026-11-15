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
STATIC UINT32 StopFailure,FreeCount,DomainClose,CommandCount;
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
  } else Registers[Offset/4]=Value;
  return Value;
}
EFI_STATUS PianoDmaSyncForCpuQuiet(PIANO_DMA_BUFFER *B){assert(B->Active);return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active && Quiet);B->Active=FALSE;return Status;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!B->Active);++FreeCount;ZeroMem(B,sizeof(*B));return EFI_SUCCESS;}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){assert(FreeCount==5);++DomainClose;C->Domain=NULL;return EFI_SUCCESS;}

STATIC VOID Fresh(PIANO_SPI_GPI *S) {
  ZeroMem(S,sizeof(*S));ZeroMem(Registers,sizeof(Registers));ZeroMem(EventStorage,sizeof(EventStorage));
  Clock=StopFailure=FreeCount=DomainClose=CommandCount=0;
  S->Started=S->Open=S->EventAllocated=TRUE;S->Instance=1;S->Frequency=1000000;S->CounterEnd=MAX_UINT64;
  S->CloseStatus=EFI_NOT_STARTED;
  S->ChannelAllocated[0]=S->ChannelAllocated[1]=TRUE;
  S->ChannelStarted[0]=S->ChannelStarted[1]=TRUE;
  S->TxRing.Signature=S->RxRing.Signature=S->EventRing.Signature=S->Tx.Signature=S->Rx.Signature=1;
  S->TxRing.Active=S->RxRing.Active=S->EventRing.Active=S->Tx.Active=S->Rx.Active=TRUE;
  S->Smmu.Domain=(VOID *)1;
  S->EventRing.Cpu=EventStorage;S->EventRing.DeviceAddress=0x40002000;
  S->TxRing.DeviceAddress=0x40000000;S->RxRing.DeviceAddress=0x40001000;
}

int main(VOID) {
  PIANO_SPI_GPI S;
  Fresh(&S);StopFailure=1;
  assert(PianoGeniSpiGpiClose(&S)!=EFI_SUCCESS && S.Retained && !FreeCount && !DomainClose);
  assert(S.Tx.Active && S.Rx.Active && S.EventRing.Active && S.Smmu.Domain);
  UINT32 Calls=CommandCount;
  assert(PianoGeniSpiGpiClose(&S)==EFI_ACCESS_DENIED && CommandCount==Calls);

  Fresh(&S);
  assert(PianoGeniSpiGpiClose(&S)==EFI_SUCCESS && FreeCount==5 && DomainClose==1);
  assert(!S.Open && !S.ChannelAllocated[0] && !S.ChannelAllocated[1] && !S.EventAllocated);
  Calls=CommandCount;assert(PianoGeniSpiGpiClose(&S)==EFI_SUCCESS && Calls==CommandCount);

  Fresh(&S);BOOLEAN Done=FALSE;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  TRE *Event=(TRE *)EventStorage;
  *Event=(TRE){{0x40000020,0,(2U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,&Done)==EFI_SUCCESS && Done && S.Events==1 && S.EventRead==1);
  Fresh(&S);Done=FALSE;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000020,0,(2U<<24)|5448,0x22U<<16}};
  assert(Events(&S,5449,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=FALSE;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000030,0,(2U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=FALSE;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40003000;
  assert(Events(&S,5449,&Done)==EFI_COMPROMISED_DATA && !Done);
  Fresh(&S);Done=FALSE;
  Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
  *Event=(TRE){{0x40000020,0,(16U<<24)|5449,0x22U<<16}};
  assert(Events(&S,5449,&Done)==EFI_DEVICE_ERROR && !Done);
  CONST UINT32 Errors[]={0x3100,0x3118,0x3200};
  for(UINTN I=0;I<ARRAY_SIZE(Errors);++I) {
    Fresh(&S);Done=FALSE;
    Registers[Register(&S,EVENT_CONTEXT+0x10)/4]=0x40002010;
    *Event=(TRE){{0x40000020,0,(2U<<24)|5449,0x22U<<16}};
    Registers[Register(&S,Errors[I])/4]=1;
    assert(Events(&S,5449,&Done)==EFI_DEVICE_ERROR && !Done && !S.Events);
  }
  puts("GPI SPI: stop failure retains DMA; exact completions, lengths and ring bounds are required.");
  return 0;
}
