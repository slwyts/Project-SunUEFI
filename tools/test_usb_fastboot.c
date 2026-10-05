// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual firmware source with fake MMIO/shared-DMA; no USB/device access.
#define PIANO_USB_FASTBOOT 1
static void *test_console;
#define PIANO_USB_CONSOLE_BASE ((UINTN)test_console)
#define main session_test_main
#include "test_usb_session.c"
#undef main
#include <openssl/sha.h>
#include "../bootprofiles/uefi-app/PianoFastboot.c"
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *A,UINTN N,UINT8 *H){return SHA256(A,N,H)!=NULL;}
static UINT8 wire[70000];static UINTN wire_bytes,wire_packets;
static PIANO_DMA_BUFFER *all[]={&mRing,&mTrbs[0],&mTrbs[1],&mSetup,&mTx,&mTrbs[2],&mTrbs[3],&mBulkRx,&mBulkTx};
static VOID model_init(VOID) {
  PIANO_DMA_DEVICE Device={0};init();gBS=&bs;bs.Stall=stall;gRT=&rt;rt.ResetSystem=reset;
  ZeroMem(&mControl,sizeof(mControl));ZeroMem(mPending,sizeof(mPending));ZeroMem(mEnding,sizeof(mEnding));ZeroMem(mPayload,sizeof(mPayload));
  mControl.Speed=4;mControl.SuperSpeed=TRUE;mConfigured=mBulkLive=mBulkPrepared=mConfigWaiting=mStatusWaiting=FALSE;
  ClearFastboot();PianoFastbootInit(&mFastboot,NULL,FastbootSend,NULL);mFastboot.Query=FastbootQuery;mFastboot.Diagnostic=FastbootDiagnostic;
  for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(PianoDmaAllocate(&Device,4096,4096,32,PianoDmaBidirectional,all[I])==EFI_SUCCESS);
  assert(ArmSetup()==EFI_SUCCESS);setup_packet(5,1);status_complete();
}
static VOID configure(VOID) {
  setup_packet(9,1);assert(mBulkPrepared && !mBulkLive && !mPending[2]);
  status_complete();assert(mConfigured && mBulkLive && mPending[2] && regs[0xC720/4]==15);
}
static VOID out(CONST VOID *Data,UINTN Bytes) {
  assert(mPending[2] && !mPending[3] && !mFrames && Bytes<=mPosted[2]);
  CopyMem(mBulkRx.Cpu,Data,Bytes);DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)Bytes;T->Control&=~BIT0;
  assert(Event(0xC044)==EFI_SUCCESS);
}
static VOID drain(VOID) {
  wire_bytes=wire_packets=0;
  while(mPending[3]) {
    assert(!mPending[2]);UINTN Bytes=mPosted[3];assert(wire_bytes+Bytes<=sizeof(wire));
    CopyMem(wire+wire_bytes,mBulkTx.Cpu,Bytes);wire_bytes+=Bytes;++wire_packets;
    DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;assert(Event(0xC046)==EFI_SUCCESS);
  }
  assert(!mFrames);
}
static VOID cmd(CONST CHAR8 *Text,CONST CHAR8 *Expected) {
  out(Text,strlen(Text));assert(mPending[3] && !mPending[2]);drain();
  assert(wire_bytes==strlen(Expected) && !memcmp(wire,Expected,wire_bytes));
}
static VOID end(UINT8 Ep) {assert(Event((8U<<24)|(7U<<6)|((UINT32)Ep<<1))==EFI_SUCCESS);}
static VOID descriptors(VOID) {
  UINT8 U[]={0x80,6,0,2,0,0,255,0},Data[256];UINTN Bytes;PIANO_USB_CONTROL_ACTION Action;
  for(UINT8 Speed=0;Speed<6;++Speed) {
    if(Speed==2)continue;
    mControl.Speed=Speed;mControl.SuperSpeed=Speed>=4;
    assert(PianoUsbControlSetup(&mControl,U,Data,sizeof(Data),&Bytes,&Action)==EFI_SUCCESS);
    assert(Bytes==(Speed>=4?44:32) && Data[2]==Bytes && Data[13]==2 && Data[14]==0xff && Data[15]==0x42 && Data[16]==3);
    assert(Data[20]==1 && Data[21]==2 && (Data[22]|Data[23]<<8)==(Speed>=4?1024:(Speed==0?512:64)));
    UINTN In=Speed>=4?31:25;assert(Data[In+2]==0x81 && (UINT32)(Data[In+4]|Data[In+5]<<8)==BulkMps());
    if(Speed>=4)assert(Data[25]==6 && Data[26]==48 && Data[38]==6 && Data[39]==48);
  }
  mControl.Speed=4;mControl.SuperSpeed=TRUE;
}
static UINTN lifecycle_stage;
static BOOLEAN lifecycle_continue,lifecycle_hold_ack;
static VOID publish(UINT32 EventValue) {
  assert(mRing.Active && regs[0xC40C/4]==0);
  ((UINT32 *)mRing.Cpu)[mRingPosition/4]=EventValue;regs[0xC40C/4]=4;
}
static VOID lifecycle_events(UINTN Us) {
  if(Us!=1000 || !mRing.Active || !(regs[0xC704/4]&BIT31) || (lifecycle_stage==0 && !mPending[0]))return;
  DWC_TRB *T;
  switch(lifecycle_stage) {
    case 0:case 3: {
      assert(mPhase==0);UINT8 U[]={0,lifecycle_stage==0?5:9,1,0,0,0,0,0};CopyMem(mSetup.Cpu,U,8);
      T=mTrbs[0].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC040);break;
    }
    case 1:case 4:assert(mPhase==2);publish(0x20C2);break;
    case 2:case 5:
      assert(mPhase==3 && mPending[1]);T=mTrbs[1].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC042);break;
    case 6: {
      assert(mConfigured && mPending[2]);CONST CHAR8 *Cmd=lifecycle_continue?"continue":"reboot";UINTN Bytes=strlen(Cmd);
      CopyMem(mBulkRx.Cpu,Cmd,Bytes);T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)Bytes;T->Control&=~BIT0;publish(0xC044);break;
    }
    case 7:
      assert(mPending[3] && mFrameCount==1 && !mPending[2] && !memcmp(mBulkTx.Cpu,"OKAY",4));
      assert(!PianoDwc3ConsumeRebootRequest()); // Enqueued OKAY is not an acknowledgement.
      if(lifecycle_hold_ack)return;
      T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC046);break;
    default:assert(FALSE);
  }
  ++lifecycle_stage;
}
static VOID device_reboot_lifecycle(VOID) {
  for(UINTN Case=0;Case<4;++Case) {
    PIANO_OWNED_SMMU Context={0};PIANO_DMA_DEVICE Device={0};init();
    lifecycle_stage=0;lifecycle_continue=Case==1;lifecycle_hold_ack=Case==2;fail_dma_free=Case==3;stall_hook=lifecycle_events;
    EFI_STATUS S=PianoDwc3Ep0Experiment(&Context,&Device);assert(S==(Case==3?EFI_DEVICE_ERROR:EFI_SUCCESS));
    assert(!resets && quiet_syncs>=7 && session_writes==4); // Device never performs normal reset.
    assert(PianoDwc3ConsumeRebootRequest()==(Case==0));assert(!PianoDwc3ConsumeRebootRequest());
    assert(!mFrames && !mFastboot.Download && !mFastboot.Upload);
    assert(lifecycle_stage==(Case==2?7:8));
    for(UINTN I=0;I<ARRAY_SIZE(all);++I) {
      assert(!all[I]->Active);
      if(Case==3){assert(all[I]->Signature);free(all[I]->Cpu);ZeroMem(all[I],sizeof(*all[I]));}
      else assert(!all[I]->Signature);
    }
  }
  stall_hook=NULL;fail_dma_free=FALSE;
}
#ifndef USB_FASTBOOT_TEST_MAIN
#define USB_FASTBOOT_TEST_MAIN main
#endif
int USB_FASTBOOT_TEST_MAIN(void) {
  model_init();descriptors();configure();
  assert(((regs[0xC838/4]>>17)&31)==1 && ((regs[0xC828/4]>>17)&31)==0);
  cmd("getvar:product","OKAYpiano-sunuefi");cmd("getvar:version","OKAY0.4");
  cmd("getvar:SunUEFI:usb-state","OKAYconfigured-speed-S");
#if !PIANO_USB_SCREENSHOT
  cmd("oem screenshot","FAILdiagnostic command unavailable");
#endif
  cmd("flash:boot_a","FAILcommand disabled by RAM-only policy");
  UINT8 Oversize[65];memset(Oversize,'A',sizeof(Oversize));out(Oversize,sizeof(Oversize));drain();
  assert(wire_bytes==20 && !memcmp(wire,"FAILcommand too long",20));
  UINT8 Pattern[65553];for(UINTN I=0;I<sizeof(Pattern);++I)Pattern[I]=(UINT8)(I*73+I/251+19);
  cmd("download:00010011","DATA00010011");assert(mFastboot.Receiving && mPosted[2]==4096);
  out(Pattern,0);assert(mFastboot.Receiving && !mFrames);
  for(UINTN Offset=0;Offset<sizeof(Pattern);) {
    UINTN Bytes=MIN((UINTN)sizeof(Pattern)-Offset,(UINTN)4096);out(Pattern+Offset,Bytes);Offset+=Bytes;
  }
  assert(mFastboot.Complete && mPending[3] && !mPending[2]);drain();assert(wire_bytes==4 && !memcmp(wire,"OKAY",4));
  out("oem sha256",10);drain();assert(wire_packets==3 && wire_bytes==76 && !memcmp(wire,"INFO",4) && !memcmp(wire+36,"INFO",4));
  out("upload",6);assert(mFrameCount==3 && mFrameBytes==sizeof(Pattern)+16);drain();
  assert(wire_bytes==sizeof(Pattern)+16 && !memcmp(wire,"DATA00010011",12) && !memcmp(wire+12,Pattern,sizeof(Pattern)) && !memcmp(wire+12+sizeof(Pattern),"OKAY",4));
  // Freeze a real DBGC-shaped console; later writes must not mutate upload.
  UINT32 *Header=calloc(1,0x200000);test_console=Header;
  assert(Header);CONST CHAR8 *Log="SUNUEFI_RAMLOG_BEGIN\nfastboot console\n";
  Header[0]=0x43474244;Header[1]=Header[2]=(UINT32)strlen(Log);CopyMem(Header+3,Log,strlen(Log));
  cmd("oem ramlog","OKAY");UINT32 LogBytes=mFastLogBytes,LogCrc=mFastLogCrc,Generation=mFastLogGeneration;
  assert(LogBytes==strlen(Log) && LogCrc==DiagCrc((CONST UINT8 *)Log,LogBytes) && Generation);
  memset(Header+3,'X',LogBytes);CHAR8 ExpectedSize[13]="OKAY";FastHex(LogBytes,ExpectedSize+4);cmd("getvar:SunUEFI:log-size",ExpectedSize);
  out("upload",6);drain();assert(wire_bytes==LogBytes+16 && !memcmp(wire+12,Log,LogBytes));
  Header[1]=Header[2]+1;cmd("oem ramlog","FAILRAM log snapshot unavailable");assert(mFastLogBytes==0);
  // IN's DMA copy stays intact while config0 zeros/frees all CPU-owned data.
  out("getvar:version",14);assert(mPending[3]);UINT8 Active[7];CopyMem(Active,mBulkTx.Cpu,7);
  setup_packet(9,0);assert(mEnding[3] && mPending[3] && mBulkTx.Active && !mFrames && !mFastboot.Download && !mFastboot.Upload);
  assert(!memcmp(Active,mBulkTx.Cpu,7) && regs[0xC720/4]==3 && mConfigWaiting);
  assert(Event(0x20C2)==EFI_SUCCESS && mStatusWaiting && mPhase==2);
  end(3);assert(!mPending[3] && !mBulkTx.Active && !mConfigWaiting && mPhase==3);
  DWC_TRB *Status=mTrbs[1].Cpu;Status->Control&=~BIT0;assert(Event(0xC042)==EFI_SUCCESS);
  assert(!mConfigured && mControl.Configuration==0 && !mBulkLive);
  configure();assert(mPending[2]);assert(Event(0x101)==EFI_SUCCESS);
  assert(!mConfigured && mControl.Configuration==0 && mEnding[2] && mBulkRx.Active && !mFastboot.Upload);
  end(2);assert(!mBulkRx.Active && !mPending[2]);
  setup_packet(5,1);status_complete();configure();
  assert(Event(1)==EFI_SUCCESS);assert(!mConfigured && mEnding[2] && !mBulkLive && !mPending[0]);end(2);
  ClearFastboot();for(UINTN I=0;I<ARRAY_SIZE(all);++I){if(all[I]->Active)assert(PianoDmaComplete(all[I],EFI_SUCCESS,TRUE)==EFI_SUCCESS);assert(PianoDmaFree(all[I])==EFI_SUCCESS);}
  if(mLogSnapshot){ZeroMem(mLogSnapshot,USB_DIAG_LOG_BYTES);FreePool(mLogSnapshot);mLogSnapshot=NULL;}free(Header);test_console=NULL;
  device_reboot_lifecycle();
  puts("USB fastboot actual source: FS/HS/SS descriptors/FIFO, standard stage/SHA256/upload 65553-byte roundtrip, frozen ramlog, Start<=Size, response serialization, config0 asynchronous DMA retirement, reset/disconnect passed.");
  puts("USB device reboot actual source: full event-loop SETUP/config/reboot/IN ACK/cleanup, one-shot consumption, continue return, missing ACK and failed DMA free suppress request; no device-layer reset passed.");
  return 0;
}
