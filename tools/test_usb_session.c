// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host MMIO/DMA model for the actual EP0 experiment; never accesses a device.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoDwc3Device.c"
#include "../bootprofiles/uefi-app/PianoUsbControl.c"

EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;
static EFI_RUNTIME_SERVICES rt;
static UINT32 regs[0x100000/4];
static UINTN session_writes,allocations,completions,resets;
static UINTN quiet_syncs;
static UINTN address_writes,address_status_starts;
static BOOLEAN ran,failed_halt,reject_session,fail_setup,fail_allocate;
static BOOLEAN fail_dma_free;
static VOID (*stall_hook)(UINTN Us);
static jmp_buf failed_reset_return;
static CONST UINT32 original_hs=0x405a55a5,original_ss=0x8055a55a;

BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
VOID EFIAPI CpuDeadLoop(VOID){assert(resets==1 && failed_halt);longjmp(failed_reset_return,1);}
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Bytes){return memset(Buffer,0,Bytes);}
VOID *EFIAPI CopyMem(VOID *Dest,CONST VOID *Source,UINTN Bytes){return memmove(Dest,Source,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
VOID *EFIAPI AllocatePool(UINTN Bytes){return malloc(Bytes);}
VOID EFIAPI FreePool(VOID *Buffer){free(Buffer);}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *Snapshot){ }

UINT32 EFIAPI MmioRead32(UINTN Address) {
  assert(Address>=DW && Address<DW+sizeof(regs) && !(Address&3));
  UINT32 Offset=(UINT32)(Address-DW);
  if(Offset==0xC70C)
    return 0x00920004|((regs[0xC704/4]&BIT31) || (ran && failed_halt)?0:BIT22);
  return regs[Offset/4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(Address>=DW && Address<DW+sizeof(regs) && !(Address&3));
  UINT32 Offset=(UINT32)(Address-DW);
  if(Offset==0xC40C){assert(Value<=regs[Offset/4]);regs[Offset/4]-=Value;return Value;}
  if(Offset==0xC700)++address_writes;
  if(Offset==USB_SESSION_HS || Offset==USB_SESSION_SS) {
    if(session_writes<2) {
      assert(mRing.Active && regs[0xC720/4]==3);
      assert(!(regs[0xC704/4]&BIT31));
    } else {
      assert(MmioRead32(DW+0xC70C)&BIT22);
    }
    ++session_writes;
    if(reject_session && Offset==USB_SESSION_HS && Value!=original_hs)Value&=~BIT20;
  }
  if(Offset==0xC704) {
    if(Value&BIT31) {
      assert(regs[USB_SESSION_HS/4]==(original_hs|USB_SESSION_HS_VALID));
      assert(regs[USB_SESSION_SS/4]==(original_ss|USB_SESSION_SS_PRESENT));
      ran=TRUE;
    }
    Value&=~BIT30; // Device reset completes immediately in the model.
  }
  if(Offset>=0xC80C && Offset<=0xC83C && (Offset&15)==12) {
    if(Offset==0xC81C && (Value&15)==6 && mPhase==3 && mControl.SetAddress) {
      // Replays test 65: the core samples DevAddr before starting STATUS.
      assert(((regs[0xC700/4]>>3)&127)==mControl.PendingAddress);
      ++address_status_starts;
    }
    if(fail_setup && (Value&15)==6)Value|=BIT12;
    Value&=~BIT10; // Synchronous command completion.
  }
  regs[Offset/4]=Value;
  return Value;
}
static EFI_STATUS EFIAPI stall(UINTN Us){if(stall_hook!=NULL)stall_hook(Us);return EFI_SUCCESS;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data) {
  assert(Type==EfiResetCold && Status==EFI_TIMEOUT);++resets;
}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *Device,UINTN Bytes,UINTN Alignment,
                           UINT8 Bits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *Buffer) {
  if(fail_allocate)return EFI_OUT_OF_RESOURCES;
  *Buffer=(PIANO_DMA_BUFFER){.Signature=1,.Cpu=calloc(1,Bytes),.Bytes=Bytes,
    .DeviceAddress=0x40000000+allocations*4096};
  assert(Buffer->Cpu);++allocations;return EFI_SUCCESS;
}
EFI_STATUS PianoDmaMap(PIANO_DMA_BUFFER *Buffer){Buffer->Mapped=TRUE;return EFI_SUCCESS;}
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *Buffer,CONST CHAR8 *Name){Buffer->Active=TRUE;return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *Buffer,EFI_STATUS Status,BOOLEAN Quiet) {
  BOOLEAN Proof=(MmioRead32(DW+0xC70C)&BIT22)!=0;
  for(UINTN Ep=0;Ep<ARRAY_SIZE(mTrbs);++Ep)
    if(mTrbs[Ep].Cpu!=NULL && (Buffer==&mTrbs[Ep] || Buffer==mPayload[Ep]) && !(((DWC_TRB *)mTrbs[Ep].Cpu)->Control&BIT0))Proof=TRUE;
  assert(Quiet && Proof);
  Buffer->Active=FALSE;++completions;return EFI_SUCCESS;
}
EFI_STATUS PianoDmaSyncForCpu(PIANO_DMA_BUFFER *Buffer){assert(FALSE);return EFI_DEVICE_ERROR;}
EFI_STATUS PianoDmaSyncForCpuQuiet(PIANO_DMA_BUFFER *Buffer){assert(Buffer==&mRing && Buffer->Active);++Buffer->QuietSyncs;++quiet_syncs;return EFI_SUCCESS;}
EFI_STATUS PianoDmaReportQuietSync(PIANO_DMA_BUFFER *Buffer){(void)Buffer;return EFI_SUCCESS;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer) {
  if(fail_dma_free)return EFI_DEVICE_ERROR;
  assert(!Buffer->Active);free(Buffer->Cpu);ZeroMem(Buffer,sizeof(*Buffer));return EFI_SUCCESS;
}
static VOID init(VOID) {
  memset(regs,0,sizeof(regs));regs[0xC704/4]=0x10F00000;
  regs[USB_SESSION_HS/4]=original_hs;regs[USB_SESSION_SS/4]=original_ss;
  session_writes=allocations=completions=resets=0;
  quiet_syncs=0;
  address_writes=address_status_starts=0;
  ran=failed_halt=reject_session=fail_setup=fail_allocate=FALSE;
  fail_dma_free=FALSE;stall_hook=NULL;
}
static VOID restored(VOID) {
  assert(session_writes==4 && regs[USB_SESSION_HS/4]==original_hs && regs[USB_SESSION_SS/4]==original_ss);
  assert(!mRing.Active && !mSetup.Active && !mTrbs[0].Active && resets==0);
}
static VOID setup_packet(UINT8 Request,UINT16 Value) {
  UINT8 packet[8]={0,Request,(UINT8)Value,(UINT8)(Value>>8),0,0,0,0};
  assert(mPending[0] && mPhase==0);CopyMem(mSetup.Cpu,packet,sizeof(packet));
  DWC_TRB *trb=mTrbs[0].Cpu;trb->Size=0;trb->Control&=~BIT0;
  assert(Event(0xC040)==EFI_SUCCESS); // SETUP completion, as in test 65.
}
static VOID status_complete(VOID) {
  assert(Event(0x20C2)==EFI_SUCCESS); // EP1 STATUS XferNotReady.
  DWC_TRB *trb=mTrbs[1].Cpu;
  assert(mPending[1] && mPhase==3 && !trb->Size && ((trb->Control>>4)&63)==3);
  trb->Control&=~BIT0;assert(Event(0xC042)==EFI_SUCCESS);
}
static VOID address_sequence(PIANO_DMA_DEVICE *Device) {
  init();ZeroMem(&mControl,sizeof(mControl));mControl.SuperSpeed=TRUE;
  ZeroMem(mPending,sizeof(mPending));ZeroMem(mPayload,sizeof(mPayload));
  PIANO_DMA_BUFFER *buffers[]={&mTrbs[0],&mTrbs[1],&mSetup,&mTx};
  for(UINTN I=0;I<ARRAY_SIZE(buffers);++I)
    assert(PianoDmaAllocate(Device,4096,4096,32,PianoDmaBidirectional,buffers[I])==EFI_SUCCESS);
  CONST UINT32 original_cfg=0x00600804;regs[0xC700/4]=original_cfg;
  assert(ArmSetup()==EFI_SUCCESS);
  CONST UINT8 addresses[]={17,0,127};
  for(UINTN I=0;I<ARRAY_SIZE(addresses);++I) {
    UINT8 old=mControl.Address;UINTN before=address_writes;
    setup_packet(5,addresses[I]);
    assert(address_writes==before+1 && mControl.Address==old && mControl.SetAddress && mPhase==2);
    assert(regs[0xC700/4]==((original_cfg&~(127U<<3))|((UINT32)addresses[I]<<3)));
    status_complete();
    assert(address_writes==before+1 && mControl.Address==addresses[I] && !mControl.SetAddress);
  }
  assert(address_status_starts==3);
  UINTN before=address_writes;
  setup_packet(5,128); // Rejected request must not program DevAddr.
  assert(address_writes==before && !mControl.SetAddress && mControl.Address==127 && mPhase==0);
  setup_packet(9,1);assert(mControl.Configuration==0);
  status_complete();assert(mControl.Configuration==1 && address_writes==before);
  assert(Event(0x101)==EFI_SUCCESS); // A bus reset clears both pending/software state and hardware address.
  assert(mControl.Address==0 && mControl.Configuration==0 && !mControl.SetAddress);
  assert((regs[0xC700/4]&(127U<<3))==0 && mPending[0] && mPhase==0);
  assert(StopTransfer(0)==EFI_SUCCESS);
  for(UINTN I=0;I<ARRAY_SIZE(buffers);++I)assert(PianoDmaFree(buffers[I])==EFI_SUCCESS);
}
int main(void) {
  PIANO_OWNED_SMMU context={0};PIANO_DMA_DEVICE device={0};
  gBS=&bs;bs.Stall=stall;gRT=&rt;rt.ResetSystem=reset;
  init();assert(PianoDwc3Ep0Experiment(&context,&device)==EFI_NOT_READY);restored();assert(ran);
  init();reject_session=TRUE;
  assert(PianoDwc3Ep0Experiment(&context,&device)==EFI_DEVICE_ERROR);restored();assert(!ran);
  init();fail_setup=TRUE;
  assert(PianoDwc3Ep0Experiment(&context,&device)==EFI_DEVICE_ERROR);restored();assert(ran);
  init();fail_allocate=TRUE;
  assert(PianoDwc3Ep0Experiment(&context,&device)==EFI_OUT_OF_RESOURCES);
  assert(session_writes==0 && allocations==0 && !ran);
  init();failed_halt=TRUE;
  if(!setjmp(failed_reset_return)){PianoDwc3Ep0Experiment(&context,&device);assert(FALSE);}
  assert(resets==1 && session_writes==2 && completions==0);
  assert(regs[USB_SESSION_HS/4]==(original_hs|USB_SESSION_HS_VALID));
  assert(regs[USB_SESSION_SS/4]==(original_ss|USB_SESSION_SS_PRESENT));
  assert(mRing.Active && mTrbs[0].Active && mSetup.Active);
  // The reset mock returns only for this host test; release retained model memory.
  PIANO_DMA_BUFFER *buffers[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx};
  for(UINTN I=0;I<ARRAY_SIZE(buffers);++I){free(buffers[I]->Cpu);ZeroMem(buffers[I],sizeof(*buffers[I]));}
  address_sequence(&device);
  puts("USB session: original bits preserved, session before RUN_STOP, readback rejection, error cleanup, and no restore/free before successful halt passed.");
  puts("USB address: test 65 SETUP/STATUS event order, DevAddr before STATUS, delayed software commit, 0/127 limits, rejected 128, configuration and reset passed.");
  return 0;
}
