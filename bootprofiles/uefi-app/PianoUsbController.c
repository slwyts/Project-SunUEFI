// SPDX-License-Identifier: BSD-2-Clause-Patent
// Retained DWC3 controller state and USB0 owned DMA context, isolated from UFS.
#include "PianoOwnedSmmu.h"
#include "PianoUsbService.h"
#include <Protocol/EFIClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#if defined(PIANO_USB_SERVICE) && PIANO_USB_SERVICE
#include <Guid/EventGroup.h>
#endif
#ifndef PIANO_USB_UFS_FETCH
#define PIANO_USB_UFS_FETCH 0
#endif
#define USB_BASE 0xA600000U
STATIC PIANO_OWNED_SMMU mUsbContext;
STATIC PIANO_DMA_DEVICE mUsbDevice;
STATIC BOOLEAN mUsbCleanupBlocked,mUsbControllerRunning;
STATIC PIANO_SMMU_USB_RETIRE_EVIDENCE mControllerRetire;
STATIC BOOLEAN mControllerRetireValid,mRetireProofConsumed;
STATIC VOID ControllerZero(VOID *Buffer,UINTN Bytes) {
  for(UINTN I=0;I<Bytes;++I)((volatile UINT8 *)Buffer)[I]=0;
}
#if PIANO_USB_SERVICE
STATIC struct {
  BOOLEAN Opening,Started,Unknown,ServicesLost;
  EFI_CLOCK_PROTOCOL *Clock;UINTN Ids[8],Held,Domain;BOOLEAN DomainHeld;
  EFI_EVENT Timer,ExitEvent;PIANO_DWC3_SERVICE_CONFIG Config;
  PIANO_DMA_BUFFER ProbeBuffer;
} mPersistent;
STATIC EFI_STATUS ServiceExact(EFI_STATUS S,BOOLEAN Mutation) {
  if(!mPersistent.Opening || S==EFI_SUCCESS)return S;
  if(Mutation){mPersistent.Unknown=TRUE;mUsbCleanupBlocked=TRUE;}
  return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
#endif
#include "PianoUsbRamBootExperiment.h"
#if PIANO_USB_RAM_BOOT
STATIC BOOLEAN mRamBootMode,mRamClockUnknown,mRamDomainReleased;
STATIC UINT32 mRamClockReleaseMask;
STATIC UINT16 mRamOwnedStreamIndex;
STATIC PIANO_DMA_BUFFER mRamProbeBuffer;
STATIC EFI_STATUS RamExact(EFI_STATUS Status) {
  if(mRamBootMode && Status!=EFI_SUCCESS && !EFI_ERROR(Status)) {
    mUsbCleanupBlocked=TRUE;mUsbContext.TableMemory.Quarantined=TRUE;return EFI_DEVICE_ERROR;
  }
  return Status;
}
STATIC EFI_STATUS RamClockStatus(EFI_STATUS Status,BOOLEAN Mutated) {
  if(!mRamBootMode || Status==EFI_SUCCESS)return Status;
  if(Mutated){mRamClockUnknown=TRUE;mUsbCleanupBlocked=TRUE;}
  return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}
#endif
#if PIANO_USB_UFS_FETCH
#include "PianoUsbStorageExperiment.h"
STATIC PIANO_SMMU_DEVICE mCoexistUfs;
STATIC PIANO_SMMU_SNAPSHOT mCoexistBasis;
STATIC BOOLEAN mCoexistReady,mCoexistFailed,mCombinedBusy,mProxyBusy,mUsbShutdownLive;
STATIC EFI_HANDLE mUsbShutdownHandle;
STATIC PIANO_FB_STORAGE mUnderlyingStorage;
STATIC UINT64 mCoexistQuietChecks;
STATIC BOOLEAN SameContext(CONST PIANO_SMMU_DEVICE *A,CONST PIANO_SMMU_DEVICE *B) {
  return A->Present==B->Present && A->Enabled==B->Enabled && A->Sid==B->Sid &&
    A->Mask==B->Mask && A->StreamIndex==B->StreamIndex && A->Type==B->Type && A->ContextBank==B->ContextBank && A->Smr==B->Smr && A->S2cr==B->S2cr &&
    A->Sctlr==B->Sctlr && A->Cbar==B->Cbar && A->Cba2r==B->Cba2r &&
    A->Tcr==B->Tcr && A->Tcr2==B->Tcr2 && A->Mair0==B->Mair0 && A->Mair1==B->Mair1 &&
    A->Ttbr0==B->Ttbr0 && A->Ttbr1==B->Ttbr1 && A->Fsr==B->Fsr && A->Far==B->Far && A->Fsynr==B->Fsynr;
}
STATIC EFI_STATUS CoexistFail(CONST CHAR8 *Phase,EFI_STATUS Status) {
  mCoexistFailed=TRUE;mUsbCleanupBlocked=TRUE;mUsbContext.TableMemory.Quarantined=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_USB_CONTRACT_FAILED phase=%a status=%r table_retained=1\n",Phase,Status));
  return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}
STATIC BOOLEAN SameGeometry(CONST PIANO_SMMU_SNAPSHOT *A,CONST PIANO_SMMU_SNAPSHOT *B) {
  return A->Valid && B->Valid && A->Base==B->Base && A->Window==B->Window && A->ContextBase==B->ContextBase &&
    A->PageShift==B->PageShift && A->Groups==B->Groups && A->Banks==B->Banks && A->ExtendedIds==B->ExtendedIds &&
    A->Id0==B->Id0 && A->Id1==B->Id1 && A->Id2==B->Id2 && A->GlobalControl==B->GlobalControl && A->GlobalFault==B->GlobalFault;
}
STATIC EFI_STATUS CoexistCheck(CONST VOID *Fdt,CONST CHAR8 *Phase,BOOLEAN UsbAttached) {
  PIANO_SMMU_SNAPSHOT Now;EFI_STATUS S=PianoSmmuCapture(Fdt,Phase,&Now);
  if(S!=EFI_SUCCESS)return CoexistFail(Phase,S);
  if(!SameGeometry(&mCoexistBasis,&Now))return CoexistFail(Phase,EFI_COMPROMISED_DATA);
  PIANO_SMMU_DEVICE *Ufs=&Now.Device[0],*Usb=&Now.Device[1];
  BOOLEAN Stable=SameContext(&mCoexistUfs,Ufs);
  BOOLEAN Separate=UsbAttached?(Usb->Present && Usb->Enabled && Usb->Type==0 &&
    Usb->Sid==0x40 && Usb->ContextBank!=Ufs->ContextBank && (Usb->Ttbr0&0x0000FFFFFFFFF000ULL)!=(Ufs->Ttbr0&0x0000FFFFFFFFF000ULL) &&
    (Usb->Ttbr0&0x0000FFFFFFFFF000ULL)==mUsbContext.TableMemory.Physical):!Usb->Present;
  if(UsbAttached) {
    PIANO_SMMU_DEVICE Expected=mCoexistBasis.Device[1];
    // OwnedOpen clears an inherited sticky USB FSR after its capture. Freeze
    // the actual post-clear fault fields only in the first full capture.
    if(!mCoexistReady){Expected.Fsr=Usb->Fsr;Expected.Far=Usb->Far;Expected.Fsynr=Usb->Fsynr;}
    Separate=Separate && SameContext(&Expected,Usb);
  }
  for(UINTN I=0;I<Now.Groups;++I) {
    if(!UsbAttached && I==mCoexistBasis.Device[1].StreamIndex)continue;
    if(Now.RawSmr[I]!=mCoexistBasis.RawSmr[I] || Now.RawS2cr[I]!=mCoexistBasis.RawS2cr[I])Stable=FALSE;
  }
  for(UINTN I=2;I<PIANO_SMMU_DEVICE_COUNT;++I)if(!SameContext(&mCoexistBasis.Device[I],&Now.Device[I]))Stable=FALSE;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_USB_COEXIST phase=%a stable_ufs=%u separate=%u ufs_cb=%u usb_cb=%u ufs_root=%lx usb_root=%lx\n",
    Phase,Stable,Separate,Ufs->ContextBank,Usb->ContextBank,Ufs->Ttbr0,Usb->Ttbr0));
  if(!Stable || !Separate)return CoexistFail(Phase,EFI_DEVICE_ERROR);
  if(UsbAttached && !mCoexistReady){mCoexistBasis=Now;mCoexistReady=TRUE;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ContractRead(UINTN Offset,UINT32 *Value) {
  if(mCoexistBasis.Window<4 || (Offset&3) || Offset>mCoexistBasis.Window-4 || mCoexistBasis.Base>MAX_UINTN-Offset)return EFI_COMPROMISED_DATA;
  *Value=MmioRead32(mCoexistBasis.Base+Offset);return EFI_SUCCESS;
}
STATIC EFI_STATUS ContractRead64(UINTN Offset,UINT64 *Value) {
  UINT32 Low,High;EFI_STATUS S=ContractRead(Offset,&Low);if(S!=EFI_SUCCESS)return S;
  S=ContractRead(Offset+4,&High);if(S!=EFI_SUCCESS)return S;*Value=Low|((UINT64)High<<32);return EFI_SUCCESS;
}
STATIC EFI_STATUS QuietContext(PIANO_SMMU_DEVICE *D) {
  UINTN G=(UINTN)1<<mCoexistBasis.PageShift,B=mCoexistBasis.ContextBase+((UINTN)D->ContextBank<<mCoexistBasis.PageShift);
  if(D->StreamIndex>=mCoexistBasis.Groups || D->ContextBank>=mCoexistBasis.Banks || B>mCoexistBasis.Window || mCoexistBasis.Window-B<0x6c)return EFI_COMPROMISED_DATA;
#define CBREAD(Offset,Field) do{EFI_STATUS C=ContractRead((Offset),&D->Field);if(C!=EFI_SUCCESS)return C;}while(0)
#define CBREAD64(Offset,Field) do{EFI_STATUS C=ContractRead64((Offset),&D->Field);if(C!=EFI_SUCCESS)return C;}while(0)
  CBREAD(0x800+4U*D->StreamIndex,Smr);CBREAD(0xC00+4U*D->StreamIndex,S2cr);
  CBREAD(G+4U*D->ContextBank,Cbar);CBREAD(G+0x800+4U*D->ContextBank,Cba2r);
  CBREAD(B,Sctlr);CBREAD(B+0x10,Tcr2);CBREAD64(B+0x20,Ttbr0);CBREAD64(B+0x28,Ttbr1);
  CBREAD(B+0x30,Tcr);CBREAD(B+0x38,Mair0);CBREAD(B+0x3c,Mair1);CBREAD(B+0x58,Fsr);CBREAD64(B+0x60,Far);CBREAD(B+0x68,Fsynr);
#undef CBREAD
#undef CBREAD64
  D->Enabled=(D->Sctlr&1)!=0;D->Type=(UINT8)((D->S2cr>>16)&3);return EFI_SUCCESS;
}
STATIC EFI_STATUS CoexistGuard(VOID *Context,CONST CHAR8 *Phase,BOOLEAN Full) {
  CONST VOID *Fdt=Context;
  if(!mCoexistReady || mCoexistFailed)return EFI_NOT_READY;
  if(Full)return CoexistCheck(Fdt,Phase,TRUE);
  if((mCoexistBasis.PageShift!=12 && mCoexistBasis.PageShift!=16) || !mCoexistBasis.Groups || mCoexistBasis.Groups>256 || !mCoexistBasis.Banks || mCoexistBasis.Banks>256)
    return CoexistFail(Phase,EFI_COMPROMISED_DATA);
  MemoryFence();
  UINT32 Value;
  CONST UINT32 Offsets[]={0,0x20,0x24,0x28,0x48};
  CONST UINT32 Expected[]={mCoexistBasis.GlobalControl,mCoexistBasis.Id0,mCoexistBasis.Id1,mCoexistBasis.Id2,mCoexistBasis.GlobalFault};
  for(UINTN I=0;I<ARRAY_SIZE(Offsets);++I)if(ContractRead(Offsets[I],&Value)!=EFI_SUCCESS || Value!=Expected[I])return CoexistFail(Phase,EFI_DEVICE_ERROR);
  for(UINTN I=0;I<mCoexistBasis.Groups;++I) {
    if(ContractRead(0x800+4U*I,&Value)!=EFI_SUCCESS || Value!=mCoexistBasis.RawSmr[I])return CoexistFail(Phase,EFI_DEVICE_ERROR);
    if(ContractRead(0xC00+4U*I,&Value)!=EFI_SUCCESS || Value!=mCoexistBasis.RawS2cr[I])return CoexistFail(Phase,EFI_DEVICE_ERROR);
  }
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I)if(mCoexistBasis.Device[I].Present) {
    PIANO_SMMU_DEVICE Current=mCoexistBasis.Device[I];EFI_STATUS S=QuietContext(&Current);
    if(S!=EFI_SUCCESS || !SameContext(&Current,&mCoexistBasis.Device[I]))return CoexistFail(Phase,S==EFI_SUCCESS?EFI_DEVICE_ERROR:S);
  }
  MemoryFence();++mCoexistQuietChecks;return EFI_SUCCESS;
}
STATIC EFI_STATUS ProxyBegin(CONST CHAR8 *Phase) {
  if(mProxyBusy)return EFI_NOT_READY;
  mProxyBusy=TRUE;
  EFI_STATUS S=PianoDwc3CheckStorageForExperiment(Phase,FALSE);if(S!=EFI_SUCCESS)mProxyBusy=FALSE;return S;
}
STATIC EFI_STATUS ProxyEnd(EFI_STATUS Status,CONST CHAR8 *Phase) {
  EFI_STATUS Check=PianoDwc3CheckStorageForExperiment(Phase,FALSE);mProxyBusy=FALSE;
  return Check!=EFI_SUCCESS?Check:Status;
}
STATIC EFI_STATUS ProxyReady(VOID *Context) {
  (VOID)Context;EFI_STATUS S=ProxyBegin("coexist-ready-before");if(S!=EFI_SUCCESS)return S;
  return ProxyEnd(mUnderlyingStorage.Ready(mUnderlyingStorage.Context),"coexist-ready-after");
}
STATIC EFI_STATUS ProxyInfo(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info) {
  (VOID)Context;EFI_STATUS S=ProxyBegin("coexist-info-before");if(S!=EFI_SUCCESS)return S;
  return ProxyEnd(mUnderlyingStorage.Info(mUnderlyingStorage.Context,Name,Info),"coexist-info-after");
}
STATIC EFI_STATUS ProxyRead(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer) {
  (VOID)Context;EFI_STATUS S=ProxyBegin("coexist-read-before");if(S!=EFI_SUCCESS)return S;
  return ProxyEnd(mUnderlyingStorage.ReadBlocks(mUnderlyingStorage.Context,Info,Lba,Bytes,Buffer),"coexist-read-after");
}
STATIC CONST PIANO_FB_STORAGE mProxyStorage={NULL,ProxyReady,ProxyInfo,ProxyRead};
#endif
#if PIANO_USB_UFS_FETCH || PIANO_USB_RAM_BOOT || PIANO_USB_SERVICE
#if !PIANO_USB_UFS_FETCH
#include "PianoUsbStorageExperiment.h"
STATIC BOOLEAN mUsbShutdownLive;
STATIC EFI_HANDLE mUsbShutdownHandle;
#endif
STATIC EFI_STATUS UsbHaltOnly(VOID) {
  if(!mUsbShutdownLive)return EFI_NOT_READY;
  MmioWrite32(USB_BASE+0xC704,MmioRead32(USB_BASE+0xC704)&~BIT31);MemoryFence();
  for(UINTN I=0;I<1000000;++I)if((MmioRead32(USB_BASE+0xC70C)&BIT22) && !(MmioRead32(USB_BASE+0xC704)&BIT31))return EFI_SUCCESS;
  return EFI_TIMEOUT;
}
STATIC PIANO_USB_SHUTDOWN mUsbShutdown={1,UsbHaltOnly};
STATIC EFI_STATUS InstallShutdown(VOID) {
  EFI_GUID Guid=PIANO_USB_SHUTDOWN_GUID;
  EFI_STATUS S=gBS->InstallMultipleProtocolInterfaces(&mUsbShutdownHandle,&Guid,&mUsbShutdown,NULL);
  if(S==EFI_SUCCESS){mUsbShutdownLive=TRUE;if(mUsbShutdownHandle==NULL)return EFI_DEVICE_ERROR;}
  return S;
}
STATIC EFI_STATUS RemoveShutdown(VOID) {
  if(!mUsbShutdownLive)return EFI_SUCCESS;
  if(mUsbShutdownHandle==NULL)return EFI_DEVICE_ERROR;
  EFI_GUID Guid=PIANO_USB_SHUTDOWN_GUID;EFI_STATUS S=gBS->UninstallMultipleProtocolInterfaces(mUsbShutdownHandle,&Guid,&mUsbShutdown,NULL);
  if(S==EFI_SUCCESS){mUsbShutdownHandle=NULL;mUsbShutdownLive=FALSE;}return S;
}
#endif
VOID PianoProbeUsbPower(CONST VOID *Fdt);
#ifdef PIANO_USB_EP0
#include "PianoUsbControl.h"
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device);
#endif
STATIC UINT32 UsbRead(UINT32 Offset){return MmioRead32(USB_BASE+Offset);}
STATIC VOID UsbWrite(UINT32 Offset,UINT32 Value){MmioWrite32(USB_BASE+Offset,Value);MemoryFence();}
STATIC UINT32 UsbBe32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC BOOLEAN UsbDt(CONST VOID *Fdt) {
  INT32 N=FdtPathOffset(Fdt,"/soc/ssusb@a600000/dwc3@a600000"),Len;
  if(N<0)return FALSE;
  CONST UINT8 *P=FdtGetProp(Fdt,N,"reg",&Len);
  if(P==NULL || Len!=16 || UsbBe32(P)!=0 || UsbBe32(P+4)!=USB_BASE || UsbBe32(P+8)!=0 || UsbBe32(P+12)!=0xD93C)return FALSE;
  P=FdtGetProp(Fdt,N,"iommus",&Len);return P!=NULL && Len==12 && UsbBe32(P+4)==0x40 && UsbBe32(P+8)==0;
}
STATIC EFI_STATUS UsbHalt(VOID) {
  UsbWrite(0xC704,UsbRead(0xC704)&~BIT31);
  for(UINTN I=0;I<10000;++I){if(UsbRead(0xC70C)&BIT22)return EFI_SUCCESS;gBS->Stall(10);}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS RunController(CONST VOID *Fdt,BOOLEAN Deferred,BOOLEAN *RebootOut) {
  if(RebootOut!=NULL)*RebootOut=FALSE;
  if(mUsbCleanupBlocked) {
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_RETAINED retry_refused=1\n"));return EFI_ACCESS_DENIED;
  }
  if(mUsbControllerRunning)return EFI_ALREADY_STARTED;
#if PIANO_USB_UFS_FETCH
  if(mCombinedBusy && !Deferred)return EFI_NOT_READY;
#endif
  mUsbControllerRunning=TRUE;
  mControllerRetireValid=mRetireProofConsumed=FALSE;ControllerZero(&mControllerRetire,sizeof(mControllerRetire));
  if(!UsbDt(Fdt)){mUsbControllerRunning=FALSE;return EFI_UNSUPPORTED;}
  PianoProbeUsbPower(Fdt);
  EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;EFI_CLOCK_PROTOCOL *Clock=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if(Status!=EFI_SUCCESS || Clock==NULL || Clock->GetClockID==NULL || Clock->EnableClock==NULL || Clock->DisableClock==NULL ||
     Clock->DisableClockPowerDomain==NULL || Clock->Version!=0x1000b ||
     (VOID *)Clock->GetClockPowerDomainID!=(VOID *)Clock->GetClockID ||
     (VOID *)Clock->EnableClockPowerDomain!=(VOID *)Clock->EnableClock){mUsbControllerRunning=FALSE;return EFI_UNSUPPORTED;}
  STATIC CONST CHAR8 *Names[]={"gcc_cfg_noc_usb3_prim_axi_clk","gcc_aggre_usb3_prim_axi_clk", "gcc_usb30_prim_master_clk",
    "gcc_usb30_prim_sleep_clk","gcc_usb30_prim_mock_utmi_clk","gcc_usb3_prim_phy_aux_clk","gcc_usb3_prim_phy_com_aux_clk","gcc_usb3_prim_phy_pipe_clk"};
  UINTN Ids[ARRAY_SIZE(Names)],Held=0,Domain=0,ClockReleaseFailures=0;
  BOOLEAN DomainHeld=FALSE,SafeToDisable=TRUE,RebootRequested=FALSE;
#if PIANO_USB_RAM_BOOT
  if(mRamBootMode){mRamClockReleaseMask=0;mRamDomainReleased=FALSE;mRamClockUnknown=FALSE;mRamOwnedStreamIndex=MAX_UINT16;}
#endif
  Status=Clock->GetClockPowerDomainID(Clock,"gcc_usb30_prim_gdsc",&Domain);
#if PIANO_USB_SERVICE
  Status=ServiceExact(Status,FALSE);
#endif
#if PIANO_USB_RAM_BOOT
  Status=RamClockStatus(Status,FALSE);
#endif
  if(!EFI_ERROR(Status)) {
    Status=Clock->EnableClockPowerDomain(Clock,Domain);
#if PIANO_USB_SERVICE
    Status=ServiceExact(Status,TRUE);
#endif
#if PIANO_USB_RAM_BOOT
    Status=RamClockStatus(Status,TRUE);
#endif
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DOMAIN %r\n",Status));if(EFI_ERROR(Status)){mUsbControllerRunning=FALSE;return Status;}
  DomainHeld=TRUE;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    Status=Clock->GetClockID(Clock,Names[I],&Ids[I]);
#if PIANO_USB_SERVICE
    Status=ServiceExact(Status,FALSE);
#endif
#if PIANO_USB_RAM_BOOT
    Status=RamClockStatus(Status,FALSE);
#endif
    if(!EFI_ERROR(Status)) {
      Status=Clock->EnableClock(Clock,Ids[I]);
#if PIANO_USB_SERVICE
      Status=ServiceExact(Status,TRUE);
#endif
#if PIANO_USB_RAM_BOOT
      Status=RamClockStatus(Status,TRUE);
#endif
    }
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_CLOCK %a %r\n",Names[I],Status));if(EFI_ERROR(Status))goto Exit;++Held;
  }
#if PIANO_USB_UFS_FETCH || PIANO_USB_RAM_BOOT || PIANO_USB_SERVICE
  if(Deferred) {
    Status=InstallShutdown();
    if(Status!=EFI_SUCCESS){if(mUsbShutdownLive){mUsbCleanupBlocked=TRUE;SafeToDisable=FALSE;}goto Exit;}
  }
#endif
  STATIC CONST UINT32 Registers[]={0xC110,0xC120,0xC12C,0xC200,0xC2C0,0xC400,0xC404,0xC408,0xC40C,0xC700,0xC704,0xC708,0xC70C,0xC720};
  for(UINTN I=0;I<ARRAY_SIZE(Registers);++I)
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_REG offset=%04x value=%08x\n",Registers[I],UsbRead(Registers[I])));
  UINT32 Id=UsbRead(0xC120);
  if((Id>>16)!=0x5533 && (Id>>16)!=0x3331 && (Id>>16)!=0x3332){Status=EFI_UNSUPPORTED;goto Exit;}
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_STATE utmi_ctrl0=%08x common0=%08x hs_ctrl2=%08x ss_com=%08x\n",
    MmioRead32(0x88E303C),MmioRead32(0x88E3054),MmioRead32(0x88E3064),MmioRead32(0x88E8008)));
  Status=UsbHalt();DEBUG((DEBUG_WARN,"SUNUEFI_USB_HALTED %r dsts=%08x\n",Status,UsbRead(0xC70C)));
  if(EFI_ERROR(Status)){SafeToDisable=FALSE;goto Exit;}
  mUsbDevice=(PIANO_DMA_DEVICE){.Name="usb",.StreamId=0x40,.AddressBits=32,.CacheLine=64};
  Status=PianoOwnedSmmuOpenUsb(Fdt,&mUsbContext,&mUsbDevice);
