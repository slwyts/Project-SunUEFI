// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB0 xHCI host session behind PianoUsbHostPci
// Host register values: stock UsbConfigDxe Usb30InitHost (RVA 0x6b84), minus
// its sticky PORTSC wake bits. GCTL.DSBLCLKGTNG/GUCTL.HSTINAUTORETRY follow
// Linux dwc3 for this DT; the VBUS override is cleared as in dwc3-qcom
// DWC31 >= 1.90a switches role without GCTL.CORESOFTRESET (Linux dwc3 core)
// Streaming DMA goes through a once-mapped arena so the xHCI timer TPL never
// touches the SMMU. IOVAs are below 4 GiB, so the DWC3 high-then-low 64-bit
// write quirk cannot tear a pointer
#include <PiDxe.h>
#include "PianoUsbHost.h"
#include "PianoUsbHostPci.h"
#include "PianoOwnedSmmu.h"
#include <Protocol/DevicePath.h>
#include <Protocol/Usb2HostController.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define DWC 0x0A600000U
#define GCTL 0xC110U
#define DCTL 0xC704U
#define DSTS 0xC70CU
#define HS_VBUS_OVERRIDE (BIT20|BIT28) // QSCRATCH UTMI_OTG_VBUS_VALID|SW_SESSVLD_SEL
#define SS_VBUS_OVERRIDE BIT24         // QSCRATCH LANE0_PWR_PRESENT
#define RECORDS PIANO_USB_HOST_MAX_RECORDS
#define ARENA_PAGES 256U
// Restored in reverse order
STATIC CONST UINTN mRegs[]={DWC+0xF8810,DWC+0xF8830,DWC+0xC200,DWC+0xC2C0,DWC+0xC104,DWC+0xC10C,DWC+0xC11C,DWC+0xC60C,DWC+0xC12C,DWC+GCTL};
enum {R_HS,R_SS,R_GUSB2,R_PIPE,R_SBUS1,R_RXTHR,R_GUCTL1,R_GUCTL3,R_GUCTL,R_GCTL};
typedef struct {BOOLEAN Used,Lost;EFI_PHYSICAL_ADDRESS Base;UINTN Pages,Live;} HOST_ALLOCATION;
typedef struct {
  BOOLEAN Used,Common;PIANO_DMA_DIRECTION Direction;VOID *Smmu,*Cpu;
  UINTN First,Span,Offset,Bytes;
} HOST_MAPPING;
STATIC struct {
  BOOLEAN Active,Programmed,Published,PciReady,Retained,Lost;
  PIANO_DMA_DEVICE *Device;
  EFI_HANDLE Handle;
  UINT32 OpBase,Saved[ARRAY_SIZE(mRegs)];
  PIANO_USB_HOST_PCI Pci;
  HOST_ALLOCATION Allocation[RECORDS];
  HOST_MAPPING Mapping[RECORDS];
  struct {EFI_PHYSICAL_ADDRESS Base,Iova;VOID *Smmu;BOOLEAN Taken;UINT8 Used[ARENA_PAGES];} Arena;
} mHost;
STATIC struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} mPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0x9F2C3A61,0x5B7E,0x4C1D,{0xA8,0x3E,0x60,0x0A,0x60,0x00,0x00,0x40}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}};

