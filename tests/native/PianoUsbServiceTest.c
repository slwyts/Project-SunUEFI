// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Device + Controller + protocol; fake hardware/backend, never devices.
#define PIANO_USB_UFS_FETCH 1
#ifndef PIANO_USB_SERVICE
#define PIANO_USB_SERVICE 1
#endif
#define PIANO_USB_EP0 1
#define PIANO_USB_RAM_BOOT 1
#define USB_FASTBOOT_TEST_MAIN bulk_source_suite
#include "test_usb_fastboot.c"
#include "../../uefi/core/usb/PianoFastbootBoot.c"
#include "../../uefi/core/usb/PianoUsbController.c"
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
EFI_SYSTEM_TABLE *gST;
static EFI_SYSTEM_TABLE service_system_table;
// The HAL fixture cannot validate a native close ledger. Model only the
// trusted validator boundary here; its actual implementation has separate
// owned-peer tests. This checks fresh capture, exact evidence and no replay.
static BOOLEAN proof_validator_accepts;static UINTN proof_validator_calls;
EFI_STATUS PianoOwnedSmmuMakeRetiredUsbProof(CONST PIANO_OWNED_SMMU *C,CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *E,
  CONST PIANO_SMMU_SNAPSHOT *S,PIANO_SMMU_RETIRED_USB_PROOF *P){
  ++proof_validator_calls;assert(C==&mUsbContext && !C->Attached && !C->Domain && !C->TableMemory.Signature);
  assert(E->Revision==1 && E->DeviceCleanupStatus==EFI_SUCCESS && E->ControllerCleanupStatus==EFI_SUCCESS &&
    E->DeviceHalted && E->DmaFreed && E->ClocksReleased && E->GdscReleased && E->ClockReleaseMask==PIANO_USB_CLOCK_RELEASE_MASK);
  if(E->Kind==PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN)assert(E->StartupStatus==EFI_TIMEOUT && !E->DmaBuffersAllocated && !E->DmaBuffersFreed);
  else assert(E->Kind==PIANO_USB_RETIRE_RUNNING && E->DmaBuffersFreed==9);
  assert(S->Valid && S->Device[0].Present && !S->Device[1].Present);
  if(!proof_validator_accepts)return EFI_NOT_READY;
  *P=(PIANO_SMMU_RETIRED_USB_PROOF){.Revision=1,.Valid=TRUE,.UsbContext=C,.Execution=*E,.Retired=*S};return EFI_SUCCESS;
}
static EFI_CLOCK_PROTOCOL clock;
static UINT32 smmu[0x100000/4];static PIANO_SMMU_SNAPSHOT hardware;
static BOOLEAN gpi_registered;
static PIANO_SMMU_DEVICE gpi_routes[2];
// The actual owner validates and supplies this evidence in test_owned_smmu*.c.
// Here the live MMIO register values are modeled independently of that evidence.
EFI_STATUS PianoOwnedSmmuExpectActiveGpi(PIANO_SMMU_SNAPSHOT *S){
  if(!gpi_registered)return EFI_SUCCESS;
  for(UINTN I=0;I<ARRAY_SIZE(gpi_routes);++I){
    CONST PIANO_SMMU_DEVICE *Route=&gpi_routes[I];
    S->Device[I+2]=*Route;
    S->RawSmr[Route->StreamIndex]=Route->Smr;
    S->RawS2cr[Route->StreamIndex]=Route->S2cr;
  }
  return EFI_SUCCESS;
}
static UINTN held,next_clock,closes,installs,removes,full_captures,backend_reads,backend_infos,stage;
static BOOLEAN domain,phy_domain,install_fail,remove_fail,backend_fail,reentry;
static UINTN scenario;
static PIANO_USB_SHUTDOWN *visible_shutdown;
static EFI_STATUS reentry_status,setter_status;
static VOID hw_store(UINTN Offset,UINT32 V){assert(!(Offset&3) && Offset<sizeof(smmu));smmu[Offset/4]=V;}
static VOID hw64(UINTN Offset,UINT64 V){hw_store(Offset,(UINT32)V);hw_store(Offset+4,(UINT32)(V>>32));}
static VOID load_hardware(VOID) {
  memset(smmu,0,sizeof(smmu));hw_store(0,hardware.GlobalControl);hw_store(0x20,hardware.Id0);hw_store(0x24,hardware.Id1);hw_store(0x28,hardware.Id2);hw_store(0x48,hardware.GlobalFault);
  for(UINTN I=0;I<hardware.Groups;++I){hw_store(0x800+4*I,hardware.RawSmr[I]);hw_store(0xC00+4*I,hardware.RawS2cr[I]);}
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I)if(hardware.Device[I].Present) {
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
static BOOLEAN pool_free_warning;
static EFI_STATUS EFIAPI service_pool_free(VOID *Buffer){if(pool_free_warning)return EFI_WARN_STALE_DATA;FreePool(Buffer);return EFI_SUCCESS;}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){assert(Fdt==(VOID *)123);return 1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Bytes) {
  STATIC CONST UINT8 Reg[]={0,0,0,0,0x0a,0x60,0,0,0,0,0,0,0,0,0xd9,0x3c},Iommu[]={0,0,0,1,0,0,0,0x40,0,0,0,0};
  assert(Fdt==(VOID *)123 && Node==1);if(!strcmp(Name,"reg")){*Bytes=16;return Reg;}assert(!strcmp(Name,"iommus"));*Bytes=12;return Iommu;
}
VOID PianoProbeUsbPower(CONST VOID *Fdt){assert(Fdt==(VOID *)123);}
EFI_STATUS PianoUsbPhyStart(EFI_CLOCK_PROTOCOL *Clock){assert(Clock==&clock);return EFI_SUCCESS;}
EFI_STATUS PianoUsbPhyStop(VOID){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI locate_clock(EFI_GUID *Guid,VOID *Registration,VOID **Interface){*Interface=&clock;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_id(EFI_CLOCK_PROTOCOL *C,CONST CHAR8 *Name,UINTN *Id){assert(C==&clock);*Id=!strcmp(Name,"gcc_usb30_prim_gdsc")?100:!strcmp(Name,"gcc_usb3_phy_gdsc")?101:next_clock++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_enable(EFI_CLOCK_PROTOCOL *C,UINTN Id){assert(C==&clock);if(Id==100)domain=TRUE;else if(Id==101){assert(domain);phy_domain=TRUE;}else {assert(Id<PIANO_USB_CLOCK_COUNT && domain && phy_domain);held|=1U<<Id;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI clock_disable(EFI_CLOCK_PROTOCOL *C,UINTN Id){assert(C==&clock && !mUsbShutdownLive);if(Id==100){assert(!held && !phy_domain);domain=FALSE;}else if(Id==101){assert(!held && domain);phy_domain=FALSE;}else {assert(held&(1U<<Id));held&=~(1U<<Id);}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI install(EFI_HANDLE *Handle,...) {
  ++installs;if(install_fail)return EFI_DEVICE_ERROR;
  va_list A;va_start(A,Handle);EFI_GUID *G=va_arg(A,EFI_GUID *);visible_shutdown=va_arg(A,PIANO_USB_SHUTDOWN *);assert(va_arg(A,VOID *)==NULL);va_end(A);
  EFI_GUID Expected=PIANO_USB_SHUTDOWN_GUID;assert(!memcmp(G,&Expected,sizeof(Expected)) && visible_shutdown==&mUsbShutdown && held==PIANO_USB_CLOCK_RELEASE_MASK);*Handle=(VOID *)987;return EFI_SUCCESS;
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
EFI_STATUS PianoSmmuCaptureQuiet(CONST VOID *Fdt,PIANO_SMMU_SNAPSHOT *Snapshot){return PianoSmmuCapture(Fdt,"usb-startup-failed-clean-recheck",Snapshot);}
static PIANO_SMMU_DEVICE bank(UINTN I,UINT16 Sid,UINT64 Root) {
  return (PIANO_SMMU_DEVICE){.Sid=Sid,.StreamIndex=(UINT16)I,.ContextBank=(UINT8)I,.Present=TRUE,.Enabled=TRUE,.Smr=0x80000000U|Sid,.S2cr=(UINT32)I,
    .Sctlr=0x1e5,.Cbar=0x10000,.Cba2r=1,.Tcr=0x00802519,.Tcr2=0x00038001,.Mair0=0xff,.Ttbr0=Root};
}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D) {
  assert(mUsbShutdownLive && held==PIANO_USB_CLOCK_RELEASE_MASK && domain);C->Before=hardware;C->Fdt=Fdt;
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
static PIANO_FB_STORAGE backend={ .Context=(VOID *)456,.Ready=ready_backend,.Info=info_backend,.ReadBlocks=read_backend };
static VOID tick(UINTN Us) {
  if(Us!=1000 || stage==9 || mExperimentContractFailed || !mRing.Active ||
     !(regs[0xC704/4]&BIT31) || (stage==0 && !mPending[0]))return;
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
  gpi_registered=FALSE;
  regs[0xC120/4]=0x33313130;
  domain=install_fail=remove_fail=backend_fail=reentry=pool_free_warning=FALSE;visible_shutdown=NULL;
  mUsbCleanupBlocked=mUsbControllerRunning=mCombinedBusy=mCoexistReady=mCoexistFailed=mProxyBusy=mUsbShutdownLive=FALSE;mUsbShutdownHandle=NULL;
  mExperimentRunning=mExperimentHasStorage=mExperimentContractFailed=FALSE;mExperimentCheck=NULL;mExperimentCheckContext=NULL;
  ZeroMem(&mUsbContext,sizeof(mUsbContext));ZeroMem(&hardware,sizeof(hardware));
  hardware.Valid=TRUE;hardware.Base=0x15000000;hardware.Window=sizeof(smmu);hardware.ContextBase=0x8000;hardware.PageShift=12;hardware.Groups=4;hardware.Banks=2;hardware.Id0=4;hardware.Id1=0x20000002;
  hardware.Device[0]=bank(0,0x60,0x81000000);hardware.Device[1].Sid=0x40;hardware.RawSmr[0]=hardware.Device[0].Smr;load_hardware();
  extra_mmio_read=extra_read;gBS=&bs;gRT=&rt;bs.Stall=stall;bs.FreePool=service_pool_free;rt.ResetSystem=reset;bs.LocateProtocol=locate_clock;bs.InstallMultipleProtocolInterfaces=install;bs.UninstallMultipleProtocolInterfaces=uninstall;
  ZeroMem(&service_system_table,sizeof(service_system_table));service_system_table.BootServices=gBS;gST=&service_system_table;
  ZeroMem(&clock,sizeof(clock));clock.Version=0x1000b;clock.GetClockID=clock.GetClockPowerDomainID=clock_id;clock.EnableClock=clock.EnableClockPowerDomain=clock_enable;clock.DisableClock=clock.DisableClockPowerDomain=clock_disable;
  lifecycle_continue=lifecycle_hold_ack=FALSE;stall_hook=tick;
}
#if PIANO_USB_SERVICE
static EFI_TPL current_tpl=TPL_APPLICATION;
static UINTN bs_calls,event_creates,event_closes,clock_calls,proof_calls;
static UINT64 fake_now;
static BOOLEAN in_timer,event_failure,event_warning,timer_cancel_warning,event_close_warning,clock_warning,domain_warning;
static VOID (EFIAPI *timer_notify)(EFI_EVENT,VOID *), (EFIAPI *exit_notify)(EFI_EVENT,VOID *);
static VOID (*restore_tpl_hook)(VOID);
EFI_GUID gEfiEventExitBootServicesGuid={0x27ABF055,0xB1B8,0x4C26,{0x80,0x48,0x74,0x8F,0x37,0xBA,0xA2,0xDF}};
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){assert(!in_timer);++bs_calls;EFI_TPL Old=current_tpl;assert(New>=Old);current_tpl=New;return Old;}
static VOID EFIAPI restore_tpl(EFI_TPL Old){
  assert(!in_timer);++bs_calls;assert(Old<=current_tpl);current_tpl=Old;
  // Lowering TPL can dispatch a pending ExitBootServices notification before
  // the caller resumes. Only tests that exercise that boundary install a hook.
  if(restore_tpl_hook)restore_tpl_hook();
}
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
static VOID service_payload(CONST VOID *Data,UINTN N) {
  assert(mPending[2] && !mPending[3] && N<=mPosted[2]);CopyMem(mBulkRx.Cpu,Data,N);
  DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-(UINT32)N;T->Control&=~BIT0;service_event(0xC044);
}
static VOID service_out(CONST CHAR8 *Text){service_payload(Text,strlen(Text));}
static VOID service_ack_one(VOID) {
  assert(mPending[3]);
  DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;service_event(0xC046);
}
static VOID service_ack(VOID) {
  while(mPending[3])service_ack_one();
}
static VOID service_end(UINT8 Ep){
  assert(mEnding[Ep]);
  ((DWC_TRB *)mTrbs[Ep].Cpu)->Control&=~BIT0;
  service_event((8U<<24)|(7U<<6)|((UINT32)Ep<<1));
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
#if PIANO_USB_HOST
static BOOLEAN host_active,phy_lost;
static UINT8 phy_orientation=1;
static UINTN host_starts,host_stops,host_stop_calls,phy_switches;
static EFI_STATUS host_stop_status,typec_status,phy_switch_status;
static PIANO_USB_TYPEC_STATE typec;
UINT8 PianoUsbPhyOrientation(VOID){return phy_orientation;}
EFI_STATUS PianoUsbPhySetOrientation(UINT8 Orientation){
  assert(current_tpl==TPL_APPLICATION && mRole.Parked && !mService.State.Started && !host_active);
  assert((Dr(0xC70C)&BIT22) && !(Dr(0xC704)&BIT31));
  assert(mUsbContext.Attached && !closes && !removes);
  for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(!all[I]->Signature);
  ++phy_switches;
  if(phy_switch_status==EFI_SUCCESS)phy_orientation=Orientation;
  else if(phy_lost)phy_orientation=0;
  return phy_switch_status;
}
EFI_STATUS PianoUsbHostStart(PIANO_DMA_DEVICE *Device){
  assert(Device==&mUsbDevice && current_tpl==TPL_APPLICATION && !host_active && !mService.State.Started);
  ++host_starts;host_active=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoUsbHostStop(VOID){
  assert(current_tpl==TPL_APPLICATION && host_active && !mService.State.Started);
  ++host_stop_calls;
  if(host_stop_status!=EFI_SUCCESS)return host_stop_status;
  ++host_stops;host_active=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoUsbHostActive(VOID){return host_active;}
BOOLEAN PianoUsbHostRetained(VOID){return FALSE;}
EFI_STATUS PianoUsbHostFenceExit(VOID){assert(!host_active);return EFI_SUCCESS;}
EFI_STATUS PianoUsbTypecRead(CONST VOID *Fdt,PIANO_USB_TYPEC_STATE *State){
  assert(Fdt==(VOID *)123 && current_tpl==TPL_APPLICATION);
  *State=typec;return typec_status;
}
static VOID typec_poll(VOID){
  fake_now+=250000;
  assert(PianoUsbControllerServicePumpApp(PIANO_PRODUCT_PUMP_GUI,5000)==EFI_SUCCESS);
}
#endif
static PIANO_PRODUCT_RUNTIME_PROTOCOL navigation_runtime,navigation_replacement;
static UINTN navigation_case,navigation_queries,navigation_requests,navigation_action;
static BOOLEAN navigation_alive=TRUE;
static EFI_STATUS EFIAPI navigation_pump(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Reason,UINTN Budget){(VOID)This;(VOID)Reason;(VOID)Budget;assert(!"navigation may not recursively pump");return EFI_UNSUPPORTED;}
static BOOLEAN EFIAPI navigation_is_alive(PIANO_PRODUCT_RUNTIME_PROTOCOL *This){assert(This==&navigation_runtime && current_tpl==TPL_APPLICATION && !in_timer);return navigation_alive;}
static EFI_STATUS EFIAPI navigation_pending(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 *Action,UINT64 *Seq){(VOID)This;(VOID)Action;(VOID)Seq;assert(!"USB navigation must only latch");return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI navigation_ack(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT64 Seq){(VOID)This;(VOID)Seq;assert(!"USB does not consume UI actions");return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI navigation_request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action){
  assert(This==&navigation_runtime && current_tpl==TPL_APPLICATION && !in_timer && !closes && held==PIANO_USB_CLOCK_RELEASE_MASK && domain && mService.State.Phase==PianoUsbServiceListening);
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
    CONST CHAR8 *Commands[]={"oem setup","oem shell","oem simpleinit","oem boot-stable"};CONST UINT32 Actions[]={PIANO_PRODUCT_ACTION_SETUP,PIANO_PRODUCT_ACTION_SHELL,PIANO_PRODUCT_ACTION_SIMPLEINIT,PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE};
    for(UINTN I=0;I<ARRAY_SIZE(Commands);++I){UINTN Alloc=allocations;service_out(Commands[I]);assert(mPending[3] && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4) && navigation_action==Actions[I]);service_ack();assert(!closes && !clock_calls && allocations==Alloc && mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);}
    assert(navigation_queries==4 && navigation_requests==4);service_out("getvar:version");service_ack();
  } else {
    if(Test==6 || Test==7){service_out("oem setup");service_ack();assert(navigation_requests==1);if(Test==7)navigation_runtime.RequestAction=navigation_changed_request;}
    if(Test==10){
      CopyMem(mBulkRx.Cpu,"oem setup",9);DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-9;T->Control&=~BIT0;
      publish(0xC044);timer_tick();assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_ABORTED);
      assert(mService.State.ServicesLost && !navigation_requests && !mFrames && !mPending[3] && !closes && held==PIANO_USB_CLOCK_RELEASE_MASK);
      free_cold_model();return;
    }
    service_out("oem shell");CONST CHAR8 *Failure="FAILUI navigation backend unavailable";
    assert(mPending[3] && mPosted[3]==strlen(Failure) && !memcmp(mBulkTx.Cpu,Failure,strlen(Failure)));service_ack();
    assert(!closes && !clock_calls && mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);
    if(Test!=4 && Test!=5 && Test!=6 && Test!=7)assert(!navigation_requests);
  }
  PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS && R.Clean);free_cold_model();
}
static UINTN ramlog_case,ramlog_calls,ramlog_generation,ramlog_calls_at_loss;
static EFI_STATUS ramlog_result;
static EFI_STATUS ramlog_poison(VOID *Context){(VOID)Context;assert(!"caller configuration must be copied");return EFI_DEVICE_ERROR;}
static EFI_STATUS before_ramlog(VOID *Context) {
  assert(Context==(VOID *)0x55 && current_tpl==TPL_APPLICATION && !in_timer);
  assert(mService.State.Busy && mService.State.Phase==PianoUsbServiceListening && mLogGeneration==ramlog_generation);
  ++ramlog_calls;
  if(ramlog_case==5){ // Emulate asynchronous EBS while the callback runs.
    exit_notify((VOID *)0x102,NULL);ramlog_calls_at_loss=bs_calls;return EFI_SUCCESS;
  }
  if(ramlog_result!=EFI_SUCCESS)return ramlog_result;
  if(ramlog_case!=4){
    UINT32 *Header=test_console;CONST CHAR8 *Text="SMEM immutable reemit before freeze\n";UINTN Bytes=strlen(Text);
    assert(Header && Header[0]==0x43474244 && Header[1]==Header[2]);
    CopyMem((UINT8 *)(Header+3)+Header[1],Text,Bytes);Header[1]+=Bytes;Header[2]+=Bytes;
  }
  return EFI_SUCCESS;
}
static VOID run_ramlog(UINTN Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;ramlog_case=Test;ramlog_calls=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  ramlog_result=Test==1?EFI_DEVICE_ERROR:Test==2?EFI_WARN_STALE_DATA:Test==6?EFI_UNSUPPORTED:Test==7?EFI_ABORTED:EFI_SUCCESS;
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&backend,.BeforeRamlog=Test==3?NULL:before_ramlog};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);service_enumerate();
  assert(mPersistent.Config.BeforeRamlog==Config.BeforeRamlog && mService.Config.BeforeRamlog==Config.BeforeRamlog);
  Config.BeforeRamlog=ramlog_poison;Config.Context=NULL; // Both actual owners must retain their copied config.
  UINT32 *Header=calloc(1,0x200000);assert(Header);test_console=Header;CONST CHAR8 *Initial="SUNUEFI_RAMLOG_BEGIN\ninitial console\n";
  Header[0]=0x43474244;Header[1]=Header[2]=(UINT32)strlen(Initial);CopyMem(Header+3,Initial,strlen(Initial));
  if(Test==4)Header[1]=Header[2]+1;
  if(Test==1||Test==2||Test==6||Test==7)test_console=NULL; // Any attempted freeze after rejection would fault.
  ramlog_generation=mLogGeneration;
  if(Test==5){
    CopyMem(mBulkRx.Cpu,"oem ramlog",10);DWC_TRB *T=mTrbs[2].Cpu;T->Size=mPosted[2]-10;T->Control&=~BIT0;publish(0xC044);timer_tick();
    assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_ABORTED && ramlog_calls==1 && bs_calls==ramlog_calls_at_loss);
    assert(mService.State.ServicesLost && !mLogSnapshot && !mFastboot.Upload && !mFrames && !mPending[3]);
    free(Header);test_console=NULL;free_cold_model();return;
  }
  service_out("oem ramlog");assert(ramlog_calls==(Test==3?0:1));
  if(Test==0||Test==3){
    assert(mPending[3] && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4));
    assert(mLogGeneration>ramlog_generation && mFastLogBytes==Header[2] && mFastboot.UploadBytes==Header[2]);
    assert(!memcmp(mFastboot.Upload,Header+3,Header[2]));
    if(Test==0){CONST CHAR8 *Text="SMEM immutable reemit before freeze\n";assert(!memcmp(mLogSnapshot+strlen(Initial),Text,strlen(Text)));}
    UINT8 Saved[256];UINTN Bytes=mFastboot.UploadBytes;assert(Bytes<=sizeof(Saved));CopyMem(Saved,mFastboot.Upload,Bytes);
    memset(Header+3,'X',Bytes);assert(!memcmp(mFastboot.Upload,Saved,Bytes));service_ack();
    if(Test==0){ // Exactly one invocation for each additional ramlog command.
      ramlog_generation=mLogGeneration;service_out("oem ramlog");assert(ramlog_calls==2 && mLogGeneration>ramlog_generation);service_ack();
    }
  }else{
    CONST CHAR8 *Failure="FAILRAM log snapshot unavailable";assert(mPending[3] && mPosted[3]==32 && !memcmp(mBulkTx.Cpu,Failure,32));
    assert(mLogGeneration==ramlog_generation && !mFastLogBytes && !mFastLogCrc && !mFastLogGeneration && !mLogValid && !mFastboot.Upload);
    if(Test!=4)assert(!mLogSnapshot);service_ack();
  }
  assert(mService.State.Phase==PianoUsbServiceListening && !mService.State.Retained);
  PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS && R.Clean);
  free(Header);test_console=NULL;free_cold_model();
}

typedef enum {
  StatusCopied,
  StatusUnbound,
  StatusCallbackError,
  StatusCallbackWarning,
  StatusExitDuringReport,
  StatusInvalidRows,
  StatusCachedBadConsole,
  StatusQueueLimit,
  StatusQueueOverflow,
  StatusExitBeforeCallback,
  StatusExitBeforeRow,
  StatusCaseCount
} STATUS_CASE;

static STATUS_CASE status_case;
static UINTN status_calls,status_calls_at_loss,status_frames_at_loss;
static EFI_STATUS status_send_after_loss;

static VOID status_restore_exit(VOID) {
  // The seven fixed status rows precede both the report callback and its
  // first Send. Earlier event-queue TPL restores must leave the fixture live.
  if(mFrameCount!=7)return;
  restore_tpl_hook=NULL;
  exit_notify((VOID *)0x102,NULL);
  status_calls_at_loss=bs_calls;status_frames_at_loss=mFrameCount;
}

static EFI_STATUS status_poison(VOID *Context,PIANO_FB_SEND Send,VOID *SendContext) {
  (VOID)Context;(VOID)Send;(VOID)SendContext;
  assert(!"status callback configuration must be copied");
  return EFI_DEVICE_ERROR;
}

static EFI_STATUS report_status(VOID *Context,PIANO_FB_SEND Send,VOID *SendContext) {
  assert(Context==(VOID *)0x55 && current_tpl==TPL_APPLICATION && !in_timer);
  assert(mService.State.Busy && mService.State.Phase==PianoUsbServiceListening);
  ++status_calls;

  if(status_case==StatusExitBeforeRow){
    restore_tpl_hook=status_restore_exit;
    status_send_after_loss=Send(SendContext,"INFOafter restore",17);
    assert(status_send_after_loss==EFI_ABORTED && !restore_tpl_hook);
    assert(bs_calls==status_calls_at_loss && mFrameCount==status_frames_at_loss);
    return EFI_SUCCESS;
  }

  if(status_case==StatusInvalidRows){
    CHAR8 Oversize[65];memset(Oversize,'x',sizeof(Oversize));CopyMem(Oversize,"INFO",4);
    UINTN Frames=mFrameCount;
    assert(Send(SendContext,NULL,4)==EFI_INVALID_PARAMETER);
    assert(Send(SendContext,"INF",3)==EFI_INVALID_PARAMETER);
    assert(Send(SendContext,"OKAY",4)==EFI_INVALID_PARAMETER);
    assert(Send(SendContext,Oversize,sizeof(Oversize))==EFI_INVALID_PARAMETER);
    current_tpl=TPL_CALLBACK;
    assert(Send(SendContext,"INFOwrong tpl",13)==EFI_UNSUPPORTED);
    current_tpl=TPL_APPLICATION;
    assert(mFrameCount==Frames);
  }

  if(status_case==StatusExitDuringReport){
    assert(Send(SendContext,"INFObefore exit",15)==EFI_SUCCESS);
    exit_notify((VOID *)0x102,NULL);
    status_calls_at_loss=bs_calls;status_frames_at_loss=mFrameCount;
    status_send_after_loss=Send(SendContext,"INFOafter exit",14);
    assert(status_send_after_loss==EFI_ABORTED);
    assert(bs_calls==status_calls_at_loss && mFrameCount==status_frames_at_loss);
    return EFI_SUCCESS;
  }

  UINTN Rows=status_case==StatusQueueLimit?FB_QUEUE_FRAMES-8:status_case==StatusQueueOverflow?FB_QUEUE_FRAMES-6:2;
  for(UINTN I=0;I<Rows;++I){
    CHAR8 Row[32];int Bytes=snprintf(Row,sizeof(Row),"INFOmemory row %llu",(unsigned long long)I);
    assert(Bytes>4 && (UINTN)Bytes<sizeof(Row));
    EFI_STATUS Result=Send(SendContext,Row,(UINTN)Bytes);
    if(Result!=EFI_SUCCESS)return Result;
  }
  if(status_case==StatusCallbackError)return EFI_DEVICE_ERROR;
  if(status_case==StatusCallbackWarning)return EFI_WARN_STALE_DATA;
  return EFI_SUCCESS;
}

static BOOLEAN status_has_frame(CONST CHAR8 *Text) {
  for(FB_FRAME *Frame=mFrames;Frame;Frame=Frame->Next)
    if(Frame->Bytes==strlen(Text) && !CompareMem(Frame->Data,Text,Frame->Bytes))return TRUE;
  return FALSE;
}

static VOID run_status(STATUS_CASE Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;
  bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  status_case=Test;status_calls=status_calls_at_loss=status_frames_at_loss=0;
  status_send_after_loss=EFI_NOT_STARTED;restore_tpl_hook=NULL;

  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&backend,
    .ReportStatus=Test==StatusUnbound?NULL:report_status};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);service_enumerate();
  assert(mPersistent.Config.ReportStatus==Config.ReportStatus && mService.Config.ReportStatus==Config.ReportStatus);
  Config.ReportStatus=status_poison;Config.Context=NULL;

  if(Test==StatusCachedBadConsole){
    // A failed snapshot must still leave diagnostic evidence. Making the live
    // console inaccessible proves that oem status only formats that evidence.
    UINT32 Header[]={0xdeadbeef,0x2b,0x7c};test_console=Header;
    service_out("oem ramlog");service_ack();test_console=NULL;
    assert(mLogSnapshotStatus==EFI_COMPROMISED_DATA && !strcmp(mLogSnapshotStage,"header"));
  }

  // Timer polling may queue the command but never invoke a report callback.
  CopyMem(mBulkRx.Cpu,"oem status",10);DWC_TRB *Trb=mTrbs[2].Cpu;
  Trb->Size=mPosted[2]-10;Trb->Control&=~BIT0;publish(0xC044);timer_tick();
  assert(!status_calls);
  current_tpl=TPL_CALLBACK;
  assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_UNSUPPORTED && !status_calls);
  current_tpl=TPL_APPLICATION;
  if(Test==StatusExitBeforeCallback)restore_tpl_hook=status_restore_exit;
  EFI_STATUS Result=PianoUsbControllerServicePumpApp(1,5000);
  assert(status_calls==((Test==StatusUnbound || Test==StatusExitBeforeCallback)?0:1));

  if(Test==StatusExitDuringReport || Test==StatusExitBeforeCallback || Test==StatusExitBeforeRow){
    assert(Result==EFI_ABORTED && mService.State.ServicesLost && mPersistent.ServicesLost);
    assert(bs_calls==status_calls_at_loss && mFrameCount==status_frames_at_loss);
    assert(mFrameCount==(Test==StatusExitDuringReport?8:7) && !restore_tpl_hook);
    assert(!mPending[3] && !status_has_frame("OKAY") && !status_has_frame("FAILstatus report unavailable"));
    free_cold_model();return;
  }
  if(Test==StatusQueueOverflow){
    // Fixed and callback INFO rows fill the queue before the next row fails.
    assert(Result==EFI_OUT_OF_RESOURCES && mFrameCount==FB_QUEUE_FRAMES && !mPending[3]);
    assert(mService.State.LastStatus==EFI_OUT_OF_RESOURCES);
    free_cold_model();return;
  }

  assert(Result==EFI_SUCCESS && mPending[3]);
  UINTN Frames=0;
  for(FB_FRAME *Frame=mFrames;Frame;Frame=Frame->Next){
    assert(Frame->Bytes>=4 && Frame->Bytes<=64);
    ++Frames;
  }
  assert(Frames==mFrameCount && Frames==(Test==StatusQueueLimit?FB_QUEUE_FRAMES:Test==StatusUnbound?8:10));
  assert(status_has_frame("INFOstorage-policy:read-only"));
  if(Test==StatusCallbackError || Test==StatusCallbackWarning){
    assert(status_has_frame("FAILstatus report unavailable") && !status_has_frame("OKAY"));
  }else{
    assert(status_has_frame("OKAY") && !status_has_frame("FAILstatus report unavailable"));
  }
  if(Test==StatusCachedBadConsole){
    CHAR8 Snapshot[64];
    snprintf(Snapshot,sizeof(Snapshot),"INFOlog-snapshot header status=%016llx",(unsigned long long)EFI_COMPROMISED_DATA);
    assert(status_has_frame("INFOlog-header sig=deadbeef start=0000002b size=0000007c"));
    assert(status_has_frame(Snapshot));
  }

  service_ack();
  // A report rejection must leave ordinary Fastboot commands usable.
  service_out("getvar:version");service_ack();
  assert(mService.State.Phase==PianoUsbServiceListening && !mService.State.Retained);
  PIANO_USB_SERVICE_RETIRE_REPORT Retire;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retire)==EFI_SUCCESS && Retire.Clean);
  free_cold_model();
}