#if PIANO_USB_SERVICE
  Status=ServiceExact(Status,TRUE);
#endif
#if PIANO_USB_RAM_BOOT
  Status=RamExact(Status);
#endif
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SMMU_OPEN %r\n",Status));
#if PIANO_USB_RAM_BOOT
  if(mRamBootMode && Status==EFI_SUCCESS && mUsbContext.After.Valid && mUsbContext.After.Device[1].Present)
    mRamOwnedStreamIndex=mUsbContext.After.Device[1].StreamIndex;
#endif
#if PIANO_USB_UFS_FETCH
  if(Deferred && Status==EFI_SUCCESS
#if PIANO_USB_SERVICE
     && (!mPersistent.Opening || mCombinedBusy)
#endif
  ) {
    mCoexistUfs=mUsbContext.Before.Device[0];
    mCoexistBasis=mUsbContext.After;
    if(!mCoexistUfs.Present || !mCoexistUfs.Enabled || mCoexistUfs.Type!=0 || mCoexistUfs.Sid!=0x60)Status=EFI_NOT_READY;
    else if(!SameGeometry(&mUsbContext.Before,&mUsbContext.After))Status=CoexistFail("coexist-geometry",EFI_COMPROMISED_DATA);
    else Status=CoexistCheck(Fdt,"coexist-usb-attached",TRUE);
  }
