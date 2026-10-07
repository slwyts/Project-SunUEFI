// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Controller wrapper; protocol/MMIO/DMA/owned HAL are host fixtures.
// Every case forks from untouched globals, including retained controller state.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#undef NULL
#ifndef PIANO_USB_RAM_BOOT
#define PIANO_USB_RAM_BOOT 1
#endif
#define PIANO_USB_EP0 1
#define PIANO_USB_UFS_FETCH 0
#include "../../uefi/core/PianoUsbController.c"
#include "../../uefi/core/PianoUsbStorageExperiment.h"

EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_RUNTIME_SERVICES Rt;STATIC EFI_CLOCK_PROTOCOL Clock;
STATIC UINT32 Case,ClockHeld,NextClock,Dctl,ClockReleases,DomainReleases,Opened,Closed,DwcRuns,Maps,Allocations,Frees,Captures,Setters,Clears,Installs,Removes;
STATIC BOOLEAN DomainHeld;
STATIC CONST PIANO_FB_BOOT *Backend;
STATIC PIANO_FB_BOOT_ACTION DeviceAction;
STATIC PIANO_SMMU_SNAPSHOT Hardware;
STATIC CONST UINT16 OwnedIndex=117;

BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(void)Level;return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(void)Level;(void)Format;}
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID PianoProbeUsbPower(CONST VOID *Fdt){assert(Fdt==(VOID *)123);}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){(void)Path;assert(Fdt==(VOID *)123);return 1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Bytes){
  STATIC CONST UINT8 Reg[]={0,0,0,0,0x0a,0x60,0,0,0,0,0,0,0,0,0xd9,0x3c},Iommu[]={0,0,0,1,0,0,0,0x40,0,0,0,0};
  assert(Fdt==(VOID *)123&&Node==1);if(!strcmp(Name,"reg")){*Bytes=16;return Reg;}assert(!strcmp(Name,"iommus"));*Bytes=12;return Iommu;
}
UINT32 EFIAPI MmioRead32(UINTN Address){
  if(Address==USB_BASE+0xC120)return 0x33313130;
  if(Address==USB_BASE+0xC704)return Dctl;
  if(Address==USB_BASE+0xC70C)return BIT22;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){assert(Address==USB_BASE+0xC704);Dctl=Value;return Value;}
