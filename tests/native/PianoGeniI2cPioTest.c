// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual fixed PIO source with synthetic registers; never MMIO/devices.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoGeniI2cPio.c"

typedef struct {
  UINT32 Reg[0x4000/4];
  UINT8 Payload[68];
  UINT64 Now;
  UINT32 Reads,Writes,Commands,FifoWrites,FifoReads,Phase,Index,Received;
  UINT32 CancelWrites,AbortWrites,FailOffset,FailWriteNumber;
  UINT32 ReadLength,CallbackDelay;
  UINT32 WritesAtQuietFailure;
  BOOLEAN Nack,Short,Overread,UnknownIrq,Hang,CancelHang,AbortHang,Freeze,Backwards,BadRestore,ModeChanged,RxModeChanged,ForeignCommand;
  BOOLEAN FailQuiet,ArmQuietFailure,QuietFailureReady;
} HARDWARE;
static HARDWARE h;
static PIANO_GENI_I2C_PIO p;
static PIANO_GENI_PIO_CONTRACT contract;
static PIANO_GENI_PIO_IO io;
static UINT32 txcmd,rxcmd;

static UINT64 EFIAPI now(void *unused){
  if(h.Backwards && h.Phase==3)return 0;
  if(!h.Freeze)h.Now+=1;
  return h.Now;
}
static EFI_STATUS EFIAPI read32(void *unused,UINT32 offset,UINT32 *v){
  assert(!(offset&3) && offset<0x4000);++h.Reads;
  assert(offset<0x1000); // Firmware/program RAM is never touched.
  if(offset==0x40 && h.QuietFailureReady){h.WritesAtQuietFailure=h.Writes;return EFI_DEVICE_ERROR;}
  if(h.FailOffset==offset)return EFI_DEVICE_ERROR;
  // The cleanup Capture ends with lengths/M_CMD0/S_CMD0. Fail only the
  // following Quiet status read, after that capture succeeded completely.
  if(h.FailQuiet && h.Phase==3 && offset==0x270)h.ArmQuietFailure=TRUE;
  if(h.ArmQuietFailure && offset==0x630)h.QuietFailureReady=TRUE;
  if(offset==0x780){
    assert(h.Phase==3 && (h.Reg[0x804/4]&RX_WORDS));++h.FifoReads;h.Now+=h.CallbackDelay;
    *v=0;
    for(UINT32 j=0;j<4 && h.Index<h.Received;++j)*v|=(UINT32)h.Payload[h.Index++%68]<<(8*j);
    UINT32 remain=h.Received-h.Index,words=(remain+3)/4;
    h.Reg[0x804/4]=words?BIT31|((remain%4?remain%4:4)<<28)|words:0;
    return EFI_SUCCESS;
  }
  *v=h.Reg[offset/4];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write32(void *unused,UINT32 offset,UINT32 value){
  ++h.Writes;assert(!(offset&3));
  // Exactly the fixed transaction's local configuration, IRQ W1C/FIFO and
  // primary cancel/abort registers. No clocks/packing/GPIO/FW/GPI/DMA writes.
  assert(offset==0x614 || offset==0x80c || offset==0x810 || offset==0x26c ||
    offset==0x270 || offset==0x600 || offset==0x700 || offset==0x618 || offset==0x604);
  if(h.FailWriteNumber==h.Writes)return EFI_DEVICE_ERROR;
  if(offset==0x600){
    assert(value==txcmd || value==rxcmd);++h.Commands;h.Reg[offset/4]=value;
    if(value==txcmd){assert(h.Commands&1);h.Phase=1;h.Reg[0x610/4]=TX_READY;h.Reg[0x40/4]=BIT0;h.Reg[0x908/4]=0;}
    else {
      assert(!(h.Commands&1));h.Phase=3;
      h.Received=h.Short?64:h.Overread?72:h.ReadLength?h.ReadLength:68;h.Index=0;
      if(h.Hang){h.Reg[0x610/4]=0;h.Reg[0x804/4]=0;h.Reg[0x40/4]=BIT0;h.Reg[0x908/4]=0;}
      else {h.Reg[0x610/4]=h.Nack?BIT10:h.UnknownIrq?BIT20:DONE|RX_READY;
        h.Reg[0x804/4]=BIT31|((h.Received%4?h.Received%4:4)<<28)|((h.Received+3)/4);h.Reg[0x40/4]=0;h.Reg[0x908/4]=3;}
      if(h.RxModeChanged)h.Reg[0xe18/4]=1;
      if(h.ForeignCommand)h.Reg[0x600/4]^=0x200;
    }
  } else if(offset==0x700){
    assert(value==0x4c && h.Phase==1);++h.FifoWrites;h.Phase=2;
    h.Reg[0x610/4]=DONE;h.Reg[0x40/4]=0;h.Reg[0x800/4]=0;
    if(h.ModeChanged)h.Reg[0xe18/4]=1;
  } else if(offset==0x618){assert(!(value&~OWN_IRQ));h.Reg[offset/4]=value;h.Reg[0x610/4]&=~value;}
  else if(offset==0x604){
    assert(value==BIT2 || value==BIT1);
    if(value==BIT2)++h.CancelWrites;else ++h.AbortWrites;
    if((value==BIT2 && h.CancelHang) || (value==BIT1 && h.AbortHang))return EFI_SUCCESS;
    h.Reg[0x40/4]=0;h.Reg[0x908/4]=3;h.Reg[offset/4]=0;
    h.Reg[0x610/4]=value==BIT2?CANCEL:ABORT;
  } else {
    if(h.BadRestore && h.Phase==3 && offset==0x80c && value==7)value^=1;
    h.Reg[offset/4]=value;
  }
  return EFI_SUCCESS;
}
static void initial(void){
  memset(&h,0,sizeof(h));memset(&p,0,sizeof(p));h.Now=100;h.FailOffset=MAX_UINT32;
  for(UINT32 i=0;i<68;++i)h.Payload[i]=(UINT8)(i*7+3);
  h.Reg[0x68/4]=0x301;h.Reg[0x48/4]=0x11;h.Reg[0x278/4]=0x202012;
  h.Reg[0x260/4]=h.Reg[0x284/4]=PACK0;h.Reg[0x264/4]=h.Reg[0x288/4]=PACK1;
  h.Reg[0xe24/4]=h.Reg[0xe28/4]=(32U<<24)|(32U<<16);h.Reg[0x908/4]=3;
  h.Reg[0x614/4]=0x55;h.Reg[0x80c/4]=7;h.Reg[0x810/4]=8;h.Reg[0x26c/4]=23;h.Reg[0x270/4]=29;
  contract=(PIANO_GENI_PIO_CONTRACT){.Firmware=0x301,.MasterClock=0x11,.SclCounters=0x202012,
    .ExplicitEnable=TRUE,.MappedDeviceMemoryVerified=TRUE,.ExclusiveOwnerVerified=TRUE,.ExistingRuntimeVerified=TRUE,
    .ClockRateAndMuxVerified=TRUE,.SupplyAndGpioVerified=TRUE,.FirmwareAndPackingVerified=TRUE,.BoundedCallbacksVerified=TRUE};
  io=(PIANO_GENI_PIO_IO){.Base=PIANO_GENI_SE6_BASE,.Bytes=PIANO_GENI_SE6_BYTES,.Read32=read32,.Write32=write32,.NowUs=now};
  PIANO_GENI_RUNTIME_PACKET packet;assert(PianoGeniI2cPioBuildRuntimePacket(&packet)==EFI_SUCCESS);
  txcmd=packet.TxCommand;rxcmd=packet.RxCommand;
}
static EFI_STATUS run(void){
  UINT8 frame[68];memset(frame,0xa5,sizeof(frame));UINTN n=123;
  EFI_STATUS r=PianoGeniI2cPioReadRuntime(&p,h.Now+10000,frame,&n);
  if(r==EFI_SUCCESS){assert(n==68 && !memcmp(frame,h.Payload,68) && p.Clean && !p.Quarantined);}
  else {assert(n==0);for(UINT32 i=0;i<68;++i)assert(!frame[i]);}
  return r;
}
static void start(void){assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_SUCCESS && !h.Writes && !h.Commands);}
static void failed(EFI_STATUS expected,BOOLEAN quarantined){
  EFI_STATUS actual=run();
  if(actual!=expected || !p.Failed || p.Quarantined!=quarantined)
    fprintf(stderr,"PIO fixture failure expected=%lx actual=%lx qr=%u/%u cmds=%u phase=%u lost=%u cleanup=%lx\n",
      (unsigned long)expected,(unsigned long)actual,quarantined,p.Quarantined,h.Commands,h.Phase,p.OwnershipLost,(unsigned long)p.CleanupStatus);
  assert(actual==expected && p.Failed && p.Quarantined==quarantined);
  assert(p.Polls<=PIANO_GENI_POLL_LIMIT+1 && p.CleanupPolls<=PIANO_GENI_CLEANUP_LIMIT+1);
  unsigned reads=h.Reads,writes=h.Writes;assert(run()==EFI_ABORTED && h.Reads==reads && h.Writes==writes);
  assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_ALREADY_STARTED);
}
int main(void){
  initial();PIANO_GENI_RUNTIME_PACKET packet;
  assert(PianoGeniI2cPioBuildRuntimePacket(NULL)==EFI_INVALID_PARAMETER);
  assert(PianoGeniI2cPioBuildRuntimePacket(&packet)==EFI_SUCCESS && packet.TxLength==1 && packet.TxWord==0x4c && packet.RxLength==68);
  assert(packet.TxCommand==0x08009804 && packet.RxCommand==0x10009800);
  PIANO_GENI_PIO_SNAPSHOT snap;assert(PianoGeniI2cPioCapture(&io,&snap)==EFI_SUCCESS && !h.Writes && !h.FifoReads);
  assert(PianoGeniI2cPioValidate(&contract,&snap)==EFI_SUCCESS);
  if(!PIANO_GENI_I2C_PIO_EXPERIMENT){
    unsigned reads=h.Reads;assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_UNSUPPORTED);
    assert(run()==EFI_UNSUPPORTED && h.Reads==reads && !h.Writes && !h.Commands);
    puts("GENI PIO default-off: pure fixed packet/read-only snapshot; no transaction callbacks invoked.");return 0;
  }
  for(unsigned bit=0;bit<8;++bit){
    initial();BOOLEAN *gate[]={&contract.ExplicitEnable,&contract.MappedDeviceMemoryVerified,&contract.ExclusiveOwnerVerified,
      &contract.ExistingRuntimeVerified,&contract.ClockRateAndMuxVerified,&contract.SupplyAndGpioVerified,
      &contract.FirmwareAndPackingVerified,&contract.BoundedCallbacksVerified};*gate[bit]=FALSE;
    assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_NOT_READY && !h.Reads && !h.Writes);
  }
  initial();start();assert(run()==EFI_SUCCESS && h.Commands==2 && h.FifoWrites==1 && h.FifoReads==17);
  assert(h.Reg[0x614/4]==0x55 && h.Reg[0x80c/4]==7 && h.Reg[0x810/4]==8 && h.Reg[0x26c/4]==23 && h.Reg[0x270/4]==29);
  assert(!p.BusMayBeHeld && p.CleanupStatus==EFI_SUCCESS && !p.Failed);
  initial();start();h.Short=TRUE;failed(EFI_BAD_BUFFER_SIZE,FALSE);
  initial();start();h.ReadLength=66;failed(EFI_BAD_BUFFER_SIZE,FALSE);assert(p.Received==66 && h.FifoReads==17);
  initial();start();h.Overread=TRUE;failed(EFI_BAD_BUFFER_SIZE,FALSE);
  initial();start();h.Nack=TRUE;failed(EFI_DEVICE_ERROR,FALSE);
  initial();start();h.Nack=h.FailQuiet=TRUE;failed(EFI_DEVICE_ERROR,TRUE);
  assert(h.QuietFailureReady && h.Writes==h.WritesAtQuietFailure && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.UnknownIrq=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  assert(p.OwnershipLost && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.ModeChanged=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  assert(h.Commands==1 && p.OwnershipLost && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.RxModeChanged=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  assert(h.Commands==2 && p.OwnershipLost && !h.FifoReads && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.ForeignCommand=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  assert(p.OwnershipLost && !h.FifoReads && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.Hang=TRUE;failed(EFI_TIMEOUT,FALSE);assert(h.CancelWrites==1 && !h.AbortWrites);
  initial();start();h.Hang=h.CancelHang=TRUE;failed(EFI_TIMEOUT,FALSE);assert(h.CancelWrites==1 && h.AbortWrites==1);
  initial();start();h.Hang=h.CancelHang=h.AbortHang=TRUE;failed(EFI_TIMEOUT,TRUE);
  initial();start();h.Hang=h.Freeze=TRUE;failed(EFI_TIMEOUT,FALSE);assert(p.Polls==PIANO_GENI_POLL_LIMIT+1);
  initial();start();h.Backwards=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  initial();start();h.BadRestore=TRUE;failed(EFI_COMPROMISED_DATA,TRUE);
  initial();start();h.FailWriteNumber=5;failed(EFI_DEVICE_ERROR,FALSE);assert(p.CommandsAttempted==1 && !h.Commands);
  initial();start();h.FailOffset=0x780;failed(EFI_DEVICE_ERROR,TRUE);
  initial();start();h.CallbackDelay=10000;failed(EFI_TIMEOUT,TRUE);
  assert(h.FifoReads==1 && p.Received==0 && !h.CancelWrites && !h.AbortWrites);
  initial();start();h.Reg[0x68/4]=0;failed(EFI_COMPROMISED_DATA,TRUE);assert(!h.Writes);
  initial();start();h.Reg[0x40/4]=BIT12;failed(EFI_NOT_READY,TRUE);assert(!h.Writes);
  initial();start();p.InRead=TRUE;assert(run()==EFI_NOT_READY && !h.Writes);p.InRead=FALSE;p.Attempts=64;assert(run()==EFI_OUT_OF_RESOURCES && !h.Writes);
  initial();io.Base++;assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_INVALID_PARAMETER && !h.Reads);
  initial();io.Bytes-=4;assert(PianoGeniI2cPioCapture(&io,&snap)==EFI_INVALID_PARAMETER && !h.Reads);
  const UINT32 offsets[]={0x68,0x64,0x258,0xe18,0xe1c,0x254,0x260,0x288,0x908,0x800,0x804,0x604,0x634,0x640};
  for(unsigned i=0;i<ARRAY_SIZE(offsets);++i){
    initial();h.Reg[offsets[i]/4]^=1;
    assert(PianoGeniI2cPioInitialize(&p,&io,&contract)!=EFI_SUCCESS && !h.Writes);
  }
  initial();h.FailOffset=0x68;assert(PianoGeniI2cPioInitialize(&p,&io,&contract)==EFI_DEVICE_ERROR && !h.Writes);
  puts("GENI PIO opt-in actual source: fixed repeated-start read68, all gates, fresh firmware/ownership, short/overread/NACK, timeout/cancel/abort, frozen/backward clock, quarantine and restore proof passed.");
  return 0;
}