#endif
  if(!EFI_ERROR(Status)) {
    PIANO_DMA_BUFFER LocalBuffer={0};PIANO_DMA_BUFFER *Buffer=&LocalBuffer;
#if PIANO_USB_RAM_BOOT
    if(mRamBootMode)Buffer=&mRamProbeBuffer; // retained proof cannot live on the stack
#endif
#if PIANO_USB_SERVICE
    if(mPersistent.Opening)Buffer=&mPersistent.ProbeBuffer;
#endif
    Status=PianoDmaAllocate(&mUsbDevice,4096,4096,32,PianoDmaBidirectional,Buffer);
#if PIANO_USB_SERVICE
    Status=ServiceExact(Status,Buffer->Signature!=0);
#endif
#if PIANO_USB_RAM_BOOT
    Status=RamExact(Status);
    if(mRamBootMode && Status!=EFI_SUCCESS && Buffer->Signature) {
      Buffer->Quarantined=TRUE;mUsbCleanupBlocked=TRUE;mUsbContext.TableMemory.Quarantined=TRUE;
    }
#endif
    if(!EFI_ERROR(Status)) {
      Status=PianoDmaMap(Buffer);
#if PIANO_USB_SERVICE
      Status=ServiceExact(Status,TRUE);
#endif
#if PIANO_USB_RAM_BOOT
      Status=RamExact(Status);
      if(mRamBootMode && Status!=EFI_SUCCESS) {
        Buffer->Quarantined=TRUE;mUsbCleanupBlocked=TRUE;mUsbContext.TableMemory.Quarantined=TRUE;
      }
#endif
      if(!EFI_ERROR(Status)) {
        UINT64 Pa;EFI_STATUS Translate=PianoIoPageTableTranslate(&mUsbContext.PageTable,Buffer->DeviceAddress,TRUE,&Pa);
          DEBUG((DEBUG_WARN,"SUNUEFI_USB_DMA_SOFTWARE %r pa=%lx expected=%lx iova=%lx\n",Translate,Pa,Buffer->Physical,Buffer->DeviceAddress));
        if(Translate!=EFI_SUCCESS || Pa!=Buffer->Physical)Status=EFI_COMPROMISED_DATA;
      }
#if PIANO_USB_RAM_BOOT
      if(!mRamBootMode || !mUsbCleanupBlocked)
#endif
#if PIANO_USB_SERVICE
      if(!mPersistent.Opening || !mUsbCleanupBlocked)
#endif
      {
        EFI_STATUS Free=PianoDmaFree(Buffer);
#if PIANO_USB_SERVICE
        Free=ServiceExact(Free,TRUE);
#endif
#if PIANO_USB_RAM_BOOT
        Free=RamExact(Free);
        if(mRamBootMode && (Free!=EFI_SUCCESS || Buffer->Signature)) {
          Buffer->Quarantined=TRUE;mUsbCleanupBlocked=TRUE;mUsbContext.TableMemory.Quarantined=TRUE;
          if(Free==EFI_SUCCESS)Free=EFI_DEVICE_ERROR;
        }
#endif
        if(EFI_ERROR(Free))Status=Free;
      }
    }
  }