static VOID run_idle_activity(VOID){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;bs_calls=event_creates=event_closes=clock_calls=proof_calls=0;
  timer_notify=exit_notify=NULL;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID*)0x55,.NowUs=now_us};assert(PianoUsbControllerServiceStart((VOID*)123,&C)==EFI_SUCCESS);service_enumerate();
  PIANO_DWC3_SERVICE_STATUS S;assert(PianoUsbControllerServicePumpApp(1,1000)==EFI_SUCCESS);
  assert(PianoUsbControllerServiceGetStatus(&S)==EFI_SUCCESS&&!S.BulkActive&&!S.WorkPending&&mPending[2]&&!mPending[3]);
  service_out("getvar:version");assert(PianoUsbControllerServiceGetStatus(&S)==EFI_SUCCESS&&S.BulkActive&&!S.WorkPending&&mPending[3]&&!mService.Count);
  service_ack();assert(PianoUsbControllerServiceGetStatus(&S)==EFI_SUCCESS&&!S.BulkActive&&!S.WorkPending&&mPending[2]);
  service_out("download:00000004");service_ack();assert(PianoUsbControllerServiceGetStatus(&S)==EFI_SUCCESS&&S.BulkActive&&mFastboot.Receiving&&mPending[2]&&!mPending[3]);
  PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS&&R.Clean);
  assert(PianoUsbControllerServiceGetStatus(&S)==EFI_SUCCESS&&!S.BulkActive);free_cold_model();
}
typedef enum {
  ReplyFetchCancelled,ReplyFetchCancellationReset,ReplyDmaEndMissing,ReplyCompletionDelayed,
  ReplyNormalFetch,ReplyNormalDownload,ReplyPartialDownload,ReplyRebootCancelled,ReplyCaseCount
} REPLY_CASE;
static EFI_STATUS reply_fetch_info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info){
  EFI_STATUS Status=info_backend(Context,Name,Info);
  if(Status==EFI_SUCCESS)Info->Bytes=PIANO_FASTBOOT_MAX_FETCH_BYTES;
  return Status;
}
static EFI_STATUS reply_fetch_read(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){
  assert(Context==(VOID *)456 && Info->Token==(VOID *)789 && !Lba && Bytes==PIANO_FASTBOOT_MAX_FETCH_BYTES);
  ++backend_reads;memset(Buffer,0xa5,Bytes);return EFI_SUCCESS;
}
static VOID run_reply_recovery(REPLY_CASE Test){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;
  bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_FB_STORAGE Storage={.Context=(VOID *)456,.Ready=ready_backend,.Info=reply_fetch_info,.ReadBlocks=reply_fetch_read,
    .MaxFetchBytes=PIANO_FASTBOOT_MAX_FETCH_BYTES};
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Storage};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);
  service_enumerate();
  assert(mBulkRx.Bytes==USB_FASTBOOT_BULK_BYTES && mBulkTx.Bytes==USB_FASTBOOT_BULK_BYTES);
  assert(mPosted[2]==BulkMps()); // Commands still occupy one USB packet.
  service_out("getvar:max-fetch-size");
  assert(mPosted[3]==14 && !memcmp(mBulkTx.Cpu,"OKAY0x00040000",14));service_ack();
  fake_now+=10*FASTBOOT_REPLY_IDLE_US;
  assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && mPending[2] && !mEnding[2]);
  UINTN AddressWrites=address_writes,SessionWrites=session_writes;
  UINT32 Dcfg=regs[0xC700/4],EpEnable=regs[0xC720/4];
  if(Test==ReplyPartialDownload || Test==ReplyNormalDownload){
    service_out("download:00010011");
    fake_now+=2*FASTBOOT_REPLY_IDLE_US;
    assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && mFastboot.Receiving && !mEnding[3]);
    service_ack();assert(mPending[2] && !mPending[3] && mPosted[2]==USB_FASTBOOT_BULK_BYTES);
    static UINT8 Data[USB_FASTBOOT_BULK_BYTES];
    for(UINTN I=0;I<sizeof(Data);++I)Data[I]=(UINT8)(I*73+I/251);
    service_payload(Data,sizeof(Data));
    fake_now+=2*FASTBOOT_REPLY_IDLE_US;
    assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && !mEnding[2] && mPending[2]);
    assert(mFastboot.Receiving && mFastboot.Received==sizeof(Data) && mPosted[2]==BulkMps() && !mFrames);
    if(Test==ReplyNormalDownload){
      service_payload(Data,17);
      assert(mFastboot.Complete && mPending[3] && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4));
      assert(!memcmp(mFastboot.Download,Data,sizeof(Data)) && !memcmp(mFastboot.Download+sizeof(Data),Data,17));
      service_ack();
    }else{
      service_event(0x101);assert(mEnding[2] && mBulkRx.Active && !mFastboot.Download && !mConfigured);
      service_end(2);service_enumerate();
    }
  }else if(Test==ReplyCompletionDelayed){
    service_out("getvar:version");
    // The TRB is finished but the corresponding event has not arrived yet.
    DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;
    fake_now=mService.ReplyPostedUs+FASTBOOT_REPLY_IDLE_US;
    assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && mPending[3] && !mEnding[3] && mBulkTx.Active);
    service_event(0xC046);assert(mPending[2] && !mPending[3] && !mFrames);
  }else{
    if(Test==ReplyRebootCancelled){
      service_out("reboot");
      assert(mFastboot.RebootRequested && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4));
    }else{
      service_out("fetch:xbl_config_a:0x0:0x40000");
      assert(backend_reads==1 && mPosted[3]==12 && !memcmp(mBulkTx.Cpu,"DATA00040000",12));
      service_ack_one();
      assert(mPending[3] && mPosted[3]==USB_FASTBOOT_BULK_BYTES && mFrameCount==2);
    }
    if(Test==ReplyNormalFetch){
      UINT64 Start=fake_now;
      for(UINTN N=0;N<PIANO_FASTBOOT_MAX_FETCH_BYTES/USB_FASTBOOT_BULK_BYTES;++N){
        fake_now+=FASTBOOT_REPLY_IDLE_US/2;
        assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && !mEnding[3]);
        assert(mPosted[3]==USB_FASTBOOT_BULK_BYTES && ((UINT8 *)mBulkTx.Cpu)[USB_FASTBOOT_BULK_BYTES-1]==0xa5);
        service_ack_one();
      }
      assert(fake_now-Start>FASTBOOT_REPLY_IDLE_US && mPosted[3]==4 && !memcmp(mBulkTx.Cpu,"OKAY",4));
      service_ack_one();
    }else{
      UINTN Posted=mPosted[3];UINT8 In[USB_FASTBOOT_BULK_BYTES];CopyMem(In,mBulkTx.Cpu,Posted);
      fake_now=mService.ReplyPostedUs+FASTBOOT_REPLY_IDLE_US-1000;
      assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && mPending[3] && !mEnding[3] && mFrames);
      fake_now=mService.ReplyPostedUs+FASTBOOT_REPLY_IDLE_US;
      assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && mService.ReplyAborting);
      assert(mEnding[3] && mBulkTx.Active && mTrbs[3].Active && !mFrames && !mPending[2]);
      assert(!memcmp(mBulkTx.Cpu,In,Posted) && !mFastboot.RebootRequested && !resets);
      assert((regs[0xC83C/4]&(15U|BIT8|BIT11))==(8U|BIT8) && address_writes==AddressWrites);
      // A stale transfer-complete event cannot end a still-owned DMA buffer.
      service_event(0xC046);
      assert(mEnding[3] && mBulkTx.Active && mTrbs[3].Active && !mPending[2]);
      if(Test==ReplyDmaEndMissing){
        fake_now=mService.ReplyAbortUs+FASTBOOT_REPLY_END_US;
        assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_TIMEOUT);
        assert(mService.State.Action==PianoUsbServiceActionFault && mBulkTx.Active && mTrbs[3].Active && !mPending[2]);
        free_cold_model();return;
      }
      if(Test==ReplyFetchCancellationReset){
        service_event(0x101);
        assert(!mConfigured && !mBulkLive && mEnding[3] && mBulkTx.Active);
        service_end(3);service_enumerate();
      }else{
        // Even a valid data completion after cancellation cannot send the
        // discarded OKAY or post a new OUT before the END acknowledgement.
        service_ack_one();
        assert(mEnding[3] && !mPending[2] && !mFrames);
        service_end(3);
        assert(regs[0xC700/4]==Dcfg && regs[0xC720/4]==EpEnable && address_writes==AddressWrites);
      }
    }
  }
  assert(session_writes==SessionWrites && !resets && mConfigured && !mService.ReplyAborting && !mFrames && mPending[2]);
  service_out("getvar:product");
  static CONST CHAR8 Product[]="OKAYpiano-sunuefi";
  assert(mFrameCount==1 && mPosted[3]==sizeof(Product)-1 && !memcmp(mBulkTx.Cpu,Product,sizeof(Product)-1));
  service_ack();
  PIANO_USB_SERVICE_RETIRE_REPORT Retire;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retire)==EFI_SUCCESS && Retire.Clean);
  free_cold_model();
}
typedef enum {
  FlashUsbErase,FlashUsbSparseFill,FlashUsbDisconnect,FlashUsbDisconnectOnFlush,
  FlashUsbBusReset,FlashUsbExitDuringWrite,FlashUsbMediaChanged,FlashUsbAbortDiagnostics,
  FlashUsbStop,FlashUsbProgressCancelled,FlashUsbCaseCount
} FLASH_USB_CASE;
#define FLASH_TEST_GIB (1ULL<<30)
static FLASH_USB_CASE flash_case;
static UINT64 flash_size,flash_bytes_written;
static UINTN flash_prepares,flash_writes,flash_reads,flash_flushes;
static BOOLEAN flash_needs_read,flash_nonzero;
static UINTN flash_last_bytes;
static UINT8 flash_last_block[PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES];
static EFI_STATUS flash_info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info){
  assert(Context==(VOID *)456 && !in_timer && current_tpl==TPL_APPLICATION);
  if(strcmp(Name,"sunuefi_esp"))return EFI_NOT_FOUND;
  *Info=(PIANO_FB_PARTITION_INFO){.Name="sunuefi_esp",.Bytes=flash_size,.BlockSize=4096,.Token=&flash_size};
  return EFI_SUCCESS;
}
static VOID flash_check_io(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info){
  assert(Context==(VOID *)456 && !in_timer && current_tpl==TPL_APPLICATION);
  assert(!strcmp(Info->Name,"sunuefi_esp") && Info->Token==&flash_size && Info->Bytes==flash_size);
}
static EFI_STATUS flash_prepare(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info){
  flash_check_io(Context,Info);++flash_prepares;return EFI_SUCCESS;
}
static EFI_STATUS flash_write(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,CONST VOID *Data){
  flash_check_io(Context,Info);
  assert(flash_prepares==1 && !flash_flushes && !flash_needs_read);
  assert(flash_bytes_written<Info->Bytes && Lba==flash_bytes_written/Info->BlockSize);
  assert(Bytes==MIN(sizeof(flash_last_block),Info->Bytes-flash_bytes_written));
  if(!flash_writes || flash_bytes_written+Bytes==Info->Bytes){
    static CONST UINT8 Fill[]={0x78,0x56,0x34,0x12};
    for(UINTN I=0;I<Bytes;++I)assert(((CONST UINT8 *)Data)[I]==(flash_nonzero?Fill[I%4]:0));
  }
  memcpy(flash_last_block,Data,Bytes);flash_last_bytes=Bytes;++flash_writes;
  flash_bytes_written+=Bytes;flash_needs_read=TRUE;
  if(flash_case==FlashUsbDisconnect || flash_case==FlashUsbBusReset){
    // The hardware reports the link change while storage is active.
    publish(flash_case==FlashUsbBusReset?0x101:1);timer_tick();
    assert(mService.Count==1);
  }else if(flash_case==FlashUsbExitDuringWrite){
    current_tpl=TPL_NOTIFY;in_timer=TRUE;exit_notify((VOID *)0x102,NULL);in_timer=FALSE;current_tpl=TPL_APPLICATION;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS flash_read(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){
  flash_check_io(Context,Info);
  assert(flash_needs_read && Bytes==flash_last_bytes && Lba==(flash_bytes_written-Bytes)/Info->BlockSize);
  flash_needs_read=FALSE;++flash_reads;memcpy(Buffer,flash_last_block,Bytes);return EFI_SUCCESS;
}
static EFI_STATUS flash_flush(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info){
  flash_check_io(Context,Info);assert(!flash_needs_read);++flash_flushes;
  if(flash_case==FlashUsbDisconnectOnFlush){publish(1);timer_tick();}
  return flash_case==FlashUsbMediaChanged?EFI_MEDIA_CHANGED:EFI_SUCCESS;
}
static VOID run_partition_write(FLASH_USB_CASE Test){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  flash_case=Test;flash_nonzero=Test==FlashUsbSparseFill;
  BOOLEAN Large=Test==FlashUsbErase || Test==FlashUsbSparseFill;
  flash_size=Large?FLASH_TEST_GIB:Test==FlashUsbProgressCancelled?2*FLASH_PROGRESS_BYTES:2*sizeof(flash_last_block);
  PIANO_FB_STORAGE Storage={
    .Context=(VOID *)456,.Ready=ready_backend,.Info=flash_info,.PrepareWrite=flash_prepare,
    .ReadBlocks=flash_read,.WriteBlocks=flash_write,.Flush=flash_flush
  };
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Storage};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);
  service_enumerate();
  if(Test==FlashUsbErase){
    service_out("getvar:storage-policy");
    assert(status_has_frame("OKAYexplicit-partition-writes"));
    service_ack();
    service_out("oem status");
    assert(status_has_frame("INFOstorage-policy:explicit-partition-writes"));
    service_ack();
  }
  if(flash_nonzero || Test==FlashUsbDisconnect || Test==FlashUsbExitDuringWrite){
    // An Android sparse FILL record encodes the whole test partition in 44 bytes.
    UINT8 Sparse[44]={0x3a,0xff,0x26,0xed,1,0,0,0,28,0,12,0,0,16,0,0};
    DiagPut32(Sparse+16,(UINT32)(flash_size/4096));DiagPut32(Sparse+20,1);
    DiagPut16(Sparse+28,0xcac2);DiagPut32(Sparse+32,(UINT32)(flash_size/4096));
    DiagPut32(Sparse+36,16);DiagPut32(Sparse+40,flash_nonzero?0x12345678:0);
    service_out("download:0000002c");service_ack();
    service_payload(Sparse,sizeof(Sparse));service_ack();
    service_out("flash:sunuefi_esp");
  }else service_out("erase:sunuefi_esp");
  PIANO_DWC3_SERVICE_STATUS State;
  assert(PianoUsbControllerServiceGetStatus(&State)==EFI_SUCCESS && State.BulkActive && State.WorkPending);
  assert(mPending[3] && !mPending[2] && mFrameCount==1 && !memcmp(mBulkTx.Cpu,"INFO",4));
  // A paused host must block further storage I/O until it receives the reply.
  assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS && !flash_prepares && !flash_writes);
  UINTN Progress=0,Okay=0,Failed=0,Calls=0;
  UINTN Budget=Test==FlashUsbAbortDiagnostics || Test==FlashUsbStop?250:1000;
  while(PianoFastbootFlashPending(&mFastboot) || mFrames || mPending[3] || mService.Count){
    assert(++Calls<100000 && mFrameCount<=1 && !mPending[2]);
    UINTN Before=flash_writes;
    if(mPending[3]){
      if(!memcmp(mBulkTx.Cpu,"INFO",4)){
        if(Test==FlashUsbProgressCancelled && flash_writes){
          assert(flash_writes==FLASH_PROGRESS_BYTES/sizeof(flash_last_block));
          fake_now=mService.ReplyPostedUs+FASTBOOT_REPLY_IDLE_US;
          assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS);
          assert(flash_writes==Before && flash_flushes==1 && !mFastboot.Flash && !mFrames);
          assert(mEnding[3] && mBulkTx.Active && !mPending[2]);
          service_end(3);
          service_out("getvar:version");
          assert(mFrameCount==1 && mPosted[3]==7 && !memcmp(mBulkTx.Cpu,"OKAY0.4",7));service_ack();
          break;
        }
        ++Progress;
        if(Large)fake_now+=FASTBOOT_REPLY_IDLE_US/2;
      }
      else if(!memcmp(mBulkTx.Cpu,"OKAY",4)){assert(flash_flushes==1);++Okay;}
      else {assert(!memcmp(mBulkTx.Cpu,"FAIL",4));++Failed;}
      DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;
      publish(0xC046);timer_tick();
    }
    EFI_STATUS Status=PianoUsbControllerServicePumpApp(1,Budget);
    if(Test==FlashUsbExitDuringWrite){
      assert(Status==EFI_ABORTED && mService.State.ServicesLost && mFastboot.BootTransferFrozen);
      assert(flash_writes==1 && !flash_reads && !flash_flushes && !mFrames && !mPending[3]);
      free(mFastboot.Flash);free_cold_model();return;
    }
    assert(Status==EFI_SUCCESS && flash_writes-Before<SERVICE_EVENTS);
    if((Test==FlashUsbAbortDiagnostics || Test==FlashUsbStop) && flash_writes && PianoFastbootFlashPending(&mFastboot)){
      if(Test==FlashUsbStop)break;
      assert(PianoUsbControllerRestrictToDiagnostics(TRUE)==EFI_SUCCESS);
      assert(!mFastboot.Flash && mFastboot.DiagnosticsOnly && flash_flushes==1);
    }
  }
  assert(flash_prepares==1 && flash_reads==flash_writes);
  if(Test==FlashUsbDisconnect || Test==FlashUsbBusReset || Test==FlashUsbDisconnectOnFlush){
    assert(flash_writes==(Test==FlashUsbDisconnectOnFlush?flash_size/sizeof(flash_last_block):1));
    assert(!Okay && !Failed && !mFastboot.Flash && !mFastboot.Download && !mFrames && !mPending[3]);
    assert(flash_flushes==1 && !mBulkLive);
    service_enumerate();service_out("getvar:version");
    assert(!memcmp(mBulkTx.Cpu,"OKAY0.4",7));service_ack();
  }else if(Test==FlashUsbMediaChanged)assert(flash_flushes==1 && !Okay && Failed==1);
  else if(Test==FlashUsbAbortDiagnostics)assert(flash_flushes==1 && !Okay && Failed==1);
  else if(Test==FlashUsbStop)assert(!flash_flushes && !Okay && !Failed);
  else if(Test==FlashUsbProgressCancelled)assert(flash_flushes==1 && !Okay && !Failed && flash_bytes_written<flash_size);
  else {
    assert(flash_writes==flash_size/sizeof(flash_last_block) && flash_bytes_written==flash_size && flash_flushes==1 && Okay==1 && !Failed);
    assert(Progress>FB_QUEUE_FRAMES);
  }
  PIANO_USB_SERVICE_RETIRE_REPORT Retire;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retire)==EFI_SUCCESS && Retire.Clean);
  assert(!mFastboot.Flash && !mFastboot.Download && flash_flushes==1);
  free_cold_model();
}
typedef enum {
  TouchGpiVerified,TouchGpiUnregistered,TouchGpiWrongRoot,TouchGpiUnknownRow,
  TouchGpiChangedUfs,TouchGpiChangedUsb,TouchGpiGlobalFault,TouchGpiCaseCount
} TOUCH_GPI_CASE;
static VOID run_touch_coexistence(TOUCH_GPI_CASE Test){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;
  bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  hardware.Groups=5;hardware.Id0=5;
  hardware.Banks=3;hardware.Id1=0x20000003;
  hardware.Device[2].Sid=0xB6;hardware.Device[3].Sid=0xA3;
  load_hardware();
  PIANO_FB_STORAGE Storage={
    .Context=(VOID *)456,.Ready=ready_backend,.Info=info_backend,.ReadBlocks=read_backend,
    .PrepareWrite=flash_prepare,.WriteBlocks=flash_write,.Flush=flash_flush
  };
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Storage};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);
  service_enumerate();
  PIANO_SMMU_SNAPSHOT Baseline=mCoexistBasis;
  // Touch attaches two streams to bank 2 after USB has enumerated.
  gpi_routes[0]=bank(2,0xB6,0x83000000);
  gpi_routes[1]=gpi_routes[0];
  gpi_routes[1].Sid=0xA3;gpi_routes[1].StreamIndex=3;gpi_routes[1].Smr=0x800000A3;
  for(UINTN I=0;I<ARRAY_SIZE(gpi_routes);++I){
    CONST PIANO_SMMU_DEVICE *Route=&gpi_routes[I];
    hardware.Device[I+2]=*Route;
    hardware.RawSmr[Route->StreamIndex]=Route->Smr;
    hardware.RawS2cr[Route->StreamIndex]=Route->S2cr;
  }
  gpi_registered=Test!=TouchGpiUnregistered;
  if(Test==TouchGpiWrongRoot)hardware.Device[2].Ttbr0=hardware.Device[3].Ttbr0=0x83001000;
  if(Test==TouchGpiUnknownRow)hardware.RawSmr[4]=0x800000AF;
  if(Test==TouchGpiChangedUfs)hardware.Device[0].Sctlr^=1;
  if(Test==TouchGpiChangedUsb)hardware.Device[1].Tcr^=1;
  if(Test==TouchGpiGlobalFault)hardware.GlobalFault=1;
  load_hardware();
  service_out("getvar:product");
  assert(status_has_frame("OKAYpiano-sunuefi"));
  service_ack();
  if(Test!=TouchGpiVerified){
    CONST CHAR8 *Query="getvar:storage-policy";
    UINTN Bytes=strlen(Query);
    memcpy(mBulkRx.Cpu,Query,Bytes);
    DWC_TRB *Trb=mTrbs[2].Cpu;Trb->Size=mPosted[2]-Bytes;Trb->Control&=~BIT0;
    publish(0xC044);timer_tick();
    assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_DEVICE_ERROR);
    assert(mService.State.Action==PianoUsbServiceActionFault && !mPending[3]);
    assert(mCoexistFailed && mExperimentContractFailed && mUsbCleanupBlocked && !backend_reads);
    assert(!memcmp(&Baseline,&mCoexistBasis,sizeof(Baseline)));
    free_cold_model();return;
  }
  service_out("getvar:storage-policy");
  assert(status_has_frame("OKAYexplicit-partition-writes"));
  service_ack();
  service_out("oem help");
  assert(status_has_frame("INFOflash PARTITION FILE - write raw or sparse image"));
  service_ack();
  service_out("fetch:xbl_config_a:0x0:0x1");
  assert(status_has_frame("OKAY") && backend_reads==1);
  service_ack();
