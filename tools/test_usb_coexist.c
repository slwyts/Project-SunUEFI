// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Device + Controller + protocol; fake hardware/backend, never devices.
#define PIANO_USB_UFS_FETCH 1
#define PIANO_USB_EP0 1
#define USB_FASTBOOT_TEST_MAIN bulk_source_suite
#include "test_usb_fastboot.c"
#include "../bootprofiles/uefi-app/PianoUsbController.c"
#include <stdarg.h>
// This existing fixture mocks HAL ownership rather than a real close ledger.
// The new exporter must not infer retirement from those mock-only contexts.
EFI_STATUS PianoOwnedSmmuMakeRetiredUsbProof(CONST PIANO_OWNED_SMMU *C,CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *E,
  CONST PIANO_SMMU_SNAPSHOT *S,PIANO_SMMU_RETIRED_USB_PROOF *P){(VOID)C;(VOID)E;(VOID)S;(VOID)P;return EFI_NOT_READY;}
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
  C->After=hardware;C->Verified=C->Attached=TRUE;C->TableMemory.Signature=1;C->TableMemory.Physical=0x82000000;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C) {
  ++closes;if(C->TableMemory.Quarantined)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(!all[I]->Signature);
  ZeroMem(C,sizeof(*C));ZeroMem(&hardware.Device[1],sizeof(hardware.Device[1]));hardware.Device[1].Sid=0x40;
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
int main(void) {
  BOOLEAN Reboot;
  for(scenario=0;scenario<=8;++scenario) {
    setup_model();if(scenario==4)stall_hook=NULL;if(scenario==5)fail_setup=TRUE;if(scenario==6)fail_allocate=TRUE;
    EFI_STATUS S=PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot);
    if(scenario==0){assert(S==EFI_SUCCESS && Reboot && stage==9 && backend_reads==1 && !mUsbShutdownLive && !held && !domain && !resets && mCoexistQuietChecks>=6 && full_captures==6);}
    else if(scenario<=3 || scenario>=7) {
      assert(S!=EFI_SUCCESS && !Reboot && mCoexistFailed && mUsbCleanupBlocked && mUsbShutdownLive && held==255 && domain && mExperimentRunning);
      assert(PianoDwc3SetStorageForExperiment(NULL)==EFI_NOT_READY && PianoDwc3Ep0Experiment(&mUsbContext,&mUsbDevice)==EFI_ALREADY_STARTED);
      for(UINTN I=0;I<ARRAY_SIZE(all);++I)assert(all[I]->Quarantined && all[I]->Signature && !all[I]->Active);
      UINTN Count=allocations;assert(visible_shutdown && visible_shutdown->Revision==1 && visible_shutdown->Halt()==EFI_SUCCESS && allocations==Count && closes==1);
      if(scenario==2)assert(!backend_reads);
    } else {assert(S==(scenario==4?EFI_NOT_READY:scenario==5?EFI_DEVICE_ERROR:EFI_OUT_OF_RESOURCES) && !Reboot && !mExperimentRunning && !mUsbControllerRunning && !mUsbShutdownLive && !held && !domain);assert(PianoDwc3SetStorageForExperiment(NULL)==EFI_SUCCESS);}
  }
  setup_model();scenario=0;backend_fail=TRUE;assert(PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot)==EFI_NOT_READY && !installs);
  setup_model();install_fail=TRUE;assert(PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot)==EFI_DEVICE_ERROR && !mUsbShutdownLive && !held);
  setup_model();remove_fail=TRUE;assert(PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot)==EFI_DEVICE_ERROR && !Reboot && mUsbShutdownLive && held==255);
  setup_model();failed_halt=TRUE;
  if(!setjmp(failed_reset_return)){PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot);assert(FALSE);}
  assert(resets==1 && mExperimentRunning && mUsbShutdownLive && held==255 && !closes && mRing.Active);
  assert(visible_shutdown->Halt()==EFI_TIMEOUT && mRing.Active);
  setup_model();scenario=0;assert(PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot)==EFI_SUCCESS && !mExperimentHasStorage);
  assert(UsbHaltOnly()==EFI_NOT_READY); // Stale protocol cannot read powered-off registers.
  setup_model();stall_hook=NULL;reentry=TRUE;assert(PianoUsbControllerRunWithStorage((VOID *)123,&backend,&Reboot)==EFI_NOT_READY);
  assert(reentry_status==EFI_NOT_READY);
  setup_model();PIANO_FB_STORAGE Local=backend;assert(PianoDwc3SetStorageForExperiment(&Local)==EFI_SUCCESS);Local.Ready=NULL;assert(mExperimentStorage.Ready==backend.Ready);
  assert(PianoDwc3SetStorageCheckForExperiment(NULL,(VOID *)1)==EFI_INVALID_PARAMETER && PianoDwc3SetStorageForExperiment(NULL)==EFI_SUCCESS);
  setup_model();puts("Actual USB+storage sources: nine mapped before run, direct CB/read contract, corruption quarantine, deferred reboot, clean error retry/setter locks, pure-MMIO reset fence lifetime/timeout, no native reset and reentry passed.");return 0;
}