#ifdef PIANO_USB_EP0
#if PIANO_USB_UFS_FETCH
  if(Deferred && Status==EFI_SUCCESS
#if PIANO_USB_SERVICE
     && (!mPersistent.Opening || mCombinedBusy)
#endif
  )Status=CoexistCheck(Fdt,"coexist-probe-retired",TRUE);
#endif
  if(!EFI_ERROR(Status)) {
#if PIANO_USB_SERVICE
    if(mPersistent.Opening) {
      Status=PianoDwc3ServiceStart(&mUsbContext,&mUsbDevice,&mPersistent.Config);
      if(Status==EFI_SUCCESS) {
        mPersistent.Clock=Clock;mPersistent.Held=Held;mPersistent.Domain=Domain;mPersistent.DomainHeld=DomainHeld;
        for(UINTN I=0;I<Held;++I)mPersistent.Ids[I]=Ids[I];
        mPersistent.Started=TRUE;return EFI_SUCCESS;
      }
      PIANO_DWC3_SERVICE_STATUS DeviceState;EFI_STATUS Stop=PianoDwc3ServiceStop(Status);
      if(PianoDwc3ServiceGetStatus(&DeviceState)!=EFI_SUCCESS || DeviceState.Retained || !DeviceState.DeviceHalted) {
        mUsbCleanupBlocked=TRUE;SafeToDisable=FALSE;
      }
      if(Stop!=EFI_SUCCESS && Status==EFI_SUCCESS)Status=Stop;
    } else
#endif
    {
    Status=PianoDwc3Ep0Experiment(&mUsbContext,&mUsbDevice);
    RebootRequested=PianoDwc3ConsumeRebootRequest();
    }
  }