STATIC UINT32 Dr(UINT32 Offset){return MmioRead32(DWC+Offset);}
STATIC BOOLEAN AtApp(VOID) {
  if(gBS==NULL || gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL)return FALSE;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);return Old==TPL_APPLICATION;
}
STATIC BOOLEAN XhcHalted(VOID) {
  return mHost.OpBase && (Dr(mHost.OpBase+4)&BIT0) && !(Dr(mHost.OpBase)&BIT0);
}
// MMIO and CPU timer only: also runs in the ExitBootServices fence
STATIC EFI_STATUS HaltXhc(VOID) {
  if(!mHost.OpBase)return EFI_NOT_READY;
  UINT32 Command=Dr(mHost.OpBase);
  if(Command&BIT0){MmioWrite32(DWC+mHost.OpBase,Command&~BIT0);MemoryFence();}
  for(UINTN Us=0;Us<100000;Us+=10){if(XhcHalted())return EFI_SUCCESS;MicroSecondDelay(10);}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS ResetXhc(VOID) {
  if(!XhcHalted())return EFI_NOT_READY;
  MmioWrite32(DWC+mHost.OpBase,Dr(mHost.OpBase)|BIT1);MemoryFence();
  for(UINTN Us=0;Us<1000000;Us+=100) {
    if(!(Dr(mHost.OpBase)&BIT1) && !(Dr(mHost.OpBase+4)&BIT11))return EFI_SUCCESS;
    MicroSecondDelay(100);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS SetCache(EFI_PHYSICAL_ADDRESS Base,UINTN Bytes,UINT64 Cache) {
  EFI_PHYSICAL_ADDRESS At=Base,End=Base+Bytes;
  while(At<End) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=gDS->GetMemorySpaceDescriptor(At,&D);
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
    if(D.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory || !(D.Capabilities&Cache) || !D.Length ||
       D.BaseAddress>At || D.Length>MAX_UINT64-D.BaseAddress)return EFI_UNSUPPORTED;
    EFI_PHYSICAL_ADDRESS Stop=MIN(End,D.BaseAddress+D.Length);
    S=gDS->SetMemorySpaceAttributes(At,Stop-At,(D.Attributes&~(UINT64)EFI_CACHE_ATTRIBUTE_MASK)|Cache);
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
    At=Stop;
  }
  return EFI_SUCCESS;
}
// The SMMU maps PA, so every page must be identity-mapped RAM
STATIC EFI_STATUS TakePages(UINTN Count,EFI_PHYSICAL_ADDRESS *Base) {
  EFI_PHYSICAL_ADDRESS Address=MAX_UINT32;
  EFI_STATUS S=gBS->AllocatePages(AllocateMaxAddress,EfiBootServicesData,Count,&Address);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  for(UINTN I=0;I<Count;++I) {
    EFI_PHYSICAL_ADDRESS Expected=Address+EFI_PAGES_TO_SIZE(I),Pa=0;
    if(PianoDmaPhysicalAddress((VOID *)(UINTN)Expected,&Pa)!=EFI_SUCCESS || Pa!=Expected) {
      gBS->FreePages(Address,Count);return EFI_UNSUPPORTED;
    }
  }
  *Base=Address;return EFI_SUCCESS;
}
STATIC BOOLEAN SmmuLive(VOID) {
  PIANO_OWNED_SMMU *C=mHost.Device==NULL?NULL:mHost.Device->Context;
  return C!=NULL && C->Attached && C->Verified && !C->ExitRetained;
}
STATIC EFI_STATUS Ready(VOID *Context) {
  if(Context!=&mHost || mHost.Lost || mHost.Retained || !mHost.Programmed || !SmmuLive())return EFI_NOT_READY;
  return ((Dr(GCTL)>>12)&3)==1?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS Read32(VOID *Context,UINT32 Offset,UINT32 *Value) {
  if(Context!=&mHost || Value==NULL || Offset>=PIANO_USB_HOST_MMIO_BYTES || (Offset&3))return EFI_INVALID_PARAMETER;
  *Value=Dr(Offset);MemoryFence();return EFI_SUCCESS;
}
// The leading barrier publishes ring writes before a doorbell
STATIC EFI_STATUS Write32(VOID *Context,UINT32 Offset,UINT32 Value) {
  if(Context!=&mHost || Offset>=PIANO_USB_HOST_MMIO_BYTES || (Offset&3))return EFI_INVALID_PARAMETER;
  MemoryFence();MmioWrite32(DWC+Offset,Value);MemoryFence();return EFI_SUCCESS;
}
STATIC EFI_STATUS SetBusMaster(VOID *Context,BOOLEAN Enable) {
  if(Context!=&mHost)return EFI_INVALID_PARAMETER;
  return Enable?Ready(Context):HaltXhc();
}
STATIC EFI_STATUS Quiesced(VOID *Context) {
  if(Context!=&mHost)return EFI_INVALID_PARAMETER;
  return XhcHalted()?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS Stall(VOID *Context,UINTN Microseconds) {
  if(Context!=&mHost)return EFI_INVALID_PARAMETER;
  MicroSecondDelay(Microseconds);return EFI_SUCCESS;
}
STATIC EFI_STATUS Flush(VOID *Context) {
  if(Context!=&mHost)return EFI_INVALID_PARAMETER;
  MemoryFence();return EFI_SUCCESS;
}
STATIC EFI_STATUS Allocate(VOID *Context,UINTN Count,UINT64 Requested,VOID **Cpu,UINT64 *Attributes,VOID **Token) {
  if(Context!=&mHost || !Count || Count>PIANO_USB_HOST_DMA_BUDGET/4096 || Cpu==NULL || Attributes==NULL || Token==NULL)return EFI_INVALID_PARAMETER;
  if(Requested)return EFI_UNSUPPORTED;
  if(Ready(Context)!=EFI_SUCCESS)return EFI_NOT_READY;
  UINTN Slot=0;while(Slot<RECORDS && mHost.Allocation[Slot].Used)++Slot;
  if(Slot==RECORDS)return EFI_OUT_OF_RESOURCES;
  EFI_PHYSICAL_ADDRESS Base=0;UINTN Bytes=EFI_PAGES_TO_SIZE(Count);
  EFI_STATUS S=TakePages(Count,&Base);if(S!=EFI_SUCCESS)return S;
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)Base,Bytes);MemoryFence();
  S=SetCache(Base,Bytes,EFI_MEMORY_WC);
  if(S!=EFI_SUCCESS) {
    EFI_STATUS Back=SetCache(Base,Bytes,EFI_MEMORY_WB);
    if(Back==EFI_SUCCESS){gBS->FreePages(Base,Count);return S;}
    // Unknown cache attributes: never return these pages to the allocator
    mHost.Allocation[Slot]=(HOST_ALLOCATION){.Used=TRUE,.Lost=TRUE,.Base=Base,.Pages=Count,.Live=Count};
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_DMA_RETAINED base=%lx pages=%lu set=%r restore=%r\n",Base,(UINT64)Count,S,Back));
    *Cpu=(VOID *)(UINTN)Base;*Token=&mHost.Allocation[Slot];return S;
  }
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=gDS->GetMemorySpaceDescriptor(Base,&D);
  if(S==EFI_SUCCESS && (D.Attributes&EFI_CACHE_ATTRIBUTE_MASK)!=EFI_MEMORY_WC)S=EFI_DEVICE_ERROR;
  mHost.Allocation[Slot]=(HOST_ALLOCATION){.Used=TRUE,.Base=Base,.Pages=Count,.Live=Count};
  if(S!=EFI_SUCCESS){mHost.Allocation[Slot].Lost=TRUE;*Cpu=(VOID *)(UINTN)Base;*Token=&mHost.Allocation[Slot];return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  ZeroMem((VOID *)(UINTN)Base,Bytes);
  *Cpu=(VOID *)(UINTN)Base;*Attributes=D.Attributes;*Token=&mHost.Allocation[Slot];return EFI_SUCCESS;
}
STATIC HOST_ALLOCATION *AllocationRecord(VOID *Token) {
  for(UINTN I=0;I<RECORDS;++I)if(Token==&mHost.Allocation[I] && mHost.Allocation[I].Used)return &mHost.Allocation[I];
  return NULL;
}
STATIC EFI_STATUS Free(VOID *Context,VOID *Token,VOID *Cpu,UINTN Count) {
  HOST_ALLOCATION *A=AllocationRecord(Token);EFI_PHYSICAL_ADDRESS At=(UINTN)Cpu;
  if(Context!=&mHost || A==NULL || Cpu==NULL || !Count || (At&4095))return EFI_INVALID_PARAMETER;
  if(A->Lost || mHost.Lost)return EFI_ACCESS_DENIED;
  if(At<A->Base || Count>A->Live || EFI_PAGES_TO_SIZE(Count)>A->Base+EFI_PAGES_TO_SIZE(A->Pages)-At)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=SetCache(At,EFI_PAGES_TO_SIZE(Count),EFI_MEMORY_WB);
  if(S!=EFI_SUCCESS){A->Lost=TRUE;return S;}
  S=gBS->FreePages(At,Count);if(S!=EFI_SUCCESS){A->Lost=TRUE;return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  A->Live-=Count;if(!A->Live)ZeroMem(A,sizeof(*A));
  return EFI_SUCCESS;
}
STATIC BOOLEAN InsideAllocation(VOID *Cpu,UINTN Bytes) {
  EFI_PHYSICAL_ADDRESS At=(UINTN)Cpu;
  for(UINTN I=0;I<RECORDS;++I) {
    HOST_ALLOCATION *A=&mHost.Allocation[I];
    if(A->Used && !A->Lost && At>=A->Base && Bytes<=EFI_PAGES_TO_SIZE(A->Pages) && At-A->Base<=EFI_PAGES_TO_SIZE(A->Pages)-Bytes)return TRUE;
  }
  return FALSE;
}
STATIC EFI_STATUS Map(VOID *Context,PIANO_DMA_DIRECTION Direction,BOOLEAN Common,VOID *Cpu,UINTN *Bytes,
                      EFI_PHYSICAL_ADDRESS *Iova,VOID **Token) {
  if(Context!=&mHost || Cpu==NULL || Bytes==NULL || !*Bytes || Iova==NULL || Token==NULL || Direction>PianoDmaBidirectional ||
     (Common && Direction!=PianoDmaBidirectional) || (!Common && Direction==PianoDmaBidirectional))return EFI_INVALID_PARAMETER;
  if(Ready(Context)!=EFI_SUCCESS)return EFI_NOT_READY;
  UINTN Length=*Bytes,Offset=(UINTN)Cpu&4095;
  if(Length>PIANO_USB_HOST_DMA_BUDGET)return EFI_OUT_OF_RESOURCES;
  UINTN Span=ALIGN_VALUE(Offset+Length,4096),Slot=0;
  while(Slot<RECORDS && mHost.Mapping[Slot].Used)++Slot;
  if(Slot==RECORDS)return EFI_OUT_OF_RESOURCES;
  HOST_MAPPING *M=&mHost.Mapping[Slot];
  if(!Common) {
    // The PCI core requires the IOVA page offset to match the caller's
    UINTN Pages=Span/4096,First=0,Run=0;
    if(!mHost.Arena.Smmu || Pages>ARENA_PAGES)return EFI_OUT_OF_RESOURCES;
    for(UINTN I=0;I<ARENA_PAGES && Run<Pages;++I){if(mHost.Arena.Used[I])Run=0;else if(!Run++)First=I;}
    if(Run<Pages)return EFI_OUT_OF_RESOURCES;
    UINT8 *Bounce=(UINT8 *)(UINTN)(mHost.Arena.Base+EFI_PAGES_TO_SIZE(First));
    if(Direction==PianoDmaToDevice){CopyMem(Bounce+Offset,Cpu,Length);WriteBackDataCacheRange(Bounce,Span);}
    else WriteBackInvalidateDataCacheRange(Bounce,Span);
    MemoryFence();SetMem(mHost.Arena.Used+First,Pages,1);
    *M=(HOST_MAPPING){.Used=TRUE,.Direction=Direction,.Cpu=Cpu,.First=First,.Span=Span,.Offset=Offset,.Bytes=Length};
    *Iova=mHost.Arena.Iova+EFI_PAGES_TO_SIZE(First)+Offset;*Token=M;return EFI_SUCCESS;
  }
  if(!InsideAllocation(Cpu,Length))return EFI_INVALID_PARAMETER;
  EFI_PHYSICAL_ADDRESS Base=0;VOID *Smmu=NULL;
  EFI_STATUS S=mHost.Device->Map(mHost.Device,(UINTN)Cpu-Offset,Span,PianoDmaBidirectional,4096,&Base,&Smmu);
  if(S!=EFI_SUCCESS && Smmu==NULL)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  // An unconfirmed rollback keeps its token for quarantine
  *M=(HOST_MAPPING){.Used=TRUE,.Common=TRUE,.Direction=PianoDmaBidirectional,.Smmu=Smmu,.Cpu=Cpu,.Span=Span,.Offset=Offset,.Bytes=Length};
  *Iova=Base+Offset;*Token=M;
  return S==EFI_SUCCESS?EFI_SUCCESS:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS Retire(VOID *Context,VOID *Token,BOOLEAN CopyBack) {
  HOST_MAPPING *M=NULL;
  for(UINTN I=0;I<RECORDS;++I)if(Token==&mHost.Mapping[I] && mHost.Mapping[I].Used)M=&mHost.Mapping[I];
  if(Context!=&mHost || M==NULL)return EFI_INVALID_PARAMETER;
  if(mHost.Lost)return EFI_ACCESS_DENIED;
  if(M->Common) {
    EFI_STATUS S=mHost.Device->Unmap(mHost.Device,M->Smmu);
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  } else {
    UINT8 *Bounce=(UINT8 *)(UINTN)(mHost.Arena.Base+EFI_PAGES_TO_SIZE(M->First));
    if(CopyBack && M->Direction==PianoDmaFromDevice) {
      InvalidateDataCacheRange(Bounce,M->Span);MemoryFence();
      CopyMem(M->Cpu,Bounce+M->Offset,M->Bytes);
    }
    SetMem(mHost.Arena.Used+M->First,M->Span/4096,0);
  }
  ZeroMem(M,sizeof(*M));return EFI_SUCCESS;
}
STATIC EFI_STATUS ArenaOpen(VOID) {
  EFI_PHYSICAL_ADDRESS Base=0,Iova=0;VOID *Smmu=NULL;
  EFI_STATUS S=TakePages(ARENA_PAGES,&Base);if(S!=EFI_SUCCESS)return S;
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)Base,EFI_PAGES_TO_SIZE(ARENA_PAGES));MemoryFence();
  mHost.Arena.Base=Base;mHost.Arena.Taken=TRUE;
  S=mHost.Device->Map(mHost.Device,Base,EFI_PAGES_TO_SIZE(ARENA_PAGES),PianoDmaBidirectional,4096,&Iova,&Smmu);
  mHost.Arena.Smmu=Smmu;mHost.Arena.Iova=Iova;
  if(S==EFI_SUCCESS)return S;
  if(Smmu==NULL){gBS->FreePages(Base,ARENA_PAGES);mHost.Arena.Taken=FALSE;}
  return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS ArenaClose(VOID) {
  if(!mHost.Arena.Taken)return EFI_SUCCESS;
  for(UINTN I=0;I<ARENA_PAGES;++I)if(mHost.Arena.Used[I])return EFI_ACCESS_DENIED;
  if(mHost.Arena.Smmu!=NULL) {
    EFI_STATUS S=mHost.Device->Unmap(mHost.Device,mHost.Arena.Smmu);
    if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
    mHost.Arena.Smmu=NULL;
  }
  EFI_STATUS S=gBS->FreePages(mHost.Arena.Base,ARENA_PAGES);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  ZeroMem(&mHost.Arena,sizeof(mHost.Arena));return EFI_SUCCESS;
}
STATIC CONST PIANO_USB_HOST_PCI_BACKEND mBackend={
  .Context=&mHost,
  .Capabilities=PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED|PIANO_USB_HOST_PARTIAL_FREE,
  .Ready=Ready,.Read32=Read32,.Write32=Write32,.SetBusMaster=SetBusMaster,.Quiesced=Quiesced,
  .Stall=Stall,.Flush=Flush,.Allocate=Allocate,.Free=Free,.Map=Map,.Retire=Retire};
STATIC BOOLEAN RecordsEmpty(VOID) {
  for(UINTN I=0;I<RECORDS;++I)if(mHost.Allocation[I].Used || mHost.Mapping[I].Used)return FALSE;
  return TRUE;
}
STATIC BOOLEAN PciEmpty(VOID) {
  return !mHost.PciReady || (!mHost.Pci.AllocatedBytes && !mHost.Pci.MappedBytes && !mHost.Pci.Busy &&
    !(mHost.Pci.Attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) && !PianoUsbHostPciHasQuarantine(&mHost.Pci));
}
STATIC EFI_STATUS Program(VOID) {
  UINT32 Value[ARRAY_SIZE(mRegs)];CopyMem(Value,mHost.Saved,sizeof(Value));
  Value[R_HS]&=~(UINT32)HS_VBUS_OVERRIDE;Value[R_SS]&=~(UINT32)SS_VBUS_OVERRIDE;
  Value[R_GUSB2]&=~(UINT32)(BIT6|BIT8);Value[R_PIPE]&=~(UINT32)BIT17; // PHY suspend off (snps,dis_u2_susphy_quirk)
  Value[R_SBUS1]|=0x1E00;Value[R_RXTHR]|=0x04630000;Value[R_GUCTL1]|=0x600000;Value[R_GUCTL3]|=BIT16;Value[R_GUCTL]|=BIT14;
  Value[R_GCTL]=(Value[R_GCTL]&~(3U<<12))|(1U<<12)|BIT2|BIT0;
  mHost.Programmed=TRUE;
  BOOLEAN Exact=TRUE;
  for(UINTN I=0;I<ARRAY_SIZE(mRegs);++I) {
    MmioWrite32(mRegs[I],Value[I]);MemoryFence();UINT32 Now=MmioRead32(mRegs[I]);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_REG addr=%08x saved=%08x host=%08x read=%08x\n",(UINT32)mRegs[I],mHost.Saved[I],Value[I],Now));
    // Only the role, PHY suspend and session bits must stick; tuning bits are logged
    if((I==R_GCTL && ((Now>>12)&3)!=1) || (I==R_HS && (Now&HS_VBUS_OVERRIDE)) || (I==R_SS && (Now&SS_VBUS_OVERRIDE)) ||
       (I==R_GUSB2 && (Now&(BIT6|BIT8))) || (I==R_PIPE && (Now&BIT17)))Exact=FALSE;
  }
  if(!Exact)return EFI_DEVICE_ERROR;
  EFI_STATUS S=gBS->Stall(10000);return S==EFI_SUCCESS?S:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS Restore(VOID) {
  BOOLEAN Exact=TRUE;
  for(UINTN I=ARRAY_SIZE(mRegs);I>0;--I){MmioWrite32(mRegs[I-1],mHost.Saved[I-1]);MemoryFence();}
  for(UINTN I=0;I<ARRAY_SIZE(mRegs);++I)if(MmioRead32(mRegs[I])!=mHost.Saved[I]) {
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_RESTORE_MISMATCH addr=%08x expected=%08x read=%08x\n",(UINT32)mRegs[I],mHost.Saved[I],MmioRead32(mRegs[I])));
    Exact=FALSE;
  }
  if(!Exact)return EFI_DEVICE_ERROR;
  // PHY stop and SMMU close require DSTS.DEVCTRLHLT
  for(UINTN Us=0;Us<100000;Us+=100){if(!(Dr(DCTL)&BIT31) && (Dr(DSTS)&BIT22))return EFI_SUCCESS;MicroSecondDelay(100);}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Inspect(VOID) {
  UINT32 Cap=Dr(0x0),Hcs1=Dr(0x4),Hcs2=Dr(0x8),Hcc1=Dr(0x10),Db=Dr(0x14)&~3U,Rt=Dr(0x18)&~0x1FU;
  UINT32 CapLength=Cap&0xFF,Version=Cap>>16,Ports=Hcs1>>24,Slots=Hcs1&0xFF,Intrs=(Hcs1>>8)&0x7FF,Ext=(Hcc1>>16)<<2;
  UINT32 Scratch=(((Hcs2>>21)&0x1F)<<5)|((Hcs2>>27)&0x1F);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_CAPS gsnpsid=%08x ver=%08x caplength=%02x hciversion=%04x hcs1=%08x hcs2=%08x hcc1=%08x dboff=%x rtsoff=%x scratch=%u\n",
    Dr(0xC120),Dr(0xC1A0),CapLength,Version,Hcs1,Hcs2,Hcc1,Db,Rt,Scratch));
  // XhciDxe registers must fit the 32 KiB BAR; scratchpads must fit 64 mappings
  if(CapLength<0x20 || CapLength>0x80 || (CapLength&3) || (Version!=0x0100 && Version!=0x0110 && Version!=0x0120) ||
     !Ports || Ports>15 || !Slots || !Intrs || Scratch>32 || !Db || Db+4*(Slots+1)>PIANO_USB_HOST_MMIO_BYTES ||
     !Rt || Rt+0x20+0x20*Intrs>PIANO_USB_HOST_MMIO_BYTES ||
     CapLength+0x400+0x10*Ports>PIANO_USB_HOST_MMIO_BYTES || !Ext || Ext>=PIANO_USB_HOST_MMIO_BYTES)return EFI_UNSUPPORTED;
  UINT32 Offset=Ext,Count=0;
  for(;Count<32;++Count) {
    UINT32 Next=(Dr(Offset)>>8)&0xFF;if(!Next)break;
    Offset+=Next<<2;if(Offset>=PIANO_USB_HOST_MMIO_BYTES-4)return EFI_UNSUPPORTED;
  }
  if(Count==32)return EFI_UNSUPPORTED;
  mHost.OpBase=CapLength;
  if(!(Dr(CapLength+8)&BIT0))return EFI_UNSUPPORTED; // 4 KiB xHC pages only
  return XhcHalted()?EFI_SUCCESS:HaltXhc();
}
// XhciDxe ASSERTs on allocation failure, so prove the DMA path first
STATIC EFI_STATUS SelfTest(VOID) {
  VOID *Cpu=NULL,*Owner=NULL,*Token=NULL;UINT64 Attributes=0;UINTN Bytes=4096;EFI_PHYSICAL_ADDRESS Iova=0,Pa=0;
  EFI_STATUS S=Allocate(&mHost,1,0,&Cpu,&Attributes,&Owner);
  if(S==EFI_SUCCESS)S=Map(&mHost,PianoDmaBidirectional,TRUE,Cpu,&Bytes,&Iova,&Token);
  PIANO_OWNED_SMMU *C=mHost.Device->Context;
  if(S==EFI_SUCCESS)S=PianoIoPageTableTranslate(&C->PageTable,Iova,TRUE,&Pa);
  if(S==EFI_SUCCESS && Pa!=(UINTN)Cpu)S=EFI_COMPROMISED_DATA;
  if(Token!=NULL){EFI_STATUS R=Retire(&mHost,Token,FALSE);if(S==EFI_SUCCESS)S=R;}
  if(Owner!=NULL){EFI_STATUS F=Free(&mHost,Owner,Cpu,1);if(S==EFI_SUCCESS)S=F;}
  UINT8 Probe[64];SetMem(Probe,sizeof(Probe),0x5A);Bytes=sizeof(Probe);Token=NULL;
  if(S==EFI_SUCCESS)S=Map(&mHost,PianoDmaFromDevice,FALSE,Probe,&Bytes,&Iova,&Token);
  if(Token!=NULL){EFI_STATUS R=Retire(&mHost,Token,FALSE);if(S==EFI_SUCCESS)S=R;}
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_DMA_SELFTEST status=%r attributes=%lx iova=%lx records_empty=%u\n",S,Attributes,Iova,RecordsEmpty()));
  return S==EFI_SUCCESS && !RecordsEmpty()?EFI_DEVICE_ERROR:S;
}
STATIC EFI_STATUS Publish(VOID) {
  mHost.Handle=NULL;
  EFI_STATUS S=gBS->InstallMultipleProtocolInterfaces(&mHost.Handle,&gEfiPciIoProtocolGuid,&mHost.Pci.Pci,
    &gEfiDevicePathProtocolGuid,&mPath,NULL);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  mHost.Published=TRUE;
  EFI_STATUS Connect=gBS->ConnectController(mHost.Handle,NULL,NULL,TRUE);
  VOID *Hc=NULL;S=gBS->HandleProtocol(mHost.Handle,&gEfiUsb2HcProtocolGuid,&Hc);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_CONNECT connect=%r usb2hc=%r usbsts=%08x portsc1=%08x portsc2=%08x\n",
    Connect,S,Dr(mHost.OpBase+4),Dr(mHost.OpBase+0x400),Dr(mHost.OpBase+0x410)));
  return S==EFI_SUCCESS?S:EFI_ERROR(Connect)?Connect:EFI_NOT_FOUND;
}
STATIC EFI_STATUS Teardown(VOID) {
  EFI_STATUS S;
  if(mHost.Published) {
    EFI_STATUS Disconnect=gBS->DisconnectController(mHost.Handle,NULL,NULL);VOID *Hc=NULL;
    if(gBS->HandleProtocol(mHost.Handle,&gEfiUsb2HcProtocolGuid,&Hc)==EFI_SUCCESS) {
      // XhciDxe stops all-or-nothing: USB2_HC still installed means nothing was freed
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_BUSY disconnect=%r consumers_bound=1\n",Disconnect));
      return EFI_NOT_READY;
    }
    if(!PciEmpty() && HaltXhc()==EFI_SUCCESS)(VOID)PianoUsbHostPciRecover(&mHost.Pci);
    if(!PciEmpty()) {
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_RETAINED stage=pci allocated=%lu mapped=%lu attributes=%lx halted=%u\n",
        (UINT64)mHost.Pci.AllocatedBytes,(UINT64)mHost.Pci.MappedBytes,mHost.Pci.Attributes,XhcHalted()));
      mHost.Retained=TRUE;return EFI_DEVICE_ERROR;
    }
    S=gBS->UninstallMultipleProtocolInterfaces(mHost.Handle,&gEfiPciIoProtocolGuid,&mHost.Pci.Pci,
      &gEfiDevicePathProtocolGuid,&mPath,NULL);
    if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_RETAINED stage=uninstall status=%r\n",S));mHost.Retained=TRUE;return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
    mHost.Published=FALSE;mHost.Handle=NULL;
  }
  // The arena stays mapped until the xHC is proven halted
  S=mHost.Programmed && mHost.OpBase?HaltXhc():EFI_SUCCESS;
  if(S==EFI_SUCCESS)S=ArenaClose();
  if(S!=EFI_SUCCESS || !RecordsEmpty()) {
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_RETAINED stage=dma status=%r halted=%u\n",S,XhcHalted()));
    mHost.Retained=TRUE;return S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;
  }
  if(mHost.Programmed) {
    S=mHost.OpBase?ResetXhc():EFI_SUCCESS;
    if(S==EFI_SUCCESS)S=Restore();
    if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_RETAINED stage=controller status=%r usbcmd=%08x usbsts=%08x\n",
      S,mHost.OpBase?Dr(mHost.OpBase):0,mHost.OpBase?Dr(mHost.OpBase+4):0));mHost.Retained=TRUE;return S;}
    mHost.Programmed=FALSE;
  }
  mHost.Active=FALSE;mHost.PciReady=FALSE;return EFI_SUCCESS;
}
EFI_STATUS PianoUsbHostStart(PIANO_DMA_DEVICE *Device) {
  if(mHost.Retained || mHost.Lost)return EFI_ACCESS_DENIED;
  if(mHost.Active || mHost.Programmed)return EFI_ALREADY_STARTED;
  if(!AtApp())return EFI_UNSUPPORTED;
  PIANO_OWNED_SMMU *Smmu=Device==NULL?NULL:Device->Context;
  if(Device==NULL || Device->StreamId!=0x40 || Device->Map==NULL || Device->Unmap==NULL || Smmu==NULL ||
     !Smmu->Attached || !Smmu->Verified || Smmu->ExitRetained)return EFI_NOT_READY;
  if((Dr(DCTL)&BIT31) || !(Dr(DSTS)&BIT22))return EFI_NOT_READY;
  ZeroMem(&mHost,sizeof(mHost));mHost.Device=Device;
  for(UINTN I=0;I<ARRAY_SIZE(mRegs);++I)mHost.Saved[I]=MmioRead32(mRegs[I]);
  EFI_STATUS S=Program();
  if(S==EFI_SUCCESS)S=Inspect();
  if(S==EFI_SUCCESS)S=ArenaOpen();
  if(S==EFI_SUCCESS)S=SelfTest();
  if(S==EFI_SUCCESS){S=PianoUsbHostPciInit(&mHost.Pci,&mBackend,NULL);mHost.PciReady=S==EFI_SUCCESS;}
  if(S==EFI_SUCCESS){mHost.Active=TRUE;S=Publish();}
  if(S==EFI_SUCCESS){DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_STARTED readonly_storage=1 vbus_control=0\n"));return S;}
  EFI_STATUS Undo=Teardown();
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_START_FAILED status=%r undo=%r active=%u retained=%u\n",S,Undo,PianoUsbHostActive(),mHost.Retained));
  return S;
}
EFI_STATUS PianoUsbHostStop(VOID) {
  if(mHost.Lost || mHost.Retained)return EFI_ACCESS_DENIED;
  if(!mHost.Active && !mHost.Programmed)return EFI_SUCCESS;
  if(!AtApp())return EFI_UNSUPPORTED;
  EFI_STATUS S=Teardown();
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_HOST_STOP status=%r retained=%u\n",S,mHost.Retained));
  return S;
}
BOOLEAN PianoUsbHostActive(VOID){return mHost.Active || mHost.Programmed;}
BOOLEAN PianoUsbHostRetained(VOID){return mHost.Retained;}
EFI_STATUS PianoUsbHostFenceExit(VOID) {
  mHost.Lost=TRUE;
  if(!mHost.Programmed)return EFI_SUCCESS;
  mHost.Retained=TRUE; // pages stay allocated until the OS resets USB0
  return mHost.OpBase?HaltXhc():EFI_SUCCESS;
}
