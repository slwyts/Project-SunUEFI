// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Device + Controller + protocol; fake hardware/backend, never devices.
#define PIANO_USB_UFS_FETCH 1
#ifndef PIANO_USB_SERVICE
#define PIANO_USB_SERVICE 1
#endif
#define PIANO_USB_EP0 1
#define PIANO_USB_RAM_BOOT 1
#define USB_FASTBOOT_TEST_MAIN bulk_source_suite
#include "../tools/test_usb_fastboot.c"
#include "../bootprofiles/uefi-app/PianoFastbootBoot.c"
#include "../bootprofiles/uefi-app/PianoUsbController.c"
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
// The HAL fixture cannot validate a native close ledger. Model only the
// trusted validator boundary here; its actual implementation has separate
// owned-peer tests. This checks fresh capture, exact evidence and no replay.
static BOOLEAN proof_validator_accepts;static UINTN proof_validator_calls;
EFI_STATUS PianoOwnedSmmuMakeRetiredUsbProof(CONST PIANO_OWNED_SMMU *C,CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *E,
  CONST PIANO_SMMU_SNAPSHOT *S,PIANO_SMMU_RETIRED_USB_PROOF *P){
  ++proof_validator_calls;assert(C==&mUsbContext && !C->Attached && !C->Domain && !C->TableMemory.Signature);
  assert(E->Revision==1 && E->DeviceCleanupStatus==EFI_SUCCESS && E->ControllerCleanupStatus==EFI_SUCCESS &&
    E->DeviceHalted && E->DmaFreed && E->ClocksReleased && E->GdscReleased && E->DmaBuffersFreed==9 && E->ClockReleaseMask==255);
  assert(S->Valid && S->Device[0].Present && !S->Device[1].Present);
  if(!proof_validator_accepts)return EFI_NOT_READY;
  *P=(PIANO_SMMU_RETIRED_USB_PROOF){.Revision=1,.Valid=TRUE,.UsbContext=C,.Execution=*E,.Retired=*S};return EFI_SUCCESS;
}
static EFI_CLOCK_PROTOCOL clock;
static UINT32 smmu[0x100000/4];static PIANO_SMMU_SNAPSHOT hardware;
static UINTN held,next_clock,closes,installs,removes,full_captures,backend_reads,backend_infos,stage;
static BOOLEAN domain,install_fail,remove_fail,backend_fail,reentry;
static UINTN scenario;
static PIANO_USB_SHUTDOWN *visible_shutdown;
static EFI_STATUS reentry_status,setter_status;
static VOID hw_store(UINTN Offset,UINT32 V){assert(!(Offset&3) && Offset<sizeof(smmu));smmu[Offset/4]=V;}
static VOID hw64(UINTN Offset,UINT64 V){hw_store(Offset,(UINT32)V);hw_store(Offset+4,(UINT32)(V>>32));}
static VOID load_hardware(VOID) {
  memset(smmu,0,sizeof(smmu));hw_store(0,hardware.GlobalControl);hw_store(0x20,hardware.Id0);hw_store(0x24,hardware.Id1);hw_store(0x28,hardware.Id2);hw_store(0x48,hardware.GlobalFault);
  for(UINTN I=0;I<hardware.Groups;++I){hw_store(0x800+4*I,hardware.RawSmr[I]);hw_store(0xC00+4*I,hardware.RawS2cr[I]);}
  for(UINTN I=0;I<2;++I)if(hardware.Device[I].Present) {
    PIANO_SMMU_DEVICE *D=&hardware.Device[I];UINTN G=1U<<hardware.PageShift,B=hardware.ContextBase+((UINTN)D->ContextBank<<hardware.PageShift);
    hw_store(G+4*D->ContextBank,D->Cbar);hw_store(G+0x800+4*D->ContextBank,D->Cba2r);
    hw_store(B,D->Sctlr);hw_store(B+0x10,D->Tcr2);hw64(B+0x20,D->Ttbr0);hw64(B+0x28,D->Ttbr1);hw_store(B+0x30,D->Tcr);hw_store(B+0x38,D->Mair0);hw_store(B+0x3c,D->Mair1);
    hw_store(B+0x58,D->Fsr);hw64(B+0x60,D->Far);hw_store(B+0x68,D->Fsynr);
  }
}
static UINT32 extra_read(UINTN Address) {
  if(Address>=hardware.Base && Address<hardware.Base+sizeof(smmu)){assert(!(Address&3));return smmu[(Address-hardware.Base)/4];}
  assert(Address==0x88E303C || Address==0x88E3054 || Address==0x88E3064 || Address==0x88E8008);return 0;
}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){assert(Fdt==(VOID *)123);return 1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Bytes) {
  STATIC CONST UINT8 Reg[]={0,0,0,0,0x0a,0x60,0,0,0,0,0,0,0,0,0xd9,0x3c},Iommu[]={0,0,0,1,0,0,0,0x40,0,0,0,0};
  assert(Fdt==(VOID *)123 && Node==1);if(!strcmp(Name,"reg")){*Bytes=16;return Reg;}assert(!strcmp(Name,"iommus"));*Bytes=12;return Iommu;
}
VOID PianoProbeUsbPower(CONST VOID *Fdt){assert(Fdt==(VOID *)123);}
static EFI_STATUS EFIAPI locate_clock(EFI_GUID *Guid,VOID *Registration,VOID **Interface){*Interface=&clock;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_id(EFI_CLOCK_PROTOCOL *C,CONST CHAR8 *Name,UINTN *Id){assert(C==&clock);*Id=!strcmp(Name,"gcc_usb30_prim_gdsc")?100:next_clock++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_enable(EFI_CLOCK_PROTOCOL *C,UINTN Id){assert(C==&clock);if(Id==100)domain=TRUE;else {assert(Id<8 && domain);held|=1U<<Id;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_disable(EFI_CLOCK_PROTOCOL *C,UINTN Id){assert(C==&clock && !mUsbShutdownLive);if(Id==100){assert(!held);domain=FALSE;}else {assert(held&(1U<<Id));held&=~(1U<<Id);}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI install(EFI_HANDLE *Handle,...) {
  ++installs;if(install_fail)return EFI_DEVICE_ERROR;
  va_list A;va_start(A,Handle);EFI_GUID *G=va_arg(A,EFI_GUID *);visible_shutdown=va_arg(A,PIANO_USB_SHUTDOWN *);assert(va_arg(A,VOID *)==NULL);va_end(A);
  EFI_GUID Expected=PIANO_USB_SHUTDOWN_GUID;assert(!memcmp(G,&Expected,sizeof(Expected)) && visible_shutdown==&mUsbShutdown && held==255);*Handle=(VOID *)987;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE Handle,...) {
  assert(Handle==(VOID *)987 && (MmioRead32(DW+0xC70C)&BIT22));++removes;if(remove_fail)return EFI_DEVICE_ERROR;visible_shutdown=NULL;return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Snapshot) {
  assert(Fdt==(VOID *)123);++full_captures;
  if(!strcmp(Phase,"coexist-dwc-mapped")) {
    assert(allocations==10 && !(regs[0xC704/4]&BIT31));for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(all[I]->Mapped);
    if(scenario==7){hardware.Device[1].Tcr^=1;load_hardware();}
  }
  if(!strcmp(Phase,"coexist-before-run")){assert(mRing.Active && !(regs[0xC704/4]&BIT31));if(scenario==8){hardware.Device[0].Sctlr^=1;load_hardware();}}
  *Snapshot=hardware;return EFI_SUCCESS;
}
static PIANO_SMMU_DEVICE bank(UINTN I,UINT16 Sid,UINT64 Root) {
  return (PIANO_SMMU_DEVICE){.Sid=Sid,.StreamIndex=(UINT16)I,.ContextBank=(UINT8)I,.Present=TRUE,.Enabled=TRUE,.Smr=0x80000000U|Sid,.S2cr=(UINT32)I,
    .Sctlr=0x1e5,.Cbar=0x10000,.Cba2r=1,.Tcr=0x00802519,.Tcr2=0x00038001,.Mair0=0xff,.Ttbr0=Root};
}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D) {
  assert(mUsbShutdownLive && held==255 && domain);C->Before=hardware;C->Fdt=Fdt;
  hardware.Device[1]=bank(1,0x40,0x82000000);hardware.RawSmr[1]=hardware.Device[1].Smr;hardware.RawS2cr[1]=1;load_hardware();
  C->After=hardware;C->Verified=C->Attached=TRUE;C->Domain=(VOID *)0x888;C->TableMemory.Signature=1;C->TableMemory.Physical=0x82000000;D->Context=C;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C) {
  ++closes;if(C->TableMemory.Quarantined)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(!all[I]->Signature);
  C->Attached=C->Verified=FALSE;C->Domain=NULL;ZeroMem(&C->TableMemory,sizeof(C->TableMemory));ZeroMem(&hardware.Device[1],sizeof(hardware.Device[1]));hardware.Device[1].Sid=0x40;
  hardware.RawSmr[1]=hardware.RawS2cr[1]=0;load_hardware();return EFI_SUCCESS;
}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,BOOLEAN Write,UINT64 *Pa){*Pa=0x81000000+Iova-0x40000000;return EFI_SUCCESS;}
static EFI_STATUS ready_backend(VOID *Context) {
  assert(Context==(VOID *)456);
  if(reentry){reentry=FALSE;BOOLEAN R;PIANO_FB_STORAGE Fake=mUnderlyingStorage;reentry_status=PianoUsbControllerRunWithStorage((VOID *)123,&Fake,&R);assert(PianoUsbControllerExperiment((VOID *)123)==EFI_NOT_READY);}
  return backend_fail?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS info_backend(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info) {
  assert(Context==(VOID *)456 && !strcmp(Name,"xbl_config_a"));++backend_infos;ZeroMem(Info,sizeof(*Info));CopyMem(Info->Name,Name,strlen(Name)+1);Info->Bytes=4096;Info->BlockSize=4096;Info->ReadOnly=TRUE;Info->Token=(VOID *)789;
  if(scenario==2)hw_store(hardware.ContextBase+4096+0x38,0xfe);
  return EFI_SUCCESS;
}
static EFI_STATUS read_backend(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer) {
  assert(Context==(VOID *)456 && Info->Token==(VOID *)789 && !Lba && Bytes==4096);++backend_reads;memset(Buffer,0xa5,Bytes);
  setter_status=PianoDwc3SetStorageForExperiment(NULL);assert(setter_status==EFI_NOT_READY);
  assert(PianoDwc3Ep0Experiment(&mUsbContext,&mUsbDevice)==EFI_ALREADY_STARTED);
  if(scenario==1)hw_store(hardware.ContextBase+0x20,0x81001000);
  if(scenario==3)hw_store(0x80c,0x80000040);
  return EFI_SUCCESS;
}
static PIANO_FB_STORAGE backend={ (VOID *)456,ready_backend,info_backend,read_backend };
static VOID tick(UINTN Us) {
  if(Us!=1000 || !mRing.Active || !(regs[0xC704/4]&BIT31) || (stage==0 && !mPending[0]))return;
  if(stage<6){lifecycle_stage=stage;lifecycle_events(Us);stage=lifecycle_stage;return;}
  DWC_TRB *T;
  if(stage==6) {
    CONST CHAR8 *C="fetch:xbl_config_a:0x0:0x1";UINTN N=strlen(C);CopyMem(mBulkRx.Cpu,C,N);T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)N;T->Control&=~BIT0;publish(0xC044);stage=7;return;
  }
  if(stage==7 && mPending[3]) {T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC046);return;}
  if(stage==7){assert(mPending[2] && !mFrames);CopyMem(mBulkRx.Cpu,"reboot",6);T=mTrbs[2].Cpu;T->Size=mPosted[2]-6;T->Control&=~BIT0;publish(0xC044);stage=8;return;}
  assert(stage==8 && mPending[3]);T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC046);stage=9;
}
static VOID setup_model(VOID) {
  // Simulated cold reset also releases intentionally retained mock memory.
  for(UINTN I=0;I<ARRAY_SIZE(all);++I){free(all[I]->Cpu);ZeroMem(all[I],sizeof(*all[I]));}
  init();held=next_clock=closes=installs=removes=full_captures=backend_reads=backend_infos=stage=0;
  regs[0xC120/4]=0x33313130;
  domain=install_fail=remove_fail=backend_fail=reentry=FALSE;visible_shutdown=NULL;
  mUsbCleanupBlocked=mUsbControllerRunning=mCombinedBusy=mCoexistReady=mCoexistFailed=mProxyBusy=mUsbShutdownLive=FALSE;mUsbShutdownHandle=NULL;
  mExperimentRunning=mExperimentHasStorage=mExperimentContractFailed=FALSE;mExperimentCheck=NULL;mExperimentCheckContext=NULL;
  ZeroMem(&mUsbContext,sizeof(mUsbContext));ZeroMem(&hardware,sizeof(hardware));
  hardware.Valid=TRUE;hardware.Base=0x15000000;hardware.Window=sizeof(smmu);hardware.ContextBase=0x8000;hardware.PageShift=12;hardware.Groups=4;hardware.Banks=2;hardware.Id0=4;hardware.Id1=0x20000002;
  hardware.Device[0]=bank(0,0x60,0x81000000);hardware.Device[1].Sid=0x40;hardware.RawSmr[0]=hardware.Device[0].Smr;load_hardware();
  extra_mmio_read=extra_read;gBS=&bs;gRT=&rt;bs.Stall=stall;rt.ResetSystem=reset;bs.LocateProtocol=locate_clock;bs.InstallMultipleProtocolInterfaces=install;bs.UninstallMultipleProtocolInterfaces=uninstall;
  ZeroMem(&clock,sizeof(clock));clock.Version=0x1000b;clock.GetClockID=clock.GetClockPowerDomainID=clock_id;clock.EnableClock=clock.EnableClockPowerDomain=clock_enable;clock.DisableClock=clock.DisableClockPowerDomain=clock_disable;
  lifecycle_continue=lifecycle_hold_ack=FALSE;stall_hook=tick;
}
#if PIANO_USB_SERVICE
static EFI_TPL current_tpl=TPL_APPLICATION;
static UINTN bs_calls,event_creates,event_closes,clock_calls,proof_calls;
static UINT64 fake_now;
static BOOLEAN in_timer,event_failure,event_warning,timer_cancel_warning,event_close_warning,clock_warning,domain_warning;
static VOID (EFIAPI *timer_notify)(EFI_EVENT,VOID *), (EFIAPI *exit_notify)(EFI_EVENT,VOID *);
EFI_GUID gEfiEventExitBootServicesGuid={0x27ABF055,0xB1B8,0x4C26,{0x80,0x48,0x74,0x8F,0x37,0xBA,0xA2,0xDF}};
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){assert(!in_timer);++bs_calls;EFI_TPL Old=current_tpl;assert(New>=Old);current_tpl=New;return Old;}
static VOID EFIAPI restore_tpl(EFI_TPL Old){assert(!in_timer);++bs_calls;assert(Old<=current_tpl);current_tpl=Old;}
static EFI_STATUS EFIAPI create_event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Event) {
  assert(current_tpl==TPL_APPLICATION && !in_timer && Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && Tpl==TPL_CALLBACK && Context==NULL);
  ++bs_calls;++event_creates;timer_notify=Notify;*Event=(VOID *)0x101;
  if(event_warning)return EFI_WARN_STALE_DATA;if(event_failure)return EFI_DEVICE_ERROR;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI create_event_ex(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Event) {
  assert(current_tpl==TPL_APPLICATION && !in_timer && Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && !Context && Guid==&gEfiEventExitBootServicesGuid);
  ++bs_calls;++event_creates;exit_notify=Notify;*Event=(VOID *)0x102;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_timer(EFI_EVENT Event,EFI_TIMER_DELAY Delay,UINT64 Time) {
  assert(current_tpl==TPL_APPLICATION && !in_timer && Event==(VOID *)0x101);++bs_calls;
  if(Delay==TimerCancel){assert(!Time);return timer_cancel_warning?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
  assert(Delay==TimerPeriodic && Time==10000);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event) {
  assert(current_tpl==TPL_APPLICATION && !in_timer && (Event==(VOID *)0x101 || Event==(VOID *)0x102));++bs_calls;++event_closes;
  return event_close_warning?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static UINT64 now_us(VOID *Context){assert(Context==(VOID *)0x55 && !in_timer);return fake_now+=50;}
static EFI_STATUS EFIAPI persistent_disable(EFI_CLOCK_PROTOCOL *C,UINTN Id) {
  ++clock_calls;if(Id==3 && clock_warning)return EFI_WARN_STALE_DATA;if(Id==100 && domain_warning)return EFI_WARN_STALE_DATA;
  return clock_disable(C,Id);
}
static VOID timer_tick(VOID) {
  UINTN Calls=bs_calls,Alloc=allocations,Cmd=address_writes,Reads=backend_reads;EFI_TPL Old=current_tpl;
  current_tpl=TPL_CALLBACK;in_timer=TRUE;timer_notify((VOID *)0x101,NULL);in_timer=FALSE;current_tpl=Old;
  assert(bs_calls==Calls && allocations==Alloc && address_writes==Cmd && backend_reads==Reads);
}
static VOID service_event(UINT32 Value) {
  publish(Value);timer_tick();assert(mService.Count && mService.State.WorkPending);
  EFI_STATUS S=PianoUsbControllerServicePumpApp(1,5000);assert(S==EFI_SUCCESS || (scenario==1 && S==EFI_DEVICE_ERROR));
}
static VOID service_enumerate(VOID) {
  lifecycle_stage=0;lifecycle_hold_ack=FALSE;
  for(UINTN I=0;I<6;++I){lifecycle_events(1000);timer_tick();assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS);}
  assert(mConfigured && mPending[2] && mBulkLive);
}
static VOID service_out(CONST CHAR8 *Text) {
  assert(mPending[2] && !mPending[3]);UINTN N=strlen(Text);CopyMem(mBulkRx.Cpu,Text,N);
  DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)N;T->Control&=~BIT0;service_event(0xC044);
}
static VOID service_ack(VOID) {
  while(mPending[3]){DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;service_event(0xC046);}
}
static UINT8 boot_payload[1024];static UINT8 *taken_payload;static UINTN boot_validates,boot_takes;static BOOLEAN boot_take_fail;
static VOID boot_fixture(VOID) {
  memset(boot_payload,0,sizeof(boot_payload));boot_payload[0]='M';boot_payload[1]='Z';
#define P16(O,V) do{boot_payload[O]=(UINT8)(V);boot_payload[(O)+1]=(UINT8)((V)>>8);}while(0)
#define P32(O,V) do{for(UINTN J=0;J<4;++J)boot_payload[(O)+J]=(UINT8)((UINT32)(V)>>(8*J));}while(0)
  P32(60,128);P32(128,0x4550);P16(132,0xaa64);P16(134,1);P16(148,240);P16(150,2);P16(152,0x20b);
  P32(168,4096);P32(184,4096);P32(188,512);P32(208,8192);P32(212,512);P16(220,10);P32(260,16);
  P32(400,512);P32(404,4096);P32(408,512);P32(412,512);P32(428,0x60000020);
#undef P16
#undef P32
}
static EFI_STATUS persistent_boot_ready(VOID *Context){assert(Context==(VOID *)0x99 && !in_timer && current_tpl==TPL_APPLICATION);return EFI_SUCCESS;}
static EFI_STATUS persistent_boot_validate(VOID *Context,CONST PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View) {
  assert(Context==(VOID *)0x99 && current_tpl==TPL_APPLICATION && !in_timer && Source==&mFastboot && View->Bytes==1024 && View->ImageBytes==8192);
  assert(!memcmp(Source->Download,boot_payload,1024));++boot_validates;return EFI_SUCCESS;
}
static EFI_STATUS persistent_boot_take(VOID *Context,PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View,VOID **Token) {
  (VOID)View;assert(Context==(VOID *)0x99 && current_tpl==TPL_APPLICATION && !in_timer && Source==&mFastboot);
  assert(boot_validates==2 && Source->BootProof.AckCompleted && Source->BootProof.DmaFreed && Source->BootProof.DmaBuffersFreed==9);
  assert(!mFrames && !mPending[3] && (Dr(0xC70C)&BIT22));for(UINTN J=0;J<ARRAY_SIZE(all);++J)assert(!all[J]->Signature);++boot_takes;
  if(boot_take_fail)return EFI_DEVICE_ERROR;
  taken_payload=Source->Download;Source->Download=Source->Upload=NULL;Source->Expected=Source->Received=Source->UploadBytes=0;Source->UploadBorrowed=Source->Receiving=Source->Complete=FALSE;
  *Token=(VOID *)0x8877;return EFI_SUCCESS;
}
static VOID service_download_fixture(VOID) {
  service_out("download:00000400");service_ack();assert(mFastboot.Receiving && mPending[2]);
  CopyMem(mBulkRx.Cpu,boot_payload,1024);DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-1024;T->Control&=~BIT0;service_event(0xC044);service_ack();
  assert(mFastboot.Complete && mFastboot.Download && !boot_takes);
}
static VOID free_cold_model(VOID) {
  if(taken_payload){free(taken_payload);taken_payload=NULL;}
  if(mFastboot.Download)free(mFastboot.Download);
  if(mFastboot.Upload && !mFastboot.UploadBorrowed && mFastboot.Upload!=mFastboot.Download)free(mFastboot.Upload);
  if(mLogSnapshot)free(mLogSnapshot);
  while(mFrames){FB_FRAME *F=mFrames;mFrames=F->Next;free(F);}
  for(UINTN I=0;I<ARRAY_SIZE(all);++I){free(all[I]->Cpu);ZeroMem(all[I],sizeof(*all[I]));}
}
static PIANO_PRODUCT_RUNTIME_PROTOCOL navigation_runtime,navigation_replacement;
static UINTN navigation_case,navigation_queries,navigation_requests,navigation_action;
static BOOLEAN navigation_alive=TRUE;
static EFI_STATUS EFIAPI navigation_pump(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Reason,UINTN Budget){(VOID)This;(VOID)Reason;(VOID)Budget;assert(!"navigation may not recursively pump");return EFI_UNSUPPORTED;}
static BOOLEAN EFIAPI navigation_is_alive(PIANO_PRODUCT_RUNTIME_PROTOCOL *This){assert(This==&navigation_runtime && current_tpl==TPL_APPLICATION && !in_timer);return navigation_alive;}
static EFI_STATUS EFIAPI navigation_pending(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 *Action,UINT64 *Seq){(VOID)This;(VOID)Action;(VOID)Seq;assert(!"USB navigation must only latch");return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI navigation_ack(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT64 Seq){(VOID)This;(VOID)Seq;assert(!"USB does not consume UI actions");return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI navigation_request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action){
  assert(This==&navigation_runtime && current_tpl==TPL_APPLICATION && !in_timer && !closes && held==255 && domain && mService.State.Phase==PianoUsbServiceListening);
  ++navigation_requests;navigation_action=Action;
  if(navigation_case==4)return EFI_WARN_STALE_DATA;
  if(navigation_case==5)return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI navigation_changed_request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action){(VOID)This;(VOID)Action;assert(!"changed runtime method must never run");return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI navigation_locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){
  EFI_GUID UiGuid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
  if(memcmp(Guid,&UiGuid,sizeof(UiGuid)))return locate_clock(Guid,Registration,Interface);
  assert(current_tpl==TPL_APPLICATION && !in_timer);++navigation_queries;
  if(navigation_case==1){*Interface=NULL;return EFI_NOT_FOUND;}
  if(navigation_case==9)return EFI_WARN_STALE_DATA;
  if(navigation_case==10){exit_notify((VOID *)0x102,NULL);*Interface=&navigation_runtime;return EFI_SUCCESS;}
  *Interface=navigation_case==6 && navigation_queries>1?&navigation_replacement:&navigation_runtime;return EFI_SUCCESS;
}
static VOID run_navigation(UINTN Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;bs.LocateProtocol=navigation_locate;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  navigation_case=Test;navigation_queries=navigation_requests=navigation_action=0;navigation_alive=Test!=3;
  navigation_runtime=(PIANO_PRODUCT_RUNTIME_PROTOCOL){.Revision=PIANO_PRODUCT_RUNTIME_REVISION,.Pump=navigation_pump,.BootServicesAlive=navigation_is_alive,.RequestAction=navigation_request,.GetPendingAction=navigation_pending,.AckAction=navigation_ack};
  navigation_replacement=navigation_runtime;
  if(Test==2)navigation_runtime.Revision=99;
  if(Test==8)navigation_runtime.GetPendingAction=NULL;
  PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&backend};
  assert(PianoUsbControllerServiceStart((VOID *)123,&C)==EFI_SUCCESS);service_enumerate();
  if(Test==0){
    CONST CHAR8 *Commands[]={"oem setup","oem shell","oem simpleinit"};CONST UINT32 Actions[]={PIANO_PRODUCT_ACTION_SETUP,PIANO_PRODUCT_ACTION_SHELL,PIANO_PRODUCT_ACTION_SIMPLEINIT};
    for(UINTN I=0;I<3;++I){UINTN Alloc=allocations;service_out(Commands[I]);assert(mPending[3] && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4) && navigation_action==Actions[I]);service_ack();assert(!closes && !clock_calls && allocations==Alloc && mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);}
    assert(navigation_queries==3 && navigation_requests==3);service_out("getvar:version");service_ack();
  } else {
    if(Test==6 || Test==7){service_out("oem setup");service_ack();assert(navigation_requests==1);if(Test==7)navigation_runtime.RequestAction=navigation_changed_request;}
    if(Test==10){
      CopyMem(mBulkRx.Cpu,"oem setup",9);DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-9;T->Control&=~BIT0;
      publish(0xC044);timer_tick();assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_ABORTED);
      assert(mService.State.ServicesLost && !navigation_requests && !mFrames && !mPending[3] && !closes && held==255);
      free_cold_model();return;
    }
    service_out("oem shell");CONST CHAR8 *Failure="FAILUI navigation backend unavailable";
    assert(mPending[3] && mPosted[3]==strlen(Failure) && !memcmp(mBulkTx.Cpu,Failure,strlen(Failure)));service_ack();
    assert(!closes && !clock_calls && mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);
    if(Test!=4 && Test!=5 && Test!=6 && Test!=7)assert(!navigation_requests);
  }
  PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS && R.Clean);free_cold_model();
}
static VOID run_service(UINTN Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;bs_calls=event_creates=event_closes=clock_calls=proof_calls=0;
  timer_notify=exit_notify=NULL;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_FB_STORAGE Copied=backend;PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Copied};
  if(Test>=17){boot_fixture();PIANO_FB_BOOT Boot={.Context=(VOID *)0x99,.MaxImageBytes=65536,.Ready=persistent_boot_ready,.Validate=persistent_boot_validate,.TakeAfterAck=persistent_boot_take};assert(PianoDwc3SetBootForExperiment(&Boot)==EFI_SUCCESS);}
  if(Test==9)event_failure=TRUE;if(Test==10)event_warning=TRUE;
  EFI_STATUS S=PianoUsbControllerServiceStart((VOID *)123,&Config);
  if(Test==9){assert(S==EFI_DEVICE_ERROR && !held && !domain && !mPersistent.Started && closes==1 && !mUsbShutdownLive);free_cold_model();return;}
  if(Test==10){assert(S==EFI_DEVICE_ERROR && mUsbCleanupBlocked && held==255 && domain && !closes);free_cold_model();return;}
  assert(S==EFI_SUCCESS && allocations==10 && held==255 && domain && mUsbShutdownLive && closes==0 && mService.State.Started && event_creates==2);
  Copied.Ready=NULL;assert(mUnderlyingStorage.Ready==backend.Ready);assert(mPersistent.Config.Storage==NULL);
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_INVALID_PARAMETER);Copied=backend;
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_ALREADY_STARTED);
  assert(PianoDwc3SetStorageForExperiment(NULL)==EFI_NOT_READY);
  if(Test==1){current_tpl=TPL_CALLBACK;assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_UNSUPPORTED);current_tpl=TPL_APPLICATION;}
  if(Test==2){mService.State.Busy=TRUE;assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_ALREADY_STARTED);mService.State.Busy=FALSE;}
  if(Test==3){regs[0xC40C/4]=3;timer_tick();assert(mService.State.Action==PianoUsbServiceActionFault && mService.Uncertain);}
  else if(Test==4){
    for(UINTN I=0;I<SERVICE_EVENTS;++I){publish(0x10D);timer_tick();}
    assert(mService.Count==SERVICE_EVENTS);publish(0x10D);timer_tick();assert(mService.Uncertain && mService.State.LastStatus==EFI_OUT_OF_RESOURCES);
  } else if(Test==8){
    UINTN Calls=bs_calls,Alloc=allocations;current_tpl=TPL_NOTIFY;in_timer=TRUE;exit_notify((VOID *)0x102,NULL);in_timer=FALSE;current_tpl=TPL_APPLICATION;
    assert(bs_calls==Calls && allocations==Alloc && mPersistent.ServicesLost && mService.State.Retained && mService.State.DeviceHalted);
    PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_ACCESS_DENIED && !closes && held==255);free_cold_model();return;
  } else {
    service_enumerate();
    if(Test==0){
      for(UINTN Page=0;Page<3;++Page){fake_now+=9000000000ULL;assert(PianoUsbControllerServicePumpApp(1U<<Page,5000)==EFI_SUCCESS);service_out("getvar:version");assert(mPending[3] && !memcmp(mBulkTx.Cpu,"OKAY0.4",7));service_ack();assert(!closes && held==255 && mService.State.Phase==PianoUsbServiceListening);}
      service_out("fetch:xbl_config_a:0x0:0x1");service_ack();assert(backend_reads==1 && mCoexistQuietChecks>=6);
      service_out("reboot");assert(mService.State.Phase==PianoUsbServiceListening);service_ack();assert(mService.State.Action==PianoUsbServiceActionReboot && mService.State.Phase==PianoUsbServiceStopRequested && !resets && !closes);
    }
    if(Test>=17){service_download_fixture();service_out("boot");assert(mFastboot.BootPending && !mBootAckObserved && !boot_takes && mService.State.Phase==PianoUsbServiceListening);
      if(Test!=18){service_ack();assert(mBootAckObserved && mService.State.Action==PianoUsbServiceActionBoot && mService.State.Phase==PianoUsbServiceStopRequested && !boot_takes && mFastboot.Download);}
      if(Test==19)boot_take_fail=TRUE;
    }
    if(Test==11){scenario=1;service_out("fetch:xbl_config_a:0x0:0x1");assert(mCoexistFailed && mUsbCleanupBlocked);}
    if(Test==12){((DWC_TRB *)mTrbs[0].Cpu)->Control&=~BIT0;service_event(1);assert(!mConfigured && mEnding[2]);((DWC_TRB *)mTrbs[2].Cpu)->Control&=~BIT0;service_event((8U<<24)|(7U<<6)|4);service_event(0x201);service_enumerate();service_out("getvar:product");service_ack();assert(mService.State.Phase==PianoUsbServiceListening);}
  }
  if(Test==5)failed_halt=TRUE;if(Test==6)dma_free_status=EFI_WARN_STALE_DATA;if(Test==7)dma_complete_status=EFI_WARN_STALE_DATA;
  if(Test==13)timer_cancel_warning=TRUE;if(Test==14)event_close_warning=TRUE;if(Test==15)clock_warning=TRUE;if(Test==16)domain_warning=TRUE;
  PIANO_USB_SERVICE_RETIRE_REPORT R;S=PianoUsbControllerServiceStop(EFI_SUCCESS,&R);
  if(Test==3 || Test==4 || Test==5 || Test==6 || Test==7 || Test==11 || Test==13 || Test==14 || Test==15 || Test==16 || Test==19) {
    assert(S!=EFI_SUCCESS && R.Retained && !R.Clean && mUsbCleanupBlocked && domain);
    if(Test<=7 || Test==11 || Test==13 || Test==14 || Test==19)assert(!closes && held==255);
    if(Test==15)assert(R.ClockReleaseMask==0xf7 && held==8 && !R.ClocksReleased);
    if(Test==16)assert(R.ClockReleaseMask==0xff && !held && !R.ClocksReleased);
  } else {
    assert(S==EFI_SUCCESS && R.Clean && !R.Retained && R.DeviceHalted && R.DmaFreed && R.DmaBuffersFreed==9 && R.DomainFreed && R.ClocksReleased && R.ClockReleaseMask==255);
    assert(closes==1 && !held && !domain && !mUsbShutdownLive && !mExperimentHasStorage && !mExperimentCheck && !mCombinedBusy && !mPersistent.Started);
    PIANO_SMMU_USB_RETIRE_EVIDENCE E;assert(PianoDwc3GetRetireEvidence(&E)==EFI_SUCCESS && E.DmaBuffersFreed==9);
    assert(mControllerRetireValid && !mRetireProofConsumed);PIANO_SMMU_RETIRED_USB_PROOF P;
    assert(PianoUsbControllerMakeRetiredUsbProof((VOID *)123,&P)==EFI_NOT_READY); // mock ledger cannot forge a proof
    assert(!mRetireProofConsumed && proof_validator_calls==1);
    UINTN Captures=full_captures;proof_validator_accepts=TRUE;
    assert(PianoUsbControllerMakeRetiredUsbProof((VOID *)124,&P)==EFI_NOT_READY && full_captures==Captures);
    assert(PianoUsbControllerMakeRetiredUsbProof((VOID *)123,&P)==EFI_SUCCESS && P.Valid && mRetireProofConsumed && full_captures==Captures+1);
    assert(PianoUsbControllerMakeRetiredUsbProof((VOID *)123,&P)==EFI_NOT_READY && !P.Valid && full_captures==Captures+1 && proof_validator_calls==2);
  }
  if(Test>=17){PIANO_FB_BOOT_ACTION A;EFI_STATUS Taken=PianoDwc3ConsumeBootAction(&A);
    if(Test==18){assert(Taken==EFI_NOT_FOUND && !boot_takes && !mFastboot.Download);}
    else if(Test==17){assert(Taken==EFI_SUCCESS && A.Taken && !A.Retained && A.Token==(VOID *)0x8877 && taken_payload && boot_takes==1);assert(PianoDwc3ConsumeBootAction(&A)==EFI_NOT_FOUND);}
    else assert(Taken==EFI_DEVICE_ERROR && A.Retained && !A.Taken && boot_takes==1 && mFastboot.Download);
  }
  free_cold_model();
}
#endif
int main(void) {
#if PIANO_USB_SERVICE
  for(UINTN I=0;I<20;++I){pid_t P=fork();assert(P>=0);if(P==0){run_service(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB persistent service case %llu failed\n",(unsigned long long)I);return 1;}}
  for(UINTN I=0;I<11;++I){pid_t P=fork();assert(P>=0);if(P==0){run_navigation(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB product navigation case %llu failed\n",(unsigned long long)I);return 1;}}
  puts("Actual Device+Controller persistent USB: 20 lifecycle + 11 navigation cases PASS; timer raw queue only, APP enumeration/commands/fetch, multi-UI lifetime, ACK-before-action, disconnect/reconnect, EBS halt-retain, exact shutdown/warning quarantine; no device.");
#else
  (VOID)backend;PIANO_DWC3_SERVICE_STATUS S;PIANO_USB_SERVICE_RETIRE_REPORT R;PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID *)0x55,.NowUs=NULL};
  assert(PianoUsbControllerServiceStart(NULL,&C)==EFI_UNSUPPORTED && PianoDwc3ServiceStart(NULL,NULL,&C)==EFI_UNSUPPORTED);
  assert(PianoUsbControllerServicePumpApp(1,1000)==EFI_UNSUPPORTED && PianoDwc3ServicePollBounded(1)==EFI_UNSUPPORTED);
  assert(PianoUsbControllerServiceGetStatus(&S)==EFI_UNSUPPORTED && PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_UNSUPPORTED);
  model_init();configure();cmd("oem setup","FAILcommand disabled by RAM-only policy");cmd("oem shell","FAILcommand disabled by RAM-only policy");cmd("oem simpleinit","FAILcommand disabled by RAM-only policy");
  assert(Halt()==EFI_SUCCESS);for(UINTN I=0;I<ARRAY_SIZE(all);++I){if(all[I]->Active)assert(PianoDmaComplete(all[I],EFI_SUCCESS,TRUE)==EFI_SUCCESS);assert(PianoDmaFree(all[I])==EFI_SUCCESS);}ClearFastboot();
  puts("USB persistent service default-off APIs and rejected OEM navigation PASS; no device.");
#endif
  return 0;
}