#endif
#if PIANO_USB_UFS_FETCH
  if(Deferred && mCoexistReady && !mCoexistFailed) {
    EFI_STATUS Check=CoexistCheck(Fdt,"coexist-usb-halted",TRUE);
    if(Check!=EFI_SUCCESS)Status=Check;
  }
#endif
  {
    EFI_STATUS Close;
#if PIANO_USB_SERVICE
    if(mPersistent.Opening && mUsbCleanupBlocked)Close=EFI_ACCESS_DENIED;
    else
#endif
#if PIANO_USB_RAM_BOOT
    if(mRamBootMode && mUsbCleanupBlocked)Close=EFI_ACCESS_DENIED;
    else
#endif
      Close=PianoOwnedSmmuClose(&mUsbContext);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_SMMU_CLOSE status=%r attached=%u table_retained=%u\n",
      Close,mUsbContext.Attached,mUsbContext.TableMemory.Signature!=0));
    if(Close!=EFI_SUCCESS){Status=EFI_ERROR(Close)?Close:EFI_DEVICE_ERROR;mUsbCleanupBlocked=TRUE;}}
#if PIANO_USB_UFS_FETCH
  if(Deferred && mCoexistReady && !mCoexistFailed) {
    EFI_STATUS Check=CoexistCheck(Fdt,"coexist-usb-closed",FALSE);
    if(Check!=EFI_SUCCESS){Status=Check;mUsbCleanupBlocked=TRUE;}
  }
#endif
Exit:
#if PIANO_USB_SERVICE
  if(mPersistent.Opening && (mPersistent.Unknown || mUsbCleanupBlocked || mPersistent.ProbeBuffer.Signature ||
    mUsbContext.Attached || mUsbContext.Verified || mUsbContext.Domain!=NULL || mUsbContext.TableMemory.Signature || mUsbContext.TableMemory.Quarantined))SafeToDisable=FALSE;
  if(mPersistent.Opening)for(UINTN I=0;I<ARRAY_SIZE(mUsbContext.Mapping);++I)if(mUsbContext.Mapping[I].Used)SafeToDisable=FALSE;
#endif
#if PIANO_USB_RAM_BOOT
  if(mRamBootMode && (mRamClockUnknown || mUsbCleanupBlocked || mUsbContext.Attached || mUsbContext.Verified ||
     mUsbContext.Domain!=NULL || mUsbContext.TableMemory.Signature || mUsbContext.TableMemory.Quarantined || mRamProbeBuffer.Signature))SafeToDisable=FALSE;
  if(mRamBootMode)for(UINTN I=0;I<ARRAY_SIZE(mUsbContext.Mapping);++I)if(mUsbContext.Mapping[I].Used)SafeToDisable=FALSE;
#endif
#if PIANO_USB_UFS_FETCH
  if(Deferred && mUsbCleanupBlocked)SafeToDisable=FALSE;
#endif
#if PIANO_USB_UFS_FETCH || PIANO_USB_RAM_BOOT || PIANO_USB_SERVICE
  if(Deferred && SafeToDisable) {
    EFI_STATUS Remove=RemoveShutdown();
    if(Remove!=EFI_SUCCESS){Status=EFI_ERROR(Remove)?Remove:EFI_DEVICE_ERROR;mUsbCleanupBlocked=TRUE;SafeToDisable=FALSE;}
  }
#endif
  if(!SafeToDisable) {
    mUsbCleanupBlocked=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_RETAINED status=%r release_suppressed=1 clocks_held=%u domain_held=%u reboot_cancelled=%u\n",
      Status,(UINT32)Held,DomainHeld,RebootRequested));return Status;
  }
  while(Held) {
    --Held;EFI_STATUS Release=Clock->DisableClock(Clock,Ids[Held]);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_CLOCK_RELEASE %a %r\n",Names[Held],Release));
    if(Release!=EFI_SUCCESS){Status=EFI_ERROR(Release)?Release:EFI_DEVICE_ERROR;++ClockReleaseFailures;mUsbCleanupBlocked=TRUE;}
    else {
      mControllerRetire.ClockReleaseMask|=1U<<Held;
#if PIANO_USB_RAM_BOOT
      if(mRamBootMode)mRamClockReleaseMask|=(1U<<Held);
#endif
    }
  }
  if(DomainHeld && !ClockReleaseFailures) {
    EFI_STATUS Release=Clock->DisableClockPowerDomain(Clock,Domain);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_DOMAIN_RELEASE %r\n",Release));
    if(Release!=EFI_SUCCESS){Status=EFI_ERROR(Release)?Release:EFI_DEVICE_ERROR;mUsbCleanupBlocked=TRUE;}else DomainHeld=FALSE;
    if(Release==EFI_SUCCESS)mControllerRetire.GdscReleased=TRUE;
#if PIANO_USB_RAM_BOOT
    if(Release==EFI_SUCCESS && mRamBootMode)mRamDomainReleased=TRUE;
#endif
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_END status=%r clock_release_failures=%u domain_release_unconfirmed=%u retained_context=%u reboot_requested=%u\n",
    Status,(UINT32)ClockReleaseFailures,DomainHeld,mUsbCleanupBlocked,RebootRequested));
  if(!mUsbCleanupBlocked)mUsbControllerRunning=FALSE;
#if PIANO_USB_UFS_FETCH || PIANO_USB_SERVICE
  PIANO_SMMU_USB_RETIRE_EVIDENCE DeviceRetire;
  if(Status==EFI_SUCCESS && !mUsbCleanupBlocked && mControllerRetire.ClockReleaseMask==0xff && mControllerRetire.GdscReleased &&
     PianoDwc3GetRetireEvidence(&DeviceRetire)==EFI_SUCCESS) {
    mControllerRetire.Revision=1;mControllerRetire.DeviceCleanupStatus=DeviceRetire.DeviceCleanupStatus;
    mControllerRetire.ControllerCleanupStatus=Status;mControllerRetire.DeviceHalted=DeviceRetire.DeviceHalted;
    mControllerRetire.DmaFreed=DeviceRetire.DmaFreed;mControllerRetire.DmaBuffersFreed=DeviceRetire.DmaBuffersFreed;
    mControllerRetire.ClocksReleased=TRUE;mControllerRetireValid=TRUE;
  }
