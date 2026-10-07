// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual command + DWC source. No Controller, Launch, hardware or profile.
#define PIANO_USB_RAM_BOOT 1
#define USB_FASTBOOT_TEST_MAIN old_bulk_source_suite
#include "test_usb_fastboot.c"
#include "../../uefi/core/PianoFastbootBoot.c"
static UINT8 payload[1024];static UINT8 *captured;
static UINTN boot_stage,ready_calls,validate_calls,take_calls;
static UINTN test_case;
static VOID put16(UINTN O,UINT16 V){payload[O]=(UINT8)V;payload[O+1]=(UINT8)(V>>8);}
static VOID put32(UINTN O,UINT32 V){for(UINTN I=0;I<4;++I)payload[O+I]=(UINT8)(V>>(8*I));}
static VOID fixture(VOID) {
  ZeroMem(payload,sizeof(payload));put16(0,0x5a4d);put32(60,128);put32(128,0x4550);put16(132,0xaa64);put16(134,1);put16(148,240);put16(150,2);
  put16(152,0x20b);put32(168,4096);put32(184,4096);put32(188,512);put32(208,8192);put32(212,512);put16(220,10);put32(260,16);
  put32(400,512);put32(404,4096);put32(408,512);put32(412,512);put32(428,0x60000020);
}
static EFI_STATUS boot_ready(VOID *Context){assert(Context==(VOID *)123);++ready_calls;return test_case==20?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS boot_validate(VOID *Context,CONST PIANO_FASTBOOT *S,CONST PIANO_FB_BOOT_VIEW *V) {
  assert(Context==(VOID *)123 && S->Download && V->Bytes==sizeof(payload) && V->ImageBytes==8192);++validate_calls;
  if(test_case==21 || (test_case==9 && S->BootTransferFrozen))return EFI_SECURITY_VIOLATION;
  return EFI_SUCCESS;
}
static EFI_STATUS boot_take(VOID *Context,PIANO_FASTBOOT *S,CONST PIANO_FB_BOOT_VIEW *V,VOID **Token) {
  assert(Context==(VOID *)123 && S==&mFastboot && V->Bytes==1024 && validate_calls==2 && boot_stage==12);++take_calls;
  assert(S->BootProof.AckCompleted && S->BootProof.QueueEmpty && S->BootProof.DeviceHalted && S->BootProof.DmaFreed && S->BootProof.DispatchFrozen && S->BootProof.AckBytes==4 && S->BootProof.DmaBuffersFreed==9);
  assert(!mFrames && !mPending[3] && (Dr(0xC70C)&BIT22));for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(!all[I]->Signature);
  UINT8 *Before=S->Download;PianoFastbootReset(S);assert(S->Download==Before && S->BootTransferFrozen);
  assert(PianoFastbootPacket(S,"getvar:version",14)==EFI_NOT_READY);
  if(test_case==2)return EFI_DEVICE_ERROR;
  if(test_case==5){*Token=(VOID *)789;return EFI_SUCCESS;}
  captured=S->Download;S->Download=S->Upload=NULL;S->Expected=S->Received=S->UploadBytes=0;S->UploadBorrowed=S->Receiving=S->Complete=FALSE;
  if(test_case!=4)*Token=(VOID *)789;
  return test_case==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static PIANO_FB_BOOT backend={.Context=(VOID *)123,.MaxImageBytes=65536,.Ready=boot_ready,.Validate=boot_validate,.TakeAfterAck=boot_take};
static VOID tick_boot(UINTN Us) {
  if(Us!=1000 || !mRing.Active || !(regs[0xC704/4]&BIT31) || (boot_stage==0 && !mPending[0]))return;
  if(boot_stage<6){lifecycle_stage=boot_stage;lifecycle_events(Us);boot_stage=lifecycle_stage;return;}
  DWC_TRB *T;
  if(boot_stage==6 || boot_stage==10) {
    CONST CHAR8 *C=boot_stage==6?"download:00000400":"boot";UINTN N=strlen(C);assert(mPending[2]);CopyMem(mBulkRx.Cpu,C,N);
    T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)N;T->Control&=~BIT0;publish(0xC044);++boot_stage;return;
  }
  if(boot_stage==8) {assert(mPending[2]);CopyMem(mBulkRx.Cpu,payload,sizeof(payload));T=mTrbs[2].Cpu;T->Size=mPosted[2]-sizeof(payload);T->Control&=~BIT0;publish(0xC044);++boot_stage;return;}
  assert(boot_stage==7 || boot_stage==9 || boot_stage==11);assert(mPending[3] && !take_calls);
  if(boot_stage==11) {
    assert(mFastboot.BootPending && !mBootAckObserved && !mFastboot.BootTransferFrozen && mFastboot.Download && !mPending[2]);
    if(test_case==1)return; // No IN completion: no transfer, even after timeout.
    if(test_case==6)fail_dma_free=TRUE;
    if(test_case==7)failed_halt=TRUE;
    if(test_case==8)dma_free_status=EFI_WARN_UNKNOWN_GLYPH;
    if(test_case==10)dma_complete_status=EFI_WARN_UNKNOWN_GLYPH;
  }
  T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC046);++boot_stage;
}
static VOID clean_model(VOID) {
  if(captured){ZeroMem(captured,1024);free(captured);captured=NULL;}
  if(mFastboot.Download){ZeroMem(mFastboot.Download,mFastboot.Expected);free(mFastboot.Download);mFastboot.Download=NULL;mFastboot.Upload=NULL;}
  for(UINTN I=0;I<ARRAY_SIZE(all);++I){free(all[I]->Cpu);ZeroMem(all[I],sizeof(*all[I]));}
  ClearFrames();mFastboot.BootTransferFrozen=mFastboot.BootPreparing=mFastboot.BootPending=FALSE;
  mExperimentRunning=FALSE;mBootActionValid=FALSE;mExperimentHasBoot=FALSE;mExperimentCheck=NULL;mExperimentHasStorage=FALSE;
}
int main(void) {
  fixture();gBS=&bs;bs.Stall=stall;gRT=&rt;rt.ResetSystem=reset;
  for(test_case=0;test_case<=10;++test_case) {
    clean_model();init();boot_stage=ready_calls=validate_calls=take_calls=0;stall_hook=tick_boot;lifecycle_continue=lifecycle_hold_ack=FALSE;
    assert(PianoDwc3SetBootForExperiment(&backend)==EFI_SUCCESS);
    PIANO_OWNED_SMMU Context={0};PIANO_DMA_DEVICE Device={0};EFI_STATUS S;
    if(test_case==7) {
      if(!setjmp(failed_reset_return)){PianoDwc3Ep0Experiment(&Context,&Device);assert(FALSE);}
      assert(!resets && !take_calls && mFastboot.BootTransferFrozen && mExperimentRunning);continue;
    }
    S=PianoDwc3Ep0Experiment(&Context,&Device);PIANO_FB_BOOT_ACTION A;EFI_STATUS Action=PianoDwc3ConsumeBootAction(&A);
    if(test_case==0){assert(S==EFI_SUCCESS && Action==EFI_SUCCESS && A.Taken && !A.Retained && A.Token==(VOID *)789 && captured && !mFastboot.Download && !mFastboot.BootTransferFrozen && !mFastboot.BootPending && !mExperimentRunning && take_calls==1);}
    else if(test_case==1){assert(S==EFI_SUCCESS && Action==EFI_NOT_FOUND && !take_calls && !mFastboot.Download && !mExperimentRunning);}
    else {assert(S!=EFI_SUCCESS && Action!=EFI_SUCCESS && !A.Taken && A.Retained && mFastboot.BootTransferFrozen && mExperimentRunning);
      assert(PianoDwc3SetBootForExperiment(NULL)==EFI_NOT_READY);UINT8 *Before=mFastboot.Download;PianoFastbootReset(&mFastboot);assert(mFastboot.Download==Before);
      if(test_case==6 || test_case==8 || test_case==9 || test_case==10)assert(!take_calls);
    }
  }
  clean_model();model_init();configure();ready_calls=validate_calls=take_calls=0;test_case=0;
  cmd("getvar:SunUEFI:ram-boot","OKAYdisabled");
  assert(PianoFastbootSetBoot(&mFastboot,&backend)==EFI_SUCCESS);
  cmd("getvar:SunUEFI:ram-boot","OKAYenabled");
  test_case=20;cmd("getvar:SunUEFI:ram-boot","OKAYdisabled");test_case=0;
  assert(PianoFastbootSetBoot(&mFastboot,NULL)==EFI_SUCCESS);ready_calls=0;
  cmd("boot","FAILRAM boot backend unavailable");assert(!take_calls && !ready_calls);
  assert(PianoFastbootSetBoot(&mFastboot,&backend)==EFI_SUCCESS);cmd("boot","FAILno exclusive complete RAM payload");
  cmd("download:00000400","DATA00000400");out(payload,1024);drain();assert(mFastboot.Complete);
  test_case=20;cmd("boot","FAILRAM boot backend not ready");test_case=21;cmd("boot","FAILRAM boot policy rejected");
  test_case=0;PIANO_FB_BOOT Broken=backend;Broken.Validate=NULL;assert(PianoFastbootSetBoot(&mFastboot,&Broken)==EFI_INVALID_PARAMETER);
  mFastboot.Download[0]=0;cmd("boot","FAILunsupported RAM boot image");mFastboot.Download[0]='M';
  PIANO_FB_BOOT Budget=backend;Budget.MaxImageBytes=4096;assert(PianoFastbootSetBoot(&mFastboot,&Budget)==EFI_SUCCESS);cmd("boot","FAILRAM boot exceeds image budget");
  assert(PianoFastbootSetBoot(&mFastboot,&backend)==EFI_SUCCESS);
  mFrameCount=FB_QUEUE_FRAMES;assert(PianoFastbootPacket(&mFastboot,"boot",4)==EFI_OUT_OF_RESOURCES && !mFastboot.BootPending);mFrameCount=0;
  // Verify the standard CLI wrapper view is selected, while a ramdisk
  // combination is rejected before OKAY rather than silently ignored.
  UINT8 *Raw=mFastboot.Download;UINT8 *Wrapped=AllocateZeroPool(4096);CopyMem(Wrapped,"ANDROID!",8);CopyMem(Wrapped+2048,Raw,1024);
  ((UINT32 *)Wrapped)[2]=1024;((UINT32 *)Wrapped)[9]=2048;ZeroMem(Raw,1024);FreePool(Raw);
  mFastboot.Download=mFastboot.Upload=Wrapped;mFastboot.Expected=mFastboot.Received=mFastboot.UploadBytes=4096;
  ((UINT32 *)Wrapped)[4]=1;cmd("boot","FAILunsupported RAM boot image");((UINT32 *)Wrapped)[4]=0;
  assert(PianoFastbootPacket(&mFastboot,"boot",4)==EFI_SUCCESS && mFastboot.BootPending && mFastboot.BootView.Wrapped && mFastboot.BootView.Offset==2048 && !take_calls);
  ClearFrames();PianoFastbootReset(&mFastboot);
  // Raw Linux stays opt-in. A strict v2 three-component container reaches
  // the owner validator only when that particular backend enables it.
  UINT8 *LinuxImage=AllocateZeroPool(8192);CopyMem(LinuxImage,"ANDROID!",8);
  ((UINT32 *)LinuxImage)[2]=1024;((UINT32 *)LinuxImage)[4]=1;((UINT32 *)LinuxImage)[9]=2048;
  ((UINT32 *)LinuxImage)[10]=2;((UINT32 *)LinuxImage)[411]=1660;((UINT32 *)LinuxImage)[412]=40;
  CopyMem(LinuxImage+2048,payload,sizeof(payload));
  mFastboot.Download=mFastboot.Upload=LinuxImage;mFastboot.Expected=mFastboot.Received=mFastboot.UploadBytes=8192;mFastboot.Complete=mFastboot.UploadBorrowed=TRUE;
  UINTN Validations=validate_calls;cmd("boot","FAILunsupported RAM boot image");assert(validate_calls==Validations);
  PIANO_FB_BOOT RawBackend=backend;RawBackend.AllowRawLinux=TRUE;assert(PianoFastbootSetBoot(&mFastboot,&RawBackend)==EFI_SUCCESS);
  test_case=21;cmd("boot","FAILRAM boot policy rejected");assert(validate_calls==Validations+1);test_case=0;
  ((UINT32 *)LinuxImage)[6]=1;cmd("boot","FAILunsupported RAM boot image");assert(validate_calls==Validations+1);
  ClearFrames();PianoFastbootReset(&mFastboot);
  mFastboot.Download=mFastboot.Upload=AllocateZeroPool(1024);CopyMem(mFastboot.Download,payload,1024);mFastboot.Expected=mFastboot.Received=mFastboot.UploadBytes=1024;mFastboot.Complete=mFastboot.UploadBorrowed=TRUE;
  PIANO_FB_BOOT Local=backend;assert(PianoFastbootSetBoot(&mFastboot,&Local)==EFI_SUCCESS);Local.TakeAfterAck=NULL;assert(mFastboot.Boot.TakeAfterAck==backend.TakeAfterAck);
  out("boot",4);assert(mFastboot.BootPending && !mBootAckObserved && !take_calls);assert(PianoFastbootSetBoot(&mFastboot,NULL)==EFI_NOT_READY);
  assert(Event(0x101)==EFI_SUCCESS && !mFastboot.BootPending && !mFastboot.Download && !take_calls);end(3); // Reset before ACK cancels and safely zeros source.
  // Retiring an old transfer during configuration/reset is equally strict:
  // a DMA warning must not become reusable state for a later boot request.
  clean_model();model_init();configure();dma_complete_status=EFI_WARN_UNKNOWN_GLYPH;mEnding[2]=TRUE;
  assert(BulkEnded(2,8U<<24)==EFI_DEVICE_ERROR && mFastboot.BootTransferFrozen && mTrbs[2].Quarantined && mEnding[2]);
  clean_model();model_init();configure();dma_complete_status=EFI_WARN_UNKNOWN_GLYPH;
  mPending[0]=TRUE;mTrbs[0].Active=TRUE;
  assert(StopTransfer(0)==EFI_DEVICE_ERROR && mFastboot.BootTransferFrozen && mTrbs[0].Quarantined && mPending[0]);
  clean_model();puts("Actual boot command/DWC: default/backend gates, structural+policy pre-ACK validation, queued send is not ACK, true IN/drain then Halt+9DMAfree, frozen dispatch/reset, copied ownership action, cancellation and failed/partial/warning retention passed.");return 0;
}