STATIC EFI_STATUS EFIAPI Stall(UINTN Us){(void)Us;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){(void)Guid;(void)Registration;*Interface=&Clock;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...) {
  Installs++;assert(ClockHeld==255&&DomainHeld);va_list Args;va_start(Args,Handle);
  EFI_GUID *Guid=va_arg(Args,EFI_GUID *);VOID *Protocol=va_arg(Args,VOID *);assert(va_arg(Args,VOID *)==NULL);va_end(Args);
  EFI_GUID Expected=PIANO_USB_SHUTDOWN_GUID;assert(!memcmp(Guid,&Expected,sizeof(Expected)));
#if PIANO_USB_RAM_BOOT
  assert(Protocol==&mUsbShutdown);
#else
  (void)Protocol;
#endif
  *Handle=(VOID *)0x456;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE Handle,...) {
  Removes++;assert(Handle==(VOID *)0x456&&ClockHeld==255&&DomainHeld&&Closed==1);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ClockId(EFI_CLOCK_PROTOCOL *C,CONST CHAR8 *Name,UINTN *Id){
  assert(C==&Clock);*Id=!strcmp(Name,"gcc_usb30_prim_gdsc")?100:NextClock++;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Enable(EFI_CLOCK_PROTOCOL *C,UINTN Id){
  assert(C==&Clock);if(Id==100)DomainHeld=TRUE;else {assert(Id<8&&DomainHeld);ClockHeld|=1U<<Id;}return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Disable(EFI_CLOCK_PROTOCOL *C,UINTN Id){
  assert(C==&Clock);if(Id==100){DomainReleases++;assert(!ClockHeld);if(Case==14)return EFI_WARN_STALE_DATA;DomainHeld=FALSE;return EFI_SUCCESS;}
  ClockReleases++;assert(Id<8&&(ClockHeld&(1U<<Id)));if(Case==13&&Id==3)return EFI_WARN_STALE_DATA;
  ClockHeld&=~(1U<<Id);return EFI_SUCCESS;
}
STATIC VOID EFIAPI Reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data){
  (void)Type;(void)Status;(void)Bytes;(void)Data;assert(!"RAM wrapper must defer reset to App");
}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"Controller never calls fail-stop in this host run");abort();}
EFI_STATUS PianoDwc3SetBootForExperiment(CONST PIANO_FB_BOOT *Boot){
  Setters++;if(Boot==NULL){Clears++;Backend=NULL;return EFI_SUCCESS;}
  if(Case==4)return EFI_WARN_STALE_DATA;Backend=Boot;return EFI_SUCCESS;
}
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action){
  if(!DwcRuns){memset(Action,0,sizeof(*Action));return EFI_NOT_FOUND;}
  *Action=DeviceAction;return EFI_SUCCESS;
}
BOOLEAN PianoDwc3ConsumeRebootRequest(VOID){return FALSE;}
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){
  (void)D;assert(C->Attached&&C->Verified&&ClockHeld==255&&DomainHeld&&Backend);DwcRuns++;
#if PIANO_USB_RAM_BOOT
  assert(mUsbShutdownLive && mUsbShutdown.Halt()==EFI_SUCCESS && C->Attached && C->Domain!=NULL);
#endif
  DeviceAction=(PIANO_FB_BOOT_ACTION){.Taken=TRUE,.Context=Backend->Context,.Token=(VOID *)0xcafe,
    .Proof={.AckCompleted=TRUE,.QueueEmpty=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DispatchFrozen=TRUE,.AckBytes=4,.DmaBuffersFreed=9}};
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){
  (void)D;assert(Fdt==(VOID *)123&&ClockHeld==255&&DomainHeld);Opened++;
  C->Before=Hardware;C->Domain=(VOID *)0x111;C->Attached=C->Verified=TRUE;C->TableMemory.Signature=1;
  Hardware.Device[1]=(PIANO_SMMU_DEVICE){.Sid=0x40,.Present=TRUE,.Enabled=TRUE,.StreamIndex=OwnedIndex,.Type=0,.ContextBank=1,.Ttbr0=0x82000000};
  if(Case==5)Hardware.Device[1].StreamIndex=256;
  Hardware.RawSmr[OwnedIndex]=0x80000040;Hardware.RawS2cr[OwnedIndex]=1;C->After=Hardware;
  return Case==15?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){
  Closed++;assert(!C->Mapping[0].Used);
  if(Case==12)return EFI_WARN_STALE_DATA;
  memset(C,0,sizeof(*C));Hardware.Device[1]=(PIANO_SMMU_DEVICE){.Sid=0x40};Hardware.RawSmr[OwnedIndex]=Hardware.RawS2cr[OwnedIndex]=0;
  if(Case==10)C->Domain=(VOID *)0x111;
  if(Case==11)C->Mapping[0].Used=TRUE;
  return EFI_SUCCESS;
}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *D,UINTN Bytes,UINTN Align,UINT8 Bits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *B){
  (void)D;(void)Align;(void)Bits;(void)Direction;Allocations++;*B=(PIANO_DMA_BUFFER){.Signature=1,.Bytes=Bytes,.Physical=0x81000000,.DeviceAddress=0x40000000};return EFI_SUCCESS;
}
EFI_STATUS PianoDmaMap(PIANO_DMA_BUFFER *B){Maps++;B->Mapped=TRUE;return EFI_SUCCESS;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){Frees++;assert(!B->Active);memset(B,0,sizeof(*B));return EFI_SUCCESS;}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,BOOLEAN Write,UINT64 *Pa){(void)T;(void)Iova;(void)Write;*Pa=0x81000000;return EFI_SUCCESS;}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *S){
  assert(Fdt==(VOID *)123);Captures++;*S=Hardware;
  if(!strcmp(Phase,"ram-boot-before")) {
    if(Case==1)S->Device[0].Present=TRUE;
    if(Case==2)S->Device[1].Present=TRUE;
    if(Case==3)return EFI_WARN_STALE_DATA;
  } else {
    assert(!strcmp(Phase,"ram-boot-after"));
    if(Case==6)S->Device[0].Present=TRUE;
    if(Case==7)S->Device[1].Present=TRUE;
    if(Case==8)S->RawS2cr[113]^=1; // not the actual owned slot117
    if(Case==9)S->Device[2].Tcr^=1;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Ready(VOID *Context){(void)Context;return EFI_SUCCESS;}
STATIC EFI_STATUS Validate(VOID *Context,CONST PIANO_FASTBOOT *F,CONST PIANO_FB_BOOT_VIEW *V){(void)Context;(void)F;(void)V;return EFI_SUCCESS;}
STATIC EFI_STATUS Take(VOID *Context,PIANO_FASTBOOT *F,CONST PIANO_FB_BOOT_VIEW *V,VOID **T){(void)Context;(void)F;(void)V;*T=(VOID *)0xcafe;return EFI_SUCCESS;}
STATIC VOID Setup(UINT32 Scenario){
  Case=Scenario;memset(&Bs,0,sizeof(Bs));memset(&Rt,0,sizeof(Rt));memset(&Clock,0,sizeof(Clock));gBS=&Bs;gRT=&Rt;
  Bs.Stall=Stall;Bs.LocateProtocol=Locate;Bs.InstallMultipleProtocolInterfaces=Install;Bs.UninstallMultipleProtocolInterfaces=Uninstall;
  Rt.ResetSystem=Reset;Clock.Version=0x1000b;
  Clock.GetClockID=Clock.GetClockPowerDomainID=ClockId;Clock.EnableClock=Clock.EnableClockPowerDomain=Enable;Clock.DisableClock=Clock.DisableClockPowerDomain=Disable;
  memset(&Hardware,0,sizeof(Hardware));Hardware.Valid=TRUE;Hardware.Base=0x15000000;Hardware.Window=0x100000;Hardware.ContextBase=0x40000;
  Hardware.PageShift=12;Hardware.Groups=128;Hardware.Banks=64;Hardware.Id0=1;Hardware.Id1=2;Hardware.Id2=3;
  Hardware.Device[0].Sid=0x60;Hardware.Device[1].Sid=0x40;
  Hardware.Device[2]=(PIANO_SMMU_DEVICE){.Sid=0x10,.Present=TRUE,.Enabled=TRUE,.StreamIndex=3,.Type=0,.ContextBank=2,.Tcr=0x11,.Ttbr0=0x83000000};
  Hardware.RawSmr[3]=0x80000010;Hardware.RawS2cr[3]=2;Hardware.RawSmr[113]=0x123;Hardware.RawS2cr[113]=0x456;
  Dctl=BIT31;
}
STATIC VOID Run(UINT32 Scenario){
  Setup(Scenario);PIANO_FB_BOOT Boot={.Context=(VOID *)0x23,.MaxImageBytes=0x100000,.Ready=Ready,.Validate=Validate,.TakeAfterAck=Take};
  PIANO_FB_BOOT_ACTION Action;PIANO_USB_BOOT_RETIRE_REPORT Report;BOOLEAN Reboot=TRUE;
  EFI_STATUS Status=PianoUsbControllerRunForRamBoot((VOID *)123,&Boot,&Action,&Report,&Reboot);
  if(!PIANO_USB_RAM_BOOT){assert(Status==EFI_UNSUPPORTED&&!Captures&&!Setters&&!Opened&&!DwcRuns&&!Report.Clean&&!Reboot);return;}
  if(Scenario==0) {
    assert(Status==EFI_SUCCESS&&Report.Clean&&!Report.Retained&&Report.DomainFreed&&Report.ClocksReleased&&Report.OtherStreamsStable);
    assert(Report.OwnedStreamIndex==117&&Report.ClockReleaseMask==255&&Report.UfsAbsent&&Report.UsbAbsent);
    assert(Report.DeviceHalted&&Report.DmaFreed&&Action.Taken&&Action.Token==(VOID *)0xcafe);
    assert(Captures==2&&Opened==1&&Closed==1&&DwcRuns==1&&ClockReleases==8&&DomainReleases==1&&Clears==1);
    assert(Installs==1&&Removes==1);
    assert(Allocations==1&&Maps==1&&Frees==1&&!ClockHeld&&!DomainHeld&&!Reboot);return;
  }
  assert(Status!=EFI_SUCCESS&&!Report.Clean&&!Reboot&&!Clears);
  if(Scenario<=3) {assert(Status==EFI_NOT_READY&&Captures==1&&!Setters&&!Opened&&!DwcRuns&&!ClockReleases);return;}
  if(Scenario==4) {assert(Status==EFI_DEVICE_ERROR&&!Opened&&!DwcRuns&&!ClockReleases);return;}
  if(Scenario==5)assert(!Report.OtherStreamsStable);
  if(Scenario==6)assert(!Report.UfsAbsent);
  if(Scenario==7)assert(!Report.UsbAbsent);
  if(Scenario==8 || Scenario==9)assert(!Report.OtherStreamsStable);
  if(Scenario==10 || Scenario==11 || Scenario==12 || Scenario==15) {
    assert(Report.Retained&&!Report.ClocksReleased&&ClockHeld==255&&DomainHeld&&!ClockReleases&&!DomainReleases);
    assert(Installs==1&&!Removes);
    if(Scenario==15)assert(!Maps&&!DwcRuns);
    BOOLEAN Again=TRUE;PIANO_FB_BOOT_ACTION A;PIANO_USB_BOOT_RETIRE_REPORT R;
    assert(PianoUsbControllerRunForRamBoot((VOID *)123,&Boot,&A,&R,&Again)==EFI_NOT_READY&&!Again);
  }
  if(Scenario==10 || Scenario==11)assert(!Report.DomainFreed);
  if(Scenario==13)assert(Report.Retained&&!Report.ClocksReleased&&Report.ClockReleaseMask==0xf7&&!DomainReleases&&DomainHeld);
  if(Scenario==14)assert(Report.Retained&&!Report.ClocksReleased&&Report.ClockReleaseMask==0xff&&DomainReleases==1&&DomainHeld);
}
int main(void){
  UINT32 Count=PIANO_USB_RAM_BOOT?16:1;
  for(UINT32 I=0;I<Count;I++) {
    pid_t P=fork();assert(P>=0);if(P==0){Run(I);_exit(0);}
    int Status;assert(waitpid(P,&Status,0)==P);
    if(!WIFEXITED(Status)||WEXITSTATUS(Status)!=0){fprintf(stderr,"Controller RAM boot case %u failed\n",I);return 1;}
  }
  printf("Actual Controller RAM boot %u fork-isolated cases PASS: actual slot117, SID absence, peers/display, zero domain/maps/table, clocks/GDSC, warning retention; default=%u; no device\n",Count,(UINT32)!PIANO_USB_RAM_BOOT);
  return 0;
}