#endif
  if(RebootRequested && Status==EFI_SUCCESS && !mUsbCleanupBlocked) {
    if(Deferred) {
      if(RebootOut!=NULL)*RebootOut=TRUE;
      DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_REBOOT_DEFERRED usb_cleanup_success=1\n"));return Status;
    }
    DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_REBOOT_CLEAN cold_reset=1 usb_dma_freed=1 owned_closed=1 clocks_released=1 domain_released=1\n"));
    gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);CpuDeadLoop();
  }
  if(RebootRequested)DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_REBOOT_CANCELLED status=%r retention=%u\n",Status,mUsbCleanupBlocked));
  return Status;
}
EFI_STATUS PianoUsbControllerExperiment(CONST VOID *Fdt) {return RunController(Fdt,FALSE,NULL);}
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *Fdt,PIANO_SMMU_RETIRED_USB_PROOF *Proof) {
  if(Proof==NULL)return EFI_INVALID_PARAMETER;
  ControllerZero(Proof,sizeof(*Proof));
#if PIANO_USB_UFS_FETCH || PIANO_USB_SERVICE
  if(!mControllerRetireValid || mRetireProofConsumed || mUsbControllerRunning || mUsbCleanupBlocked || Fdt!=mUsbContext.Fdt)return EFI_NOT_READY;
  PIANO_SMMU_SNAPSHOT Current;EFI_STATUS S=PianoSmmuCapture(Fdt,"usb-retired-proof-current",&Current);if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  S=PianoOwnedSmmuMakeRetiredUsbProof(&mUsbContext,&mControllerRetire,&Current,Proof);
  if(S==EFI_SUCCESS)mRetireProofConsumed=TRUE;
  return S;
#else
  (VOID)Fdt;return EFI_UNSUPPORTED;
#endif
}
#if PIANO_USB_SERVICE
STATIC VOID EFIAPI ServiceTimer(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(mPersistent.Started && !mPersistent.ServicesLost)PianoDwc3ServicePollBounded(16);
}
STATIC VOID EFIAPI ServiceExit(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mPersistent.ServicesLost=TRUE;mPersistent.Started=FALSE;
  // No BS/native teardown after the signal. Stop should have happened before
  // an OS loader; an unexpected EBS retains pages and only halts hardware.
  if(PianoDwc3ServiceFenceExit()!=EFI_SUCCESS)CpuDeadLoop();
}
STATIC BOOLEAN ControllerAtApp(VOID) {
  if(mPersistent.ServicesLost || gBS==NULL || gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL)return FALSE;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);return Old==TPL_APPLICATION;
}
EFI_STATUS PianoUsbControllerServiceStart(CONST VOID *Fdt,CONST PIANO_DWC3_SERVICE_CONFIG *Config) {
  if(Config==NULL || Config->NowUs==NULL)return EFI_INVALID_PARAMETER;
  if(Config->Storage!=NULL && (Config->Storage->Ready==NULL || Config->Storage->Info==NULL || Config->Storage->ReadBlocks==NULL))return EFI_INVALID_PARAMETER;
  if(!ControllerAtApp())return EFI_UNSUPPORTED;
  if(mPersistent.Opening || mPersistent.Started || mUsbCleanupBlocked || mUsbControllerRunning)return EFI_ALREADY_STARTED;
  ZeroMem(&mPersistent,sizeof(mPersistent));mPersistent.Opening=TRUE;mPersistent.Config=*Config;
#if PIANO_USB_UFS_FETCH
  mCoexistReady=mCoexistFailed=mProxyBusy=FALSE;mCoexistQuietChecks=0;
  if(Config->Storage!=NULL) {
    mUnderlyingStorage=*Config->Storage;mCombinedBusy=TRUE;
    EFI_STATUS Bind=mUnderlyingStorage.Ready(mUnderlyingStorage.Context);
    if(Bind==EFI_SUCCESS)Bind=PianoDwc3SetStorageForExperiment(&mProxyStorage);
    if(Bind==EFI_SUCCESS)Bind=PianoDwc3SetStorageCheckForExperiment(CoexistGuard,(VOID *)Fdt);
    if(Bind!=EFI_SUCCESS) {
      PianoDwc3SetStorageCheckForExperiment(NULL,NULL);PianoDwc3SetStorageForExperiment(NULL);
      mCombinedBusy=FALSE;ZeroMem(&mUnderlyingStorage,sizeof(mUnderlyingStorage));mPersistent.Opening=FALSE;
      return EFI_ERROR(Bind)?Bind:EFI_DEVICE_ERROR;
    }
  }
  // Do not keep a caller-owned callback-struct pointer beyond this call.
  mPersistent.Config.Storage=NULL;
#else
  if(Config->Storage!=NULL){mPersistent.Opening=FALSE;return EFI_UNSUPPORTED;}
#endif
  EFI_STATUS S=RunController(Fdt,TRUE,NULL);mPersistent.Opening=FALSE;
  if(S!=EFI_SUCCESS || !mPersistent.Started) {
#if PIANO_USB_UFS_FETCH
    if(!mUsbCleanupBlocked) {
      PianoDwc3SetStorageCheckForExperiment(NULL,NULL);PianoDwc3SetStorageForExperiment(NULL);
      mCombinedBusy=FALSE;ZeroMem(&mUnderlyingStorage,sizeof(mUnderlyingStorage));
    }
#endif
    return S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;
  }
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ServiceExit,NULL,&gEfiEventExitBootServicesGuid,&mPersistent.ExitEvent);
  if(S==EFI_SUCCESS && mPersistent.ExitEvent!=NULL)
    S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ServiceTimer,NULL,&mPersistent.Timer);
  else if(S==EFI_SUCCESS)S=EFI_COMPROMISED_DATA;
  if(S==EFI_SUCCESS && mPersistent.Timer!=NULL)S=gBS->SetTimer(mPersistent.Timer,TimerPeriodic,10000);
  else if(S==EFI_SUCCESS)S=EFI_COMPROMISED_DATA;
  if(S!=EFI_SUCCESS) {
    // A warning cannot confirm whether an event object/notification is live.
    if(!EFI_ERROR(S)){mPersistent.Unknown=TRUE;mUsbCleanupBlocked=TRUE;return EFI_DEVICE_ERROR;}
    PIANO_USB_SERVICE_RETIRE_REPORT Report;PianoUsbControllerServiceStop(S,&Report);return S;
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoUsbControllerServicePumpApp(UINT32 Reason,UINTN BudgetUs) {
  if(mPersistent.ServicesLost || !mPersistent.Started)return EFI_NOT_READY;
  if(!ControllerAtApp())return EFI_UNSUPPORTED;
  return PianoDwc3ServicePumpApp(Reason,BudgetUs);
}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status) {
  EFI_STATUS S=PianoDwc3ServiceGetStatus(Status);if(S!=EFI_SUCCESS)return S;
  Status->Started=Status->Started && mPersistent.Started && !mPersistent.ServicesLost && !mUsbCleanupBlocked;
  Status->Retained=Status->Retained || mUsbCleanupBlocked || mPersistent.Unknown;
  Status->ServicesLost=Status->ServicesLost || mPersistent.ServicesLost;return EFI_SUCCESS;
}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report) {
  if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;
  if(mPersistent.ServicesLost || mPersistent.Opening || !ControllerAtApp())return Report->Status=EFI_ACCESS_DENIED;
  if(!mPersistent.Started || mPersistent.Unknown || mUsbCleanupBlocked){Report->Retained=mPersistent.Unknown || mUsbCleanupBlocked;return Report->Status=EFI_NOT_READY;}
  Report->Attempted=TRUE;mPersistent.Started=FALSE;EFI_STATUS S;
  if(mPersistent.Timer!=NULL) {
    S=gBS->SetTimer(mPersistent.Timer,TimerCancel,0);
    if(S==EFI_SUCCESS)S=gBS->CloseEvent(mPersistent.Timer);
    Report->TimerCloseStatus=S;
    if(S!=EFI_SUCCESS){mPersistent.Unknown=TRUE;mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
    mPersistent.Timer=NULL;
  }
  Report->DeviceStatus=PianoDwc3ServiceStop(Reason);PIANO_DWC3_SERVICE_STATUS DeviceState;
  S=PianoDwc3ServiceGetStatus(&DeviceState);
  if(S!=EFI_SUCCESS || DeviceState.Retained || !DeviceState.DeviceHalted || !DeviceState.DmaFreed) {
    mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=Report->DeviceStatus==EFI_SUCCESS?EFI_DEVICE_ERROR:Report->DeviceStatus;
  }
  Report->DeviceHalted=DeviceState.DeviceHalted;Report->DmaFreed=DeviceState.DmaFreed;Report->DmaBuffersFreed=DeviceState.DmaBuffersFreed;
#if PIANO_USB_UFS_FETCH
  if(mCombinedBusy) {
    S=CoexistCheck(mUsbContext.Fdt,"service-usb-halted",TRUE);
    if(S==EFI_SUCCESS)S=PianoDwc3SetStorageCheckForExperiment(NULL,NULL);
    if(S==EFI_SUCCESS)S=PianoDwc3SetStorageForExperiment(NULL);
    if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
    mCombinedBusy=FALSE;ZeroMem(&mUnderlyingStorage,sizeof(mUnderlyingStorage));
  }
#endif
  Report->OwnedCloseStatus=PianoOwnedSmmuClose(&mUsbContext);
  Report->DomainFreed=Report->OwnedCloseStatus==EFI_SUCCESS && !mUsbContext.Attached && !mUsbContext.Verified && mUsbContext.Domain==NULL &&
    !mUsbContext.TableMemory.Signature && !mUsbContext.TableMemory.Quarantined;
  for(UINTN I=0;I<ARRAY_SIZE(mUsbContext.Mapping);++I)if(mUsbContext.Mapping[I].Used)Report->DomainFreed=FALSE;
  if(!Report->DomainFreed){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=Report->OwnedCloseStatus==EFI_SUCCESS?EFI_DEVICE_ERROR:Report->OwnedCloseStatus;}
#if PIANO_USB_UFS_FETCH
  if(mCoexistReady) {
    S=CoexistCheck(mUsbContext.Fdt,"service-usb-closed",FALSE);
    if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  }
#endif
  S=RemoveShutdown();if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  if(mPersistent.ExitEvent!=NULL){S=gBS->CloseEvent(mPersistent.ExitEvent);if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;return Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}mPersistent.ExitEvent=NULL;}
  while(mPersistent.Held) {
    UINTN Index=--mPersistent.Held;S=mPersistent.Clock->DisableClock(mPersistent.Clock,mPersistent.Ids[Index]);
    if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
    else Report->ClockReleaseMask|=1U<<Index;
  }
  if(mPersistent.DomainHeld && !mUsbCleanupBlocked) {
    S=mPersistent.Clock->DisableClockPowerDomain(mPersistent.Clock,mPersistent.Domain);
    if(S!=EFI_SUCCESS){mUsbCleanupBlocked=TRUE;Report->Retained=TRUE;Report->Status=EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}else mPersistent.DomainHeld=FALSE;
  }
  Report->ClocksReleased=Report->ClockReleaseMask==0xff && !mPersistent.DomainHeld;
  Report->Clean=!Report->Retained && Report->DeviceStatus==EFI_SUCCESS && Report->DomainFreed && Report->ClocksReleased;
  if(!Report->Retained)mUsbControllerRunning=FALSE;
  if(Report->Clean) {
    mControllerRetire=(PIANO_SMMU_USB_RETIRE_EVIDENCE){.Revision=1,.DeviceCleanupStatus=Report->DeviceStatus,.ControllerCleanupStatus=EFI_SUCCESS,
      .DeviceHalted=Report->DeviceHalted,.DmaFreed=Report->DmaFreed,.ClocksReleased=Report->ClocksReleased,.GdscReleased=TRUE,
      .DmaBuffersFreed=Report->DmaBuffersFreed,.ClockReleaseMask=Report->ClockReleaseMask};mControllerRetireValid=TRUE;mRetireProofConsumed=FALSE;
  }
  if(Report->Status==EFI_SUCCESS && !Report->Clean)Report->Status=Report->DeviceStatus==EFI_SUCCESS?EFI_DEVICE_ERROR:Report->DeviceStatus;
  return Report->Status;
}
#else
EFI_STATUS PianoUsbControllerServiceStart(CONST VOID *Fdt,CONST PIANO_DWC3_SERVICE_CONFIG *Config){(VOID)Fdt;(VOID)Config;return EFI_UNSUPPORTED;}
EFI_STATUS PianoUsbControllerServicePumpApp(UINT32 Reason,UINTN BudgetUs){(VOID)Reason;(VOID)BudgetUs;return EFI_UNSUPPORTED;}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *S){if(S==NULL)return EFI_INVALID_PARAMETER;ControllerZero(S,sizeof(*S));S->Revision=1;return EFI_UNSUPPORTED;}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *R){(VOID)Reason;if(R==NULL)return EFI_INVALID_PARAMETER;ControllerZero(R,sizeof(*R));R->Revision=1;return R->Status=EFI_UNSUPPORTED;}
#endif
EFI_STATUS PianoUsbControllerRunForRamBoot(CONST VOID *Fdt,CONST PIANO_FB_BOOT *Boot,
  PIANO_FB_BOOT_ACTION *Action,PIANO_USB_BOOT_RETIRE_REPORT *Report,BOOLEAN *RebootRequested) {
  if(Report==NULL || Action==NULL || RebootRequested==NULL)return EFI_INVALID_PARAMETER;
  // Keep the default-off API independent of additional library calls.
  for(UINTN I=0;I<sizeof(*Report);++I)((volatile UINT8 *)Report)[I]=0;
  for(UINTN I=0;I<sizeof(*Action);++I)((volatile UINT8 *)Action)[I]=0;
  *RebootRequested=FALSE;
#if !PIANO_USB_RAM_BOOT
  (VOID)Fdt;(VOID)Boot;Report->Result=EFI_UNSUPPORTED;return EFI_UNSUPPORTED;
#else
  if(Boot==NULL || mRamBootMode || mUsbControllerRunning || mUsbCleanupBlocked)return EFI_NOT_READY;
  PIANO_SMMU_SNAPSHOT Before,After;
  Report->BeforeSnapshot=PianoSmmuCapture(Fdt,"ram-boot-before",&Before);
  if(Report->BeforeSnapshot!=EFI_SUCCESS || !Before.Valid || !Before.Groups || Before.Groups>ARRAY_SIZE(Before.RawSmr) ||
     !Before.Banks || Before.Banks>256 || Before.Device[0].Present || Before.Device[1].Present)
    return Report->Result=EFI_NOT_READY;
  Report->UfsAbsent=TRUE;Report->UsbAbsent=TRUE;
  EFI_STATUS Status=PianoDwc3SetBootForExperiment(Boot);
  if(Status!=EFI_SUCCESS)return Report->Result=EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
  mRamBootMode=TRUE;Report->Attempted=TRUE;
  Status=RunController(Fdt,TRUE,RebootRequested);Report->Returned=TRUE;
  Report->DomainFreed=!mUsbContext.Attached && !mUsbContext.Verified && mUsbContext.Domain==NULL &&
    !mUsbContext.TableMemory.Signature && !mUsbContext.TableMemory.Quarantined;
  for(UINTN I=0;I<ARRAY_SIZE(mUsbContext.Mapping);++I)if(mUsbContext.Mapping[I].Used)Report->DomainFreed=FALSE;
  Report->ClockReleaseMask=mRamClockReleaseMask;
  Report->OwnedStreamIndex=mRamOwnedStreamIndex;
  Report->ClocksReleased=mRamClockReleaseMask==0xff && mRamDomainReleased && !mRamClockUnknown;
  Report->Retained=mUsbCleanupBlocked || mUsbControllerRunning;
  // Consume the device result even on a controller failure, preserving any
  // transferred token in the caller's ledger; never discard a partial Take.
  EFI_STATUS Consume=PianoDwc3ConsumeBootAction(Action);
  if(Consume==EFI_SUCCESS || Action->Retained || Action->Token!=NULL) {
    Report->DeviceHalted=Action->Proof.DeviceHalted;Report->DmaFreed=Action->Proof.DmaFreed;
    if(Consume!=EFI_SUCCESS || !Action->Taken || Action->Retained)Report->Retained=TRUE;
  } else if(Consume!=EFI_NOT_FOUND) {Report->Retained=TRUE;if(Status==EFI_SUCCESS)Status=Consume;}
  if(Status==EFI_SUCCESS && !Report->Retained) {
    Report->AfterSnapshot=PianoSmmuCapture(Fdt,"ram-boot-after",&After);
    Report->UfsAbsent=Report->AfterSnapshot==EFI_SUCCESS && After.Valid && !After.Device[0].Present;
    Report->UsbAbsent=Report->AfterSnapshot==EFI_SUCCESS && After.Valid && !After.Device[1].Present;
    Report->OtherStreamsStable=Report->AfterSnapshot==EFI_SUCCESS && After.Valid &&
      mRamOwnedStreamIndex<Before.Groups && Before.Groups==After.Groups && Before.Banks==After.Banks &&
      Before.Base==After.Base && Before.Window==After.Window && Before.ContextBase==After.ContextBase &&
      Before.PageShift==After.PageShift && Before.ExtendedIds==After.ExtendedIds &&
      Before.Id0==After.Id0 && Before.Id1==After.Id1 && Before.Id2==After.Id2 &&
      Before.GlobalControl==After.GlobalControl && Before.GlobalFault==After.GlobalFault;
    if(Report->OtherStreamsStable) {
      for(UINTN I=0;I<Before.Groups;++I)if(I!=mRamOwnedStreamIndex &&
        (Before.RawSmr[I]!=After.RawSmr[I] || Before.RawS2cr[I]!=After.RawS2cr[I]))Report->OtherStreamsStable=FALSE;
      for(UINTN I=2;I<PIANO_SMMU_DEVICE_COUNT;++I)
        if(CompareMem(&Before.Device[I],&After.Device[I],sizeof(Before.Device[I]))!=0)Report->OtherStreamsStable=FALSE;
    }
  }
  Report->Clean=Status==EFI_SUCCESS && !Report->Retained && Report->DomainFreed && Report->ClocksReleased &&
    Report->UfsAbsent && Report->UsbAbsent && Report->OtherStreamsStable && (Consume==EFI_NOT_FOUND ||
      (Action->Taken && Action->Proof.AckCompleted && Action->Proof.QueueEmpty && Action->Proof.DispatchFrozen &&
       Action->Proof.DeviceHalted && Action->Proof.DmaFreed));
  if(!Report->Clean && Status==EFI_SUCCESS)Status=EFI_DEVICE_ERROR;
  if(Report->Clean) {
    EFI_STATUS Clear=PianoDwc3SetBootForExperiment(NULL);
    if(Clear!=EFI_SUCCESS){Report->Clean=FALSE;Report->Retained=TRUE;Status=Clear;}
  }
  if(!Report->Clean)*RebootRequested=FALSE;
  if(!Report->Retained)mRamBootMode=FALSE;
  Report->Result=Status;return Status;
#endif
}
#if PIANO_USB_UFS_FETCH
EFI_STATUS PianoUsbControllerRunWithStorage(CONST VOID *Fdt,CONST PIANO_FB_STORAGE *Storage,BOOLEAN *RebootRequested) {
  if(RebootRequested!=NULL)*RebootRequested=FALSE;
  if(Storage==NULL || RebootRequested==NULL || Storage->Ready==NULL || Storage->Info==NULL || Storage->ReadBlocks==NULL)return EFI_INVALID_PARAMETER;
  if(mCombinedBusy || mUsbControllerRunning || mUsbCleanupBlocked)return EFI_NOT_READY;
  mCombinedBusy=TRUE;mCoexistReady=mCoexistFailed=mProxyBusy=FALSE;mCoexistQuietChecks=0;mUnderlyingStorage=*Storage;
  EFI_STATUS S=mUnderlyingStorage.Ready(mUnderlyingStorage.Context);if(S!=EFI_SUCCESS){mCombinedBusy=FALSE;return S;}
  S=PianoDwc3SetStorageForExperiment(&mProxyStorage);if(S!=EFI_SUCCESS){mCombinedBusy=FALSE;return S;}
  S=PianoDwc3SetStorageCheckForExperiment(CoexistGuard,(VOID *)Fdt);
  if(S!=EFI_SUCCESS){PianoDwc3SetStorageForExperiment(NULL);mCombinedBusy=FALSE;return S;}
  S=RunController(Fdt,TRUE,RebootRequested);
  EFI_STATUS Clear=PianoDwc3SetStorageCheckForExperiment(NULL,NULL);
  if(Clear==EFI_SUCCESS)Clear=PianoDwc3SetStorageForExperiment(NULL);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_USB_CONTRACT_END quiet_checks=%lu clear=%r retained=%u\n",mCoexistQuietChecks,Clear,mUsbCleanupBlocked));
  if(Clear!=EFI_SUCCESS){*RebootRequested=FALSE;return S!=EFI_SUCCESS?S:Clear;}
  if(!mUsbCleanupBlocked){mCombinedBusy=FALSE;ZeroMem(&mUnderlyingStorage,sizeof(mUnderlyingStorage));}
  return S;
}
#endif