#if PIANO_USB_HOST
  typec=(PIANO_USB_TYPEC_STATE){.Attached=TRUE,.PartnerSink=TRUE,.Orientation=1};
  typec_poll();typec_poll();
  assert(host_active && host_starts==1 && mRole.Parked && !mService.State.Started && !closes);
  typec.Attached=FALSE;
  typec_poll();typec_poll();
  assert(!host_active && host_stops==1 && !mRole.Parked && mService.State.Started && !closes);
  service_enumerate();
  service_out("getvar:storage-policy");assert(status_has_frame("OKAYexplicit-partition-writes"));service_ack();
#endif
  assert(!memcmp(&Baseline,&mCoexistBasis,sizeof(Baseline)));
  // The regular OS handoff retires touch first, restoring both invalid rows.
  hardware=Baseline;gpi_registered=FALSE;load_hardware();
  PIANO_USB_SERVICE_RETIRE_REPORT Retire;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retire)==EFI_SUCCESS && Retire.Clean);
  free_cold_model();
}
#if PIANO_USB_HOST
typedef enum {
  TypecDeviceFlash,TypecHostBusy,TypecPhyRestored,TypecPhyRetained,TypecDeviceRetained,TypecCaseCount
} TYPEC_CASE;
static VOID run_typec_orientation(TYPEC_CASE Test){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;
  bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  flash_case=FlashUsbErase;flash_size=2*sizeof(flash_last_block);
  PIANO_FB_STORAGE Storage={
    .Context=(VOID *)456,.Ready=ready_backend,.Info=flash_info,.ReadBlocks=flash_read,
    .PrepareWrite=flash_prepare,.WriteBlocks=flash_write,.Flush=flash_flush
  };
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Storage};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);
  service_enumerate();
  typec=(PIANO_USB_TYPEC_STATE){.Attached=TRUE,.PartnerSink=Test==TypecHostBusy,.Orientation=1};
  typec_poll();typec_poll();
  assert(!phy_switches);
  if(Test==TypecHostBusy){
    assert(host_active && host_starts==1 && !host_stop_calls);
    typec_status=EFI_TIMEOUT;
    typec_poll();typec_poll();
    assert(host_active && !host_stop_calls);
    typec_status=EFI_SUCCESS;typec.Orientation=2;
    typec_poll();assert(!host_stop_calls);
    host_stop_status=EFI_NOT_READY;
    typec_poll();assert(host_active && !phy_switches && host_stop_calls==1);
    host_stop_status=EFI_SUCCESS;
    typec_poll();assert(host_active && host_starts==2 && host_stops==1 && phy_switches==1 && phy_orientation==2);
    typec_poll();assert(host_starts==2 && phy_switches==1);
    typec.PartnerSink=FALSE;typec.Orientation=1;
    typec_poll();typec_poll();
    assert(!host_active && host_stops==2 && phy_switches==2 && phy_orientation==1);
  }else {
    if(Test==TypecDeviceFlash)service_out("erase:sunuefi_esp");
    typec.Orientation=2;typec_poll();
    // A stale CC bit on a failed read must not complete the debounce.
    typec_status=EFI_TIMEOUT;typec_poll();
    typec_status=EFI_SUCCESS;typec_poll();
    assert(!phy_switches);
    if(Test==TypecDeviceFlash){
      typec_poll();
      assert(!phy_switches && mService.State.Started && PianoFastbootFlashPending(&mFastboot));
      assert(mPending[3] && !flash_writes);
      for(UINTN I=0;I<20 && (PianoFastbootFlashPending(&mFastboot) || mPending[3]);++I){
        if(mPending[3])service_ack();
        else assert(PianoUsbControllerServicePumpApp(1,5000)==EFI_SUCCESS);
      }
      assert(flash_writes==2 && flash_reads==2 && flash_flushes==1 && !phy_switches && !mPending[3]);
    }else if(Test==TypecDeviceRetained)failed_halt=TRUE;
    else{phy_switch_status=EFI_TIMEOUT;phy_lost=Test==TypecPhyRetained;}
    typec_poll();
    if(Test==TypecDeviceRetained || Test==TypecPhyRetained){
      assert(mRole.Failed && mUsbCleanupBlocked && !closes && !removes && !host_active);
      if(Test==TypecDeviceRetained)assert(!phy_switches && mService.State.Retained && mRing.Active);
      else assert(phy_switches==1 && !phy_orientation && !mService.State.Started);
      fake_now+=250000;
      EFI_STATUS Pump=PianoUsbControllerServicePumpApp(PIANO_PRODUCT_PUMP_GUI,5000);
      assert(Pump==(Test==TypecDeviceRetained?EFI_TIMEOUT:EFI_SUCCESS));
      assert(phy_switches==(Test==TypecPhyRetained?1:0));
      free_cold_model();return;
    }
    if(Test==TypecPhyRestored){
      assert(phy_switches==1 && phy_orientation==1 && mService.State.Started && !mRole.Failed);
      typec_poll();typec_poll();assert(phy_switches==1);
      // Retry after a new attachment, not while the failed connection remains.
      typec.Attached=FALSE;typec_poll();typec_poll();
      typec.Attached=TRUE;phy_switch_status=EFI_SUCCESS;
      typec_poll();typec_poll();assert(phy_switches==2);
    }else assert(phy_switches==1);
    assert(phy_orientation==2);
  }
  assert(!closes && !mRole.Parked && mService.State.Started && !mConfigured);
  service_enumerate();
  service_out("getvar:version");assert(status_has_frame("OKAY0.4"));service_ack();
  PIANO_USB_SERVICE_RETIRE_REPORT Retire;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retire)==EFI_SUCCESS && Retire.Clean);
  free_cold_model();
}
#endif
static VOID run_stale_completion(VOID){
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID *)0x55,.NowUs=now_us};
  assert(PianoUsbControllerServiceStart((VOID *)123,&C)==EFI_SUCCESS);
  UINTN Completed=completions;
  for(UINTN I=0;I<3;++I){
    service_event(0x101);
    assert(completions==Completed&&mPending[0]&&mTrbs[0].Active&&mSetup.Active&&mPhase==0);
  }
  service_event(0xC040); // Observed stale SETUP completion: new TRB still HWO.
  assert(completions==Completed && mPending[0] && mTrbs[0].Active && mSetup.Active && mPhase==0);
  service_enumerate();Completed=completions;
  service_event(0xC044); // Same ownership rule for a reset-era bulk event.
  assert(completions==Completed && mPending[2] && mTrbs[2].Active && mBulkRx.Active);
  assert(mService.State.Phase==PianoUsbServiceListening && !mService.State.Retained);
  service_out("getvar:version");service_ack();
  PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS && R.Clean);
  free_cold_model();
}
static UINTN reset_elapsed,reset_sync,reset_case;
static VOID reset_tick(UINTN Us){
  if(Us==50000){reset_sync+=Us;return;}
  if(Us!=20000 || !(regs[0xC704/4]&BIT30))return;
  assert(!(regs[0xC200/4]&(BIT6|BIT8)) && !(regs[0xC2C0/4]&BIT17));
  reset_elapsed+=Us;
  if(reset_case<=2 && reset_elapsed>=150000)regs[0xC704/4]&=~BIT30;
  if(reset_case==4 && reset_elapsed==200000){ran=TRUE;failed_halt=TRUE;}
  if(reset_case==5 && reset_elapsed==200000)regs[0xC708/4]=1;
  if(reset_case==6)mService.State.ServicesLost=TRUE;
  if(reset_case==8 && reset_elapsed==200000){mLogSnapshot=calloc(1,USB_DIAG_LOG_BYTES);assert(mLogSnapshot);pool_free_warning=TRUE;}
  if(reset_elapsed==200000){
    if(reset_case==10)regs[0xC408/4]=4096;
    if(reset_case==11)regs[0xC40C/4]=4;
    if(reset_case==12)regs[0xC110/4]=1U<<12;
    if(reset_case==14){mRing.Cpu=malloc(16);assert(mRing.Cpu);}
  }
}
static VOID run_reset(UINTN Test){
  setup_model();stall_hook=reset_tick;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  reset_case=Test;reset_elapsed=reset_sync=0;hold_device_reset=TRUE;
  regs[0xC110/4]=0x00102004;
  regs[0xC1A0/4]=Test==0?0x3139302a:Test==1?0x3138302a:0;
  regs[0xC1A4/4]=0x6574;regs[0xC200/4]=0x00102500;regs[0xC2C0/4]=0x030c1002;
  if(Test==7)clock_warning=TRUE;
  if(Test==9)ignore_reset_register_writes=TRUE;
  if(Test==13)regs[0xC408/4]=4096;
  PIANO_USB_STARTUP_FAILURE_EVIDENCE E;
  assert(PianoDwc3GetStartupFailureEvidence(NULL)==EFI_INVALID_PARAMETER);
  assert(PianoDwc3GetStartupFailureEvidence(&E)==EFI_NOT_READY);
  PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&backend};
  EFI_STATUS S=PianoUsbControllerServiceStart((VOID *)123,&C);
  if(Test<=2){
    assert(S==EFI_SUCCESS && mService.State.Started && allocations==10 && reset_elapsed==160000);
    assert(reset_sync==(Test==0?0:50000) && regs[0xC200/4]==0x00102400 && regs[0xC2C0/4]==0x030c1002);
    assert(PianoDwc3GetStartupFailureEvidence(&E)==EFI_NOT_READY);
    PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS && R.Clean && R.DmaBuffersFreed==9);
    assert(regs[0xC200/4]==0x00102500 && regs[0xC2C0/4]==0x030c1002);
  }else if(Test==3||Test==9){
    assert(S==EFI_TIMEOUT && reset_elapsed==200000 && !reset_sync && allocations==1 && !mPersistent.Started && !mService.DmaBuffersAllocated);
    assert(PianoDwc3GetStartupFailureEvidence(&E)==EFI_SUCCESS && E.CsftrstTimeoutPreDma && E.ServiceNeverStarted && E.DeviceHalted && E.DmaFreed);
    assert(E.StartStatus==EFI_TIMEOUT && E.CleanupStatus==EFI_SUCCESS && !E.DmaBuffersAllocated && !E.DmaBuffersFreed && !E.Retained && !E.ServicesLost);
    assert(closes==1 && !held && !domain && !mUsbCleanupBlocked && !mExperimentRunning && !event_creates);
    assert(mService.State.Action==PianoUsbServiceActionFault && mService.State.LastStatus==EFI_TIMEOUT);
    assert(!address_writes && regs[0xC408/4]==0); // No ring/config restore when none was installed.
    PIANO_SMMU_USB_RETIRE_EVIDENCE Running;assert(PianoDwc3GetRetireEvidence(&Running)==EFI_NOT_READY);
    PIANO_SMMU_RETIRED_USB_PROOF P,Bad;
    assert(PianoUsbControllerMakeRetiredUsbProof((VOID *)123,&P)==EFI_NOT_READY);
    assert(PianoUsbControllerGetStartupFailureProof((VOID *)123,&P)==EFI_NOT_READY);
    proof_validator_accepts=TRUE;
    assert(PianoUsbControllerGetStartupFailureProof((VOID *)123,&P)==EFI_SUCCESS && P.Valid && P.Execution.Kind==PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN);
    assert(PianoUsbControllerValidateStartupFailureProof((VOID *)123,&P)==EFI_SUCCESS);
    Bad=P;Bad.Execution.DeviceHalted=FALSE;assert(PianoUsbControllerValidateStartupFailureProof((VOID *)123,&Bad)==EFI_NOT_READY);
    mService.State.ServicesLost=TRUE;assert(PianoDwc3GetStartupFailureEvidence(&E)==EFI_NOT_READY && PianoUsbControllerValidateStartupFailureProof((VOID *)123,&P)!=EFI_SUCCESS);
    mService.State.ServicesLost=FALSE;mService.CleanupCompleted=FALSE;assert(PianoDwc3GetStartupFailureEvidence(&E)==EFI_NOT_READY);
  }else{
    assert(S!=EFI_SUCCESS && allocations==1 && !mService.DmaBuffersAllocated);
    if(Test==4)assert(mService.CleanupStatus==EFI_TIMEOUT && mService.State.Retained && mUsbCleanupBlocked && !closes);
    if(Test==5)assert(mService.CleanupStatus==EFI_DEVICE_ERROR && mService.Uncertain && mUsbCleanupBlocked && !closes);
    if(Test==6)assert(mService.State.ServicesLost && !mService.CsftrstTimeoutPreDma && mUsbCleanupBlocked);
    if(Test==7)assert(mService.CleanupStatus==EFI_SUCCESS && mUsbCleanupBlocked && held==8 && domain);
    if(Test==8){assert(mService.CleanupStatus==EFI_DEVICE_ERROR && mService.Uncertain && mLogSnapshot && mUsbCleanupBlocked);free(mLogSnapshot);mLogSnapshot=NULL;}
    if(Test>=10)assert(mService.CleanupStatus==EFI_DEVICE_ERROR && mService.Uncertain && mUsbCleanupBlocked && !closes);
    PIANO_SMMU_RETIRED_USB_PROOF P;assert(PianoUsbControllerGetStartupFailureProof((VOID *)123,&P)!=EFI_SUCCESS);
    if(Test!=7)assert(PianoDwc3GetStartupFailureEvidence(&E)!=EFI_SUCCESS);
  }
  free_cold_model();
}
static VOID run_service(UINTN Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;bs_calls=event_creates=event_closes=clock_calls=proof_calls=0;
  timer_notify=exit_notify=NULL;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_FB_STORAGE Copied=backend;PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&Copied};
  if(Test>=17&&Test<=19){boot_fixture();PIANO_FB_BOOT Boot={.Context=(VOID *)0x99,.MaxImageBytes=65536,.Ready=persistent_boot_ready,.Validate=persistent_boot_validate,.TakeAfterAck=persistent_boot_take};assert(PianoDwc3SetBootForExperiment(&Boot)==EFI_SUCCESS);}
  if(Test==9)event_failure=TRUE;if(Test==10)event_warning=TRUE;
  EFI_STATUS S=PianoUsbControllerServiceStart((VOID *)123,&Config);
  if(Test==9){assert(S==EFI_DEVICE_ERROR && !held && !domain && !mPersistent.Started && closes==1 && !mUsbShutdownLive);free_cold_model();return;}
  if(Test==10){assert(S==EFI_DEVICE_ERROR && mUsbCleanupBlocked && held==PIANO_USB_CLOCK_RELEASE_MASK && domain && !closes);free_cold_model();return;}
  assert(S==EFI_SUCCESS && allocations==10 && held==PIANO_USB_CLOCK_RELEASE_MASK && domain && mUsbShutdownLive && closes==0 && mService.State.Started && event_creates==2);
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
    PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_ACCESS_DENIED && !closes && held==PIANO_USB_CLOCK_RELEASE_MASK);free_cold_model();return;
  } else {
    service_enumerate();
    if(Test==0){
      for(UINTN Page=0;Page<3;++Page){fake_now+=9000000000ULL;assert(PianoUsbControllerServicePumpApp(1U<<Page,5000)==EFI_SUCCESS);service_out("getvar:version");assert(mPending[3] && !memcmp(mBulkTx.Cpu,"OKAY0.4",7));service_ack();assert(!closes && held==PIANO_USB_CLOCK_RELEASE_MASK && mService.State.Phase==PianoUsbServiceListening);}
      service_out("fetch:xbl_config_a:0x0:0x1");service_ack();assert(backend_reads==1 && mCoexistQuietChecks>=6);
      service_out("reboot");assert(mService.State.Phase==PianoUsbServiceListening);service_ack();assert(mService.State.Action==PianoUsbServiceActionReboot && mService.State.Phase==PianoUsbServiceStopRequested && !resets && !closes);
    }
    if(Test>=17&&Test<=19){service_download_fixture();service_out("boot");assert(mFastboot.BootPending && !mBootAckObserved && !boot_takes && mService.State.Phase==PianoUsbServiceListening);
      if(Test!=18){service_ack();assert(mBootAckObserved && mService.State.Action==PianoUsbServiceActionBoot && mService.State.Phase==PianoUsbServiceStopRequested && !boot_takes && mFastboot.Download);}
      if(Test==19)boot_take_fail=TRUE;
    }
    if(Test==11){scenario=1;service_out("fetch:xbl_config_a:0x0:0x1");assert(mCoexistFailed && mUsbCleanupBlocked);}
    if(Test==12){((DWC_TRB *)mTrbs[0].Cpu)->Control&=~BIT0;service_event(1);assert(!mConfigured && mEnding[2]);((DWC_TRB *)mTrbs[2].Cpu)->Control&=~BIT0;service_event((8U<<24)|(7U<<6)|4);service_event(0x201);service_enumerate();service_out("getvar:product");service_ack();assert(mService.State.Phase==PianoUsbServiceListening);}
  }
  if(Test==5)failed_halt=TRUE;if(Test==6)dma_free_status=EFI_WARN_STALE_DATA;if(Test==7)dma_complete_status=EFI_WARN_STALE_DATA;
  if(Test==20)corrupt_gctl_restore=TRUE;
  if(Test==21){
    halt_required_ends=0;
    for(UINTN Ep=0;Ep<ARRAY_SIZE(mPending);++Ep)if(mPending[Ep]&&!mEnding[Ep])halt_required_ends|=1U<<Ep;
    assert(halt_required_ends);
    halt_requires_empty_ring=TRUE;regs[0xC40C/4]=8;regs[0xC200/4]|=BIT6|BIT8;
  }
  if(Test==13)timer_cancel_warning=TRUE;if(Test==14)event_close_warning=TRUE;if(Test==15)clock_warning=TRUE;if(Test==16)domain_warning=TRUE;
  PIANO_USB_SERVICE_RETIRE_REPORT R;S=PianoUsbControllerServiceStop(EFI_SUCCESS,&R);
  if(Test==3 || Test==4 || Test==5 || Test==6 || Test==7 || Test==11 || Test==13 || Test==14 || Test==15 || Test==16 || Test==19 || Test==20) {
    assert(S!=EFI_SUCCESS && R.Retained && !R.Clean && mUsbCleanupBlocked && domain);
    if(Test<=7 || Test==11 || Test==13 || Test==14 || Test==19)assert(!closes && held==PIANO_USB_CLOCK_RELEASE_MASK);
    if(Test==15)assert(R.ClockReleaseMask==(PIANO_USB_CLOCK_RELEASE_MASK&~BIT3) && held==8 && !R.ClocksReleased);
    if(Test==16)assert(R.ClockReleaseMask==PIANO_USB_CLOCK_RELEASE_MASK && !held && !R.ClocksReleased);
  } else {
    assert(S==EFI_SUCCESS && R.Clean && !R.Retained && R.DeviceHalted && R.DmaFreed && R.DmaBuffersFreed==9 && R.DomainFreed && R.ClocksReleased && R.ClockReleaseMask==PIANO_USB_CLOCK_RELEASE_MASK);
    if(Test==21)assert(!halt_required_ends&&!regs[0xC40C/4]);
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
  if(Test>=17&&Test<=19){PIANO_FB_BOOT_ACTION A;EFI_STATUS Taken=PianoDwc3ConsumeBootAction(&A);
    if(Test==18){assert(Taken==EFI_NOT_FOUND && !boot_takes && !mFastboot.Download);}
    else if(Test==17){assert(Taken==EFI_SUCCESS && A.Taken && !A.Retained && A.Token==(VOID *)0x8877 && taken_payload && boot_takes==1);assert(PianoDwc3ConsumeBootAction(&A)==EFI_NOT_FOUND);}
    else assert(Taken==EFI_DEVICE_ERROR && A.Retained && !A.Taken && boot_takes==1 && mFastboot.Download);
  }
  free_cold_model();
}
static VOID diagnostics_exit(VOID) {
  restore_tpl_hook=NULL;
  exit_notify((VOID *)0x102,NULL);
}
static VOID run_diagnostics(UINTN Test) {
  setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;
  clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
  PIANO_DWC3_SERVICE_CONFIG Config={.Context=(VOID *)0x55,.NowUs=now_us,.Storage=&backend};
  assert(PianoUsbControllerServiceStart((VOID *)123,&Config)==EFI_SUCCESS);
  assert(PianoUsbControllerPumpDiagnostics(5000)==EFI_NOT_READY);
  UINT8 Bounce[4096];UINTN Queued=0;VOID *Payload=NULL;
  if(Test==25){
    service_enumerate();service_out("getvar:version");
    assert(mPending[3] && mBulkTx.Active && mTrbs[3].Active && mFrames);
    CopyMem(Bounce,mBulkTx.Cpu,sizeof(Bounce));Queued=mFrameCount;Payload=mPayload[3];
  }
  if(Test==26){
    service_enumerate();((DWC_TRB *)mTrbs[0].Cpu)->Control&=~BIT0;service_event(1);
    assert(mEnding[2] && mPending[2] && mTrbs[2].Active);
  }
  if(Test==27){
    service_enumerate();service_out("reboot");service_ack();
    assert(mService.State.Phase==PianoUsbServiceStopRequested);
  }
  EFI_BOOT_SERVICES OtherServices=bs;EFI_SYSTEM_TABLE OtherTable=service_system_table;
  switch(Test) {
    case 1:mService.State.Busy=TRUE;break;
    case 2:mService.Uncertain=TRUE;break;
    case 3:mExperimentContractFailed=TRUE;break;
    case 4:mPersistent.Unknown=TRUE;break;
    case 5:mUsbCleanupBlocked=TRUE;break;
    case 6:mPersistent.ServicesLost=TRUE;break;
    case 7:mService.State.Retained=TRUE;break;
    case 8:mService.State.Started=FALSE;break;
    case 9:mService.State.Phase=PianoUsbServiceStopRequested;break;
    case 10:mService.State.Action=PianoUsbServiceActionReboot;break;
    case 11:mFastboot.RebootRequested=TRUE;break;
    case 12:mFastboot.ExitRequested=TRUE;break;
    case 13:mFastboot.Receiving=TRUE;break;
    case 14:mFastboot.BootPreparing=TRUE;break;
    case 15:mFastboot.BootPending=TRUE;break;
    case 16:mFastboot.BootTransferFrozen=TRUE;break;
    case 17:mBootAckObserved=TRUE;break;
    case 18:mBootActionValid=TRUE;break;
    case 19:service_system_table.BootServices=NULL;break;
    case 20:gBS=&OtherServices;break;
    case 21:gST=&OtherTable;break;
    case 22:current_tpl=TPL_CALLBACK;break;
    case 23:restore_tpl_hook=diagnostics_exit;break;
    case 24:mService.State.ServicesLost=TRUE;break;
  }
  UINTN Captures=full_captures,Reads=backend_reads,Infos=backend_infos,Clocks=clock_calls,Events=event_creates;
  EFI_STATUS Status=PianoUsbControllerRestrictToDiagnostics(Test!=27);
  assert(full_captures==Captures && backend_reads==Reads && backend_infos==Infos &&
    clock_calls==Clocks && event_creates==Events && !closes);
  if(Test && Test!=25 && Test!=26 && Test!=27) {
    assert(Status!=EFI_SUCCESS && !mPersistent.DiagnosticsOnly && !mService.DiagnosticsOnly && !mFastboot.DiagnosticsOnly);
    free_cold_model();return;
  }
  assert(Status==EFI_SUCCESS && mPersistent.DiagnosticsOnly && mService.DiagnosticsOnly && mFastboot.DiagnosticsOnly);
  if(Test==27){
    assert(mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);
    assert(mPending[2]&&!mFastboot.RebootRequested);
    service_out("getvar:product");assert(!memcmp(mBulkTx.Cpu,"OKAYpiano-sunuefi",16));service_ack();
    service_out("reboot");assert(!memcmp(mBulkTx.Cpu,"FAIL",4));service_ack();
    service_out("getvar:version");assert(!memcmp(mBulkTx.Cpu,"OKAY0.4",7));service_ack();
    assert(mService.State.Phase==PianoUsbServiceListening);
    PIANO_USB_SERVICE_RETIRE_REPORT Retired;
    assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retired)==EFI_SUCCESS&&Retired.Clean);
    free_cold_model();return;
  }
  if(Test==25){
    assert(mPending[3] && mBulkTx.Active && mTrbs[3].Active && mFrameCount==Queued && mPayload[3]==Payload);
    assert(!memcmp(Bounce,mBulkTx.Cpu,sizeof(Bounce)));service_ack();
  }
  if(Test==26){
    assert(mEnding[2] && mPending[2] && mTrbs[2].Active);
    ((DWC_TRB *)mTrbs[2].Cpu)->Control&=~BIT0;service_event((8U<<24)|(7U<<6)|4);service_event(0x201);
  }
  // Enter before a PC enumerates, as on a failed unattended boot.
  if(Test!=25)service_enumerate();assert(mFastboot.DiagnosticsOnly);
  CONST CHAR8 *Blocked[]={"boot","download:00000004","fetch:xbl_config_a:0x0:0x1","oem setup","oem shell","oem boot-stable"};
  for(UINTN I=0;I<ARRAY_SIZE(Blocked);++I){
    service_out(Blocked[I]);assert(mPending[3] && !memcmp(mBulkTx.Cpu,"FAIL",4));service_ack();
    assert(mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);
  }
  service_out("getvar:max-download-size");assert(mPosted[3]==14 && !memcmp(mBulkTx.Cpu,"OKAY0x00000000",14));service_ack();
  service_out("oem status");assert(status_has_frame("INFOstorage-policy:read-only"));service_ack();
  UINT32 *Header=calloc(1,0x200000);assert(Header);test_console=Header;
  CONST CHAR8 *Text="SUNUEFI_RAMLOG_BEGIN\nboot failure retained\n";Header[0]=0x43474244;Header[1]=Header[2]=(UINT32)strlen(Text);CopyMem(Header+3,Text,strlen(Text));
  service_out("oem ramlog");service_ack();assert(mFastboot.Upload && mFastboot.UploadBytes==strlen(Text) && !memcmp(mFastboot.Upload,Text,strlen(Text)));
  service_out("upload");service_ack();
  // Device reset and fresh SET_CONFIGURATION both call ClearFastboot.
  ((DWC_TRB *)mTrbs[0].Cpu)->Control&=~BIT0;service_event(1);
  ((DWC_TRB *)mTrbs[2].Cpu)->Control&=~BIT0;service_event((8U<<24)|(7U<<6)|4);service_event(0x201);service_enumerate();
  assert(mFastboot.DiagnosticsOnly && mService.DiagnosticsOnly && mPersistent.DiagnosticsOnly);
  service_out(Test==26?"continue":"reboot");assert(!memcmp(mBulkTx.Cpu,"OKAY",4));
  assert(mService.State.Phase==PianoUsbServiceListening && mService.State.Action==PianoUsbServiceActionNone);
  service_ack();
  assert(mService.State.Phase==PianoUsbServiceStopRequested &&
    mService.State.Action==(Test==26?PianoUsbServiceActionContinue:PianoUsbServiceActionReboot));
  assert(PianoUsbControllerPumpDiagnostics(5000)==EFI_SUCCESS);
  assert(full_captures==Captures && backend_reads==Reads && backend_infos==Infos && clock_calls==Clocks && event_creates==Events && !closes && !resets);
  service_system_table.BootServices=NULL;
  assert(PianoUsbControllerPumpDiagnostics(5000)!=EFI_SUCCESS);
  assert(PianoDwc3ServicePumpDiagnostics(5000)!=EFI_SUCCESS);
  service_system_table.BootServices=&bs;
  PIANO_USB_SERVICE_RETIRE_REPORT Retired;
  assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&Retired)==EFI_SUCCESS);
  assert(Retired.Clean && Retired.DeviceHalted && Retired.DmaFreed && Retired.DomainFreed &&
    Retired.ClocksReleased && Retired.DmaBuffersFreed==9 && !mPersistent.Started);
  free(Header);test_console=NULL;free_cold_model();
}
#endif
#ifndef USB_SERVICE_TEST_MAIN
#define USB_SERVICE_TEST_MAIN main
#endif
int USB_SERVICE_TEST_MAIN(void) {
#if PIANO_USB_SERVICE
  for(UINTN I=0;I<15;++I){pid_t P=fork();assert(P>=0);if(!P){run_reset(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S))return 1;}
  {pid_t P=fork();assert(P>=0);if(!P){run_idle_activity();_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S))return 1;}
  {pid_t P=fork();assert(P>=0);if(!P){run_stale_completion();_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S))return 1;}
  for(REPLY_CASE I=0;I<ReplyCaseCount;++I){
    pid_t P=fork();assert(P>=0);if(!P){run_reply_recovery(I);_exit(0);}
    int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"USB reply recovery case %u failed\n",(unsigned)I);return 1;}
  }
  for(FLASH_USB_CASE I=0;I<FlashUsbCaseCount;++I){
    pid_t P=fork();assert(P>=0);if(!P){run_partition_write(I);_exit(0);}
    int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"USB partition write case %u failed\n",(unsigned)I);return 1;}
  }
  for(TOUCH_GPI_CASE I=0;I<TouchGpiCaseCount;++I){
    pid_t P=fork();assert(P>=0);if(!P){run_touch_coexistence(I);_exit(0);}
    int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"USB touch coexistence case %u failed\n",(unsigned)I);return 1;}
  }
