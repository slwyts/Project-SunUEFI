// SPDX-License-Identifier: BSD-2-Clause-Patent
// Retained DWC3 controller state and USB0 owned DMA context, isolated from UFS.
#include "PianoOwnedSmmu.h"
#include <Protocol/EFIClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#define USB_BASE 0xA600000U
VOID PianoProbeUsbPower(CONST VOID *Fdt);
#ifdef PIANO_USB_EP0
EFI_STATUS PianoDwc3Ep0Experiment(PIANO_OWNED_SMMU *Context,PIANO_DMA_DEVICE *Device);
#endif
STATIC PIANO_OWNED_SMMU mUsbContext;
STATIC PIANO_DMA_DEVICE mUsbDevice;
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
EFI_STATUS PianoUsbControllerExperiment(CONST VOID *Fdt) {
  if(!UsbDt(Fdt))return EFI_UNSUPPORTED;
  PianoProbeUsbPower(Fdt);
  EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;EFI_CLOCK_PROTOCOL *Clock=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if(EFI_ERROR(Status) || Clock->Version!=0x1000b ||
     (VOID *)Clock->GetClockPowerDomainID!=(VOID *)Clock->GetClockID ||
     (VOID *)Clock->EnableClockPowerDomain!=(VOID *)Clock->EnableClock)return EFI_UNSUPPORTED;
  STATIC CONST CHAR8 *Names[]={"gcc_cfg_noc_usb3_prim_axi_clk","gcc_aggre_usb3_prim_axi_clk", "gcc_usb30_prim_master_clk",
    "gcc_usb30_prim_sleep_clk","gcc_usb30_prim_mock_utmi_clk","gcc_usb3_prim_phy_aux_clk","gcc_usb3_prim_phy_com_aux_clk","gcc_usb3_prim_phy_pipe_clk"};
  UINTN Ids[ARRAY_SIZE(Names)],Held=0,Domain=0;BOOLEAN DomainHeld=FALSE;
  Status=Clock->GetClockPowerDomainID(Clock,"gcc_usb30_prim_gdsc",&Domain);
  if(!EFI_ERROR(Status))Status=Clock->EnableClockPowerDomain(Clock,Domain);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DOMAIN %r\n",Status));if(EFI_ERROR(Status))return Status;
  DomainHeld=TRUE;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    Status=Clock->GetClockID(Clock,Names[I],&Ids[I]);if(!EFI_ERROR(Status))Status=Clock->EnableClock(Clock,Ids[I]);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_CLOCK %a %r\n",Names[I],Status));if(EFI_ERROR(Status))goto Exit;++Held;
  }
  STATIC CONST UINT32 Registers[]={0xC110,0xC120,0xC12C,0xC200,0xC2C0,0xC400,0xC404,0xC408,0xC40C,0xC700,0xC704,0xC708,0xC70C,0xC720};
  for(UINTN I=0;I<ARRAY_SIZE(Registers);++I)
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_REG offset=%04x value=%08x\n",Registers[I],UsbRead(Registers[I])));
  UINT32 Id=UsbRead(0xC120);
  if((Id>>16)!=0x5533 && (Id>>16)!=0x3331 && (Id>>16)!=0x3332){Status=EFI_UNSUPPORTED;goto Exit;}
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_PHY_STATE utmi_ctrl0=%08x common0=%08x hs_ctrl2=%08x ss_com=%08x\n",
    MmioRead32(0x88E303C),MmioRead32(0x88E3054),MmioRead32(0x88E3064),MmioRead32(0x88E8008)));
  Status=UsbHalt();DEBUG((DEBUG_WARN,"SUNUEFI_USB_HALTED %r dsts=%08x\n",Status,UsbRead(0xC70C)));
  if(EFI_ERROR(Status))goto Exit;
  mUsbDevice=(PIANO_DMA_DEVICE){.Name="usb",.StreamId=0x40,.AddressBits=32,.CacheLine=64};
  Status=PianoOwnedSmmuOpenUsb(Fdt,&mUsbContext,&mUsbDevice);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_SMMU_OPEN %r\n",Status));
  if(!EFI_ERROR(Status)) {
    PIANO_DMA_BUFFER Buffer;
    Status=PianoDmaAllocate(&mUsbDevice,4096,4096,32,PianoDmaBidirectional,&Buffer);
    if(!EFI_ERROR(Status)) {
      Status=PianoDmaMap(&Buffer);
      if(!EFI_ERROR(Status)) {
        UINT64 Pa;EFI_STATUS Translate=PianoIoPageTableTranslate(&mUsbContext.PageTable,Buffer.DeviceAddress,TRUE,&Pa);
        DEBUG((DEBUG_WARN,"SUNUEFI_USB_DMA_SOFTWARE %r pa=%lx expected=%lx iova=%lx\n",Translate,Pa,Buffer.Physical,Buffer.DeviceAddress));
      }
      EFI_STATUS Free=PianoDmaFree(&Buffer);if(EFI_ERROR(Free))Status=Free;
    }
  }
#ifdef PIANO_USB_EP0
  if(!EFI_ERROR(Status))Status=PianoDwc3Ep0Experiment(&mUsbContext,&mUsbDevice);
#endif
  {EFI_STATUS Close=PianoOwnedSmmuClose(&mUsbContext);if(EFI_ERROR(Close))Status=Close;}
Exit:
  while(Held){--Held;Clock->DisableClock(Clock,Ids[Held]);}
  if(DomainHeld)Clock->DisableClockPowerDomain(Clock,Domain);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_END %r\n",Status));return Status;
}
