// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual controller source, fake protocol/resources. Never accesses a device.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define PIANO_USB_EP0 1
#include "../bootprofiles/uefi-app/PianoUsbController.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;static EFI_RUNTIME_SERVICES rt;static EFI_CLOCK_PROTOCOL clock;
static jmp_buf reset_return;
static UINTN next_id,enabled_clocks,release_calls,reset_calls,dead_loops,close_calls,consume_calls,dwc_calls;
static BOOLEAN domain_enabled,reboot_request,fail_close,fail_domain,fail_halt;
static INTN fail_clock;
static EFI_STATUS dwc_status;
static UINT32 dctl;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
VOID PianoProbeUsbPower(CONST VOID *Fdt){assert(Fdt==(VOID *)123);}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){assert(Fdt==(VOID *)123);return 1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Len) {
  static CONST UINT8 reg[]={0,0,0,0,0x0a,0x60,0,0,0,0,0,0,0,0,0xd9,0x3c};
  static CONST UINT8 iommu[]={0,0,0,1,0,0,0,0x40,0,0,0,0};
  assert(Fdt==(VOID *)123 && Node==1);
  if(!strcmp(Name,"reg")){*Len=sizeof(reg);return reg;}
  assert(!strcmp(Name,"iommus"));*Len=sizeof(iommu);return iommu;
}
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address==USB_BASE+0xC120)return 0x33313130;
  if(Address==USB_BASE+0xC704)return dctl;
  if(Address==USB_BASE+0xC70C)return fail_halt?0:BIT22;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){assert(Address==USB_BASE+0xC704);dctl=Value;return Value;}
static EFI_STATUS EFIAPI stall(UINTN Us){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){*Interface=&clock;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI get_id(EFI_CLOCK_PROTOCOL *This,CONST CHAR8 *Name,UINTN *Id) {
  assert(This==&clock);*Id=!strcmp(Name,"gcc_usb30_prim_gdsc")?100:next_id++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI enable(EFI_CLOCK_PROTOCOL *This,UINTN Id) {
  assert(This==&clock);if(Id==100)domain_enabled=TRUE;else {assert(Id<8 && domain_enabled);enabled_clocks|=(1U<<Id);}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI disable(EFI_CLOCK_PROTOCOL *This,UINTN Id) {
  assert(This==&clock && (close_calls || fail_halt));
  if(Id==100) {
    assert(!enabled_clocks && release_calls==8);if(fail_domain)return EFI_DEVICE_ERROR;
    domain_enabled=FALSE;return EFI_SUCCESS;
  }
  assert(Id<8 && (enabled_clocks&(1U<<Id)));++release_calls;
  if((INTN)Id==fail_clock)return EFI_DEVICE_ERROR;
  enabled_clocks&=~(1U<<Id);return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device) {
  assert(enabled_clocks==255 && domain_enabled);Context->Attached=TRUE;Context->TableMemory.Signature=1;
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *Context) {
  assert(dwc_calls==1 && consume_calls==1 && !Context->Mapping[0].Used);++close_calls;
  if(fail_close)return EFI_DEVICE_ERROR;
  memset(Context,0,sizeof(*Context));return EFI_SUCCESS;
}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *Device,UINTN Bytes,UINTN Align,UINT8 Bits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *Buffer) {
  *Buffer=(PIANO_DMA_BUFFER){.Signature=1,.Bytes=Bytes,.Physical=0x81000000,.DeviceAddress=0x40000000};return EFI_SUCCESS;
}
EFI_STATUS PianoDmaMap(PIANO_DMA_BUFFER *Buffer){Buffer->Mapped=TRUE;return EFI_SUCCESS;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer){assert(!Buffer->Active);memset(Buffer,0,sizeof(*Buffer));return EFI_SUCCESS;}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *Table,UINT64 Iova,BOOLEAN Write,UINT64 *Pa){*Pa=0x81000000;return EFI_SUCCESS;}
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device) {
  assert(Context->Attached && enabled_clocks==255 && domain_enabled);++dwc_calls;return dwc_status;
}
BOOLEAN PianoDwc3ConsumeRebootRequest(VOID){++consume_calls;BOOLEAN Request=reboot_request;reboot_request=FALSE;return Request;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data) {
  assert(Type==EfiResetCold && Status==EFI_SUCCESS && !Bytes && !Data);
  assert(close_calls==1 && !mUsbContext.Attached && !mUsbContext.TableMemory.Signature);
  assert(release_calls==8 && !enabled_clocks && !domain_enabled && !mUsbCleanupBlocked);++reset_calls;
}
VOID EFIAPI CpuDeadLoop(VOID){assert(reset_calls==1);++dead_loops;longjmp(reset_return,1);}
static VOID init(VOID) {
  memset(&bs,0,sizeof(bs));memset(&rt,0,sizeof(rt));memset(&clock,0,sizeof(clock));memset(&mUsbContext,0,sizeof(mUsbContext));
  gBS=&bs;gRT=&rt;bs.Stall=stall;bs.LocateProtocol=locate;rt.ResetSystem=reset;
  clock.Version=0x1000b;clock.GetClockID=clock.GetClockPowerDomainID=get_id;
  clock.EnableClock=clock.EnableClockPowerDomain=enable;clock.DisableClock=clock.DisableClockPowerDomain=disable;
  next_id=enabled_clocks=release_calls=reset_calls=dead_loops=close_calls=consume_calls=dwc_calls=0;
  domain_enabled=reboot_request=fail_close=fail_domain=fail_halt=mUsbCleanupBlocked=mUsbControllerRunning=FALSE;fail_clock=-1;dwc_status=EFI_SUCCESS;dctl=BIT31;
}
int main(void) {
  init();reboot_request=TRUE;
  if(!setjmp(reset_return)){PianoUsbControllerExperiment((VOID *)123);assert(FALSE);}
  assert(reset_calls==1 && dead_loops==1 && !reboot_request);
  init();assert(PianoUsbControllerExperiment((VOID *)123)==EFI_SUCCESS);
  assert(!reset_calls && close_calls==1 && release_calls==8 && !domain_enabled); // continue/normal return.
  init();reboot_request=TRUE;fail_close=TRUE;
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_DEVICE_ERROR && !reset_calls);
  assert(mUsbCleanupBlocked && mUsbContext.Attached && mUsbContext.TableMemory.Signature && !domain_enabled);
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_ACCESS_DENIED && dwc_calls==1);
  init();reboot_request=TRUE;fail_clock=3;
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_DEVICE_ERROR && !reset_calls);
  assert(mUsbCleanupBlocked && enabled_clocks==(1U<<3) && domain_enabled && !mUsbContext.TableMemory.Signature);
  init();reboot_request=TRUE;fail_domain=TRUE;
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_DEVICE_ERROR && !reset_calls && mUsbCleanupBlocked && domain_enabled);
  init();reboot_request=TRUE;dwc_status=EFI_DEVICE_ERROR;
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_DEVICE_ERROR && !reset_calls && !domain_enabled);
  init();fail_halt=TRUE;
  assert(PianoUsbControllerExperiment((VOID *)123)==EFI_TIMEOUT && !reset_calls && !dwc_calls);
  assert(enabled_clocks==255 && domain_enabled && !release_calls && mUsbCleanupBlocked);
  puts("USB reboot controller actual source: one-shot request, owned-close before clocks/domain before reset, reset-return DeadLoop, continue return, close/clock/domain/Halt failure retention and retry refusal passed.");
  return 0;
}