#if PIANO_USB_HOST
  for(TYPEC_CASE I=0;I<TypecCaseCount;++I){
    pid_t P=fork();assert(P>=0);if(!P){run_typec_orientation(I);_exit(0);}
    int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"USB Type-C orientation case %u failed\n",(unsigned)I);return 1;}
  }
#endif
  for(UINTN I=0;I<22;++I){pid_t P=fork();assert(P>=0);if(P==0){run_service(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB persistent service case %llu failed\n",(unsigned long long)I);return 1;}}
  for(UINTN I=0;I<11;++I){pid_t P=fork();assert(P>=0);if(P==0){run_navigation(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB product navigation case %llu failed\n",(unsigned long long)I);return 1;}}
  for(UINTN I=0;I<8;++I){pid_t P=fork();assert(P>=0);if(P==0){run_ramlog(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB before-ramlog case %llu failed\n",(unsigned long long)I);return 1;}}
  for(STATUS_CASE I=0;I<StatusCaseCount;++I){pid_t P=fork();assert(P>=0);if(P==0){run_status(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB status-report case %u failed\n",(unsigned)I);return 1;}}
  for(UINTN I=0;I<28;++I){pid_t P=fork();assert(P>=0);if(P==0){run_diagnostics(I);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status) || WEXITSTATUS(Status)){fprintf(stderr,"USB diagnostics case %u failed\n",(unsigned)I);return 1;}}
  puts("Actual Device+Controller persistent USB: 15 reset/startup-proof + 22 lifecycle + 11 navigation + 8 before-ramlog + 11 status-report + 28 failure-diagnostics cases PASS; in-flight response/END preservation, diagnostic enumeration/reset, saved log upload and fail-closed state checks; no device.");
  puts("Fastboot APP pump: abandoned fetch and reboot replies, END fencing, fresh commands and long transfers passed; partial download requires bus reset.");
  puts("Fastboot APP pump: 1 GiB virtual erase and sparse FILL above the 48-frame queue limit, cancelled progress reply, reset/disconnect, exit fence, media change and stop passed.");
  puts("Fastboot and OTG: two verified touch streams survive device/host transitions; changed or unregistered mappings still quarantine.");
#else
  PIANO_USB_STARTUP_FAILURE_EVIDENCE Evidence;assert(PianoDwc3GetStartupFailureEvidence(&Evidence)==EFI_UNSUPPORTED && Evidence.Revision==1);
  (VOID)backend;PIANO_DWC3_SERVICE_STATUS S;PIANO_USB_SERVICE_RETIRE_REPORT R;PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID *)0x55,.NowUs=NULL};
  assert(PianoUsbControllerServiceStart(NULL,&C)==EFI_UNSUPPORTED && PianoDwc3ServiceStart(NULL,NULL,&C)==EFI_UNSUPPORTED);
  assert(PianoUsbControllerServicePumpApp(1,1000)==EFI_UNSUPPORTED && PianoDwc3ServicePollBounded(1)==EFI_UNSUPPORTED);
  assert(PianoUsbControllerRestrictToDiagnostics(TRUE)==EFI_UNSUPPORTED && PianoUsbControllerPumpDiagnostics(1000)==EFI_UNSUPPORTED);
  assert(PianoDwc3ServiceRestrictToDiagnostics(TRUE)==EFI_UNSUPPORTED && PianoDwc3ServicePumpDiagnostics(1000)==EFI_UNSUPPORTED);
  assert(PianoUsbControllerServiceGetStatus(&S)==EFI_UNSUPPORTED && PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_UNSUPPORTED);
  model_init();configure();cmd("oem setup","FAILcommand disabled by RAM-only policy");cmd("oem shell","FAILcommand disabled by RAM-only policy");cmd("oem simpleinit","FAILcommand disabled by RAM-only policy");
  assert(Halt()==EFI_SUCCESS);for(UINTN I=0;I<ARRAY_SIZE(all);++I){if(all[I]->Active)assert(PianoDmaComplete(all[I],EFI_SUCCESS,TRUE)==EFI_SUCCESS);assert(PianoDmaFree(all[I])==EFI_SUCCESS);}ClearFastboot();
  puts("USB persistent service default-off APIs and rejected OEM navigation PASS; no device.");
#endif
  return 0;
}
