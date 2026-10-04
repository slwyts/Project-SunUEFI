// SPDX-License-Identifier: BSD-2-Clause-Patent
// Narrow adapter to the captured SM8750 HAL IOMMU ABI. Owns only UFS_MEM's
// domain, root tables and mappings. Never resets/detaches all SMMU contexts.
#include "PianoOwnedSmmu.h"
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

typedef UINT32 (*CREATE)(VOID **);
typedef UINT32 (*DESTROY)(VOID *);
typedef UINT32 (*ATTACH)(VOID *,CONST CHAR8 *,UINT32,UINT32);
typedef UINT32 (*CONFIGURE)(VOID *,CONST VOID *);
typedef UINT32 (*SYNC)(VOID *);
// Native SetDomainConfig copies these fields verbatim; verified using the
// individual register-writing helpers and test41's readback. CBA2R is set by
// native Attach, not a field in this configuration structure.
typedef struct {UINT32 Type,Pad;UINT64 Ttbr0,Ttbr1;UINT32 Mair0,Mair1,Sctlr,Tcr,Tcr2,Pad2;} DOMAIN_CONFIG;
typedef struct {UINT64 Revision;VOID (*GetApi)(VOID **);VOID (*Reserved)(VOID);} HAL_PROTOCOL;
STATIC EFI_GUID mGuid={0x54B6D3B4,0x5D33,0x4F91,{0x86,0,0x6C,0x41,0xD5,0xDE,0xB1,0x9A}};
STATIC PIANO_OWNED_SMMU mExperiment;
STATIC PIANO_DMA_DEVICE mDevice;
STATIC UINTN *Functions(PIANO_OWNED_SMMU *C){return C->Api;}
STATIC EFI_STATUS Validate(HAL_PROTOCOL *P,VOID **Api) {
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status))return Status;
  Status=EFI_SECURITY_VIOLATION;
  for(UINTN I=0;I<Count;++I) {
    EFI_LOADED_IMAGE_PROTOCOL *L=NULL;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&L)))continue;
    UINTN Base=(UINTN)L->ImageBase;
    if((UINTN)P!=Base+0xB0B8 || L->ImageSize<0xB148)continue;
    if(P->Revision!=0x10002 || (UINTN)P->GetApi!=Base+0x152C)break;
    P->GetApi(Api);
    STATIC CONST UINTN Rvas[]={0x1644,0x16A0,0x1744,0x1D38,0x1FB8,0x2154,0x2198,0x21F4,0x20F4,0x21FC,0x2C8C,0x2D24,0x2B08,0x2C1C,0x1F58};
    if((UINTN)*Api!=Base+0xB0D0)break;
    BOOLEAN Good=TRUE;UINTN *F=*Api;
    for(UINTN J=0;J<ARRAY_SIZE(Rvas);++J)if(F[J]!=Base+Rvas[J])Good=FALSE;
    if(Good){Status=EFI_SUCCESS;DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_HAL_ABI_OK base=%lx\n",Base));}
    break;
  }
  FreePool(Handles);return Status;
}
STATIC VOID Fault(PIANO_DMA_DEVICE *D){PIANO_OWNED_SMMU *C=D->Context;PianoSmmuLogFaults(&C->After);}
STATIC EFI_STATUS Map(PIANO_DMA_DEVICE *D,EFI_PHYSICAL_ADDRESS Pa,UINTN Bytes,
                     PIANO_DMA_DIRECTION Direction,UINTN Alignment,EFI_PHYSICAL_ADDRESS *Iova,VOID **Token) {
  if(D==NULL || Iova==NULL || Token==NULL || Direction>PianoDmaBidirectional ||
     Alignment<4096 || (Alignment&(Alignment-1)) || !Bytes || Bytes>PIANO_IOVA_BYTES ||
     (Bytes&4095) || (Pa&4095))return EFI_INVALID_PARAMETER;
  // The owned CB's TCR2.PASIZE is 36 bits. Device address width and CPU PA
  // width are separate; reject addresses the SMMU cannot actually translate.
  if(Pa>0xFFFFFFFFFULL || Bytes-1>0xFFFFFFFFFULL-Pa)return EFI_BAD_BUFFER_SIZE;
  PIANO_OWNED_SMMU *C=D->Context;
  if(C==NULL || !C->Verified || !C->Attached || C->ExitRetained)return EFI_NOT_READY;
  UINT32 Pages=(UINT32)(Bytes/4096),First=0,Run=0;
  for(UINT32 I=0;I<ARRAY_SIZE(C->Used);++I) {
    if(C->Used[I])Run=0;
    else if(Run==0 && ((PIANO_IOVA_BASE+(UINT64)I*4096)&(Alignment-1))==0)Run=1;
    else if(Run)++Run;
    if(Run==Pages){First=I+1-Pages;break;}
  }
  if(Run<Pages)return EFI_OUT_OF_RESOURCES;
  UINTN Slot=0;while(Slot<ARRAY_SIZE(C->Mapping) && C->Mapping[Slot].Used)++Slot;
  if(Slot==ARRAY_SIZE(C->Mapping))return EFI_OUT_OF_RESOURCES;
  UINT64 Address=PIANO_IOVA_BASE+(UINT64)First*4096;
  EFI_STATUS Status=PianoIoPageTableMap(&C->PageTable,Address,Pa,Bytes,Direction);
  if(EFI_ERROR(Status))return Status;
  WriteBackDataCacheRange(C->TableMemory.Cpu,C->TableMemory.ReservedBytes);MemoryFence();
  UINT32 Native=((SYNC)Functions(C)[5])(C->Domain);
  if(Native!=0) {
    PianoIoPageTableUnmap(&C->PageTable,Address,Bytes);
    WriteBackDataCacheRange(C->TableMemory.Cpu,C->TableMemory.ReservedBytes);MemoryFence();
    if(((SYNC)Functions(C)[5])(C->Domain)==0)return EFI_DEVICE_ERROR;
    // Cannot confirm rollback/TLB completion: retain the mapping token so the
    // shared DMA layer quarantines the physical pages until a cold reset.
    for(UINT32 I=0;I<Pages;++I)C->Used[First+I]=1;
    C->Mapping[Slot].Used=TRUE;C->Mapping[Slot].First=First;C->Mapping[Slot].Pages=Pages;
    *Iova=Address;*Token=&C->Mapping[Slot];return EFI_DEVICE_ERROR;
  }
  for(UINT32 I=0;I<Pages;++I)C->Used[First+I]=1;
  C->Mapping[Slot].Used=TRUE;C->Mapping[Slot].First=First;C->Mapping[Slot].Pages=Pages;
  *Iova=Address;*Token=&C->Mapping[Slot];
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_MAP sid=%x pa=%lx iova=%lx bytes=%lu root=%lx config_verified=1 hardware_dma_verified=0\n",D->StreamId,Pa,Address,(UINT64)Bytes,C->TableMemory.Physical));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Unmap(PIANO_DMA_DEVICE *D,VOID *Token) {
  PIANO_OWNED_SMMU *C=D->Context;
  if(C==NULL)return EFI_INVALID_PARAMETER;
  if(C->ExitRetained)return EFI_ACCESS_DENIED;
  UINTN Slot=0;while(Slot<ARRAY_SIZE(C->Mapping) && Token!=&C->Mapping[Slot])++Slot;
  if(Slot==ARRAY_SIZE(C->Mapping) || !C->Mapping[Slot].Used)return EFI_INVALID_PARAMETER;
  UINT32 First=C->Mapping[Slot].First,Pages=C->Mapping[Slot].Pages;
  EFI_STATUS Status=PianoIoPageTableUnmap(&C->PageTable,PIANO_IOVA_BASE+(UINT64)First*4096,Pages*4096);
  if(EFI_ERROR(Status))return Status;
  WriteBackDataCacheRange(C->TableMemory.Cpu,C->TableMemory.ReservedBytes);MemoryFence();
  if(((SYNC)Functions(C)[5])(C->Domain)!=0)return EFI_DEVICE_ERROR;
  for(UINT32 I=0;I<Pages;++I)C->Used[First+I]=0;
  C->Mapping[Slot].Used=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS OpenResource(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D,UINT8 Index) {
  if(C==NULL || D==NULL)return EFI_INVALID_PARAMETER;
  UINT16 Sid=Index==0?0x60:0x40;
  if(Index>1 || D->StreamId!=Sid)return EFI_UNSUPPORTED;
  ZeroMem(C,sizeof(*C));C->Fdt=Fdt;
  C->DeviceIndex=Index;C->ResourceName=Index==0?"UFS_MEM":"USB0";
  EFI_STATUS Status=PianoSmmuCapture(Fdt,"owned-before",&C->Before);
  if(EFI_ERROR(Status))return Status;
  if(C->Before.Device[Index].Present)return EFI_ALREADY_STARTED;
  // Quiescence is checked before attaching a new UFS context. No disk command.
  if(Index==0 && (MmioRead32(0x1D84058) || MmioRead32(0x1D84078)))return EFI_NOT_READY;
  if(Index==1 && ((MmioRead32(0xA60C704)&BIT31) || !(MmioRead32(0xA60C70C)&BIT22)))return EFI_NOT_READY;
  HAL_PROTOCOL *Protocol=NULL;Status=gBS->LocateProtocol(&mGuid,NULL,(VOID **)&Protocol);
  if(EFI_ERROR(Status))return Status;
  Status=Validate(Protocol,&C->Api);if(EFI_ERROR(Status))return Status;
  PIANO_DMA_DEVICE Tables={.Name="smmu-tables",.StreamId=Sid,.AddressBits=64,.CacheLine=64,.ReserveAcrossExit=TRUE};
  Status=PianoDmaAllocate(&Tables,PIANO_IO_PT_BYTES,4096,32,PianoDmaToDevice,&C->TableMemory);
  if(EFI_ERROR(Status))return Status;
  // The table-memory device description must have driver lifetime.
  C->TableMemory.Device=D;
  Status=PianoIoPageTableInit(&C->PageTable,C->TableMemory.Cpu,C->TableMemory.Physical,C->TableMemory.ReservedBytes);
  if(EFI_ERROR(Status))return Status;
  UINT32 Native=((CREATE)Functions(C)[0])(&C->Domain);
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_CREATE code=%u\n",Native));if(Native)return EFI_DEVICE_ERROR;
  DOMAIN_CONFIG Config={.Type=0,.Ttbr0=C->TableMemory.Physical,.Mair0=0xFF,
    .Sctlr=0x000001E5,.Tcr=0x00802519,.Tcr2=0x00038001};
  WriteBackDataCacheRange(C->TableMemory.Cpu,C->TableMemory.ReservedBytes);MemoryFence();
  Native=((CONFIGURE)Functions(C)[4])(C->Domain,&Config);
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_CONFIG code=%u root=%lx\n",Native,Config.Ttbr0));if(Native)return EFI_DEVICE_ERROR;
  // Captured UsbConfigDxe uses USB0 and attach argument 0x03000000;
  // detachment uses zero. Reject any resulting SID/context mismatch below.
  Native=((ATTACH)Functions(C)[2])(C->Domain,C->ResourceName,Index==0?0:0x03000000,0);
  // Treat even a partially failed attach conservatively: detach must succeed
  // before table memory may be released.
  C->Attached=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_ATTACH name=%a code=%u\n",C->ResourceName,Native));if(Native)return EFI_DEVICE_ERROR;
  Status=PianoSmmuCapture(Fdt,"owned-after",&C->After);if(EFI_ERROR(Status))return Status;
  PIANO_SMMU_DEVICE *U=&C->After.Device[Index];
  if(!U->Present || U->Type!=0 || !U->Enabled || !(U->Cba2r&1) ||
     (U->Ttbr0&0x0000FFFFFFFFF000ULL)!=C->TableMemory.Physical || U->Tcr!=Config.Tcr ||
     (U->Tcr2&0x0003800FU)!=Config.Tcr2 || U->Mair0!=Config.Mair0 || U->Sctlr!=Config.Sctlr)
    return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<C->Before.Groups;++I) {
    if(I==U->StreamIndex)continue;
    if(C->Before.RawSmr[I]!=C->After.RawSmr[I] || C->Before.RawS2cr[I]!=C->After.RawS2cr[I])return EFI_COMPROMISED_DATA;
  }
  // Log before clearing sticky faults, and clear only the bank this newly
  // created UFS domain owns. TTBCR2 bits 5/6 read as RES1 on this hardware.
  if(U->Fsr) {
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_STICKY_FAULT bank=%u fsr=%08x far=%lx fsynr=%08x\n",U->ContextBank,U->Fsr,U->Far,U->Fsynr));
    UINTN Bank=C->After.Base+C->After.ContextBase+((UINTN)U->ContextBank<<C->After.PageShift);
    MmioWrite32(Bank+0x58,U->Fsr);MemoryFence();
  }
  BOOLEAN Reserve=Index==0 || D->ReserveAcrossExit;
  *D=(PIANO_DMA_DEVICE){.Name=Index==0?"ufs":"usb",.StreamId=Sid,.AddressBits=32,.CacheLine=64,.Context=C,.Map=Map,.Unmap=Unmap,.Fault=Fault,.ReserveAcrossExit=Reserve};
  C->Verified=TRUE;DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_READY sid=%x bank=%u root=%lx other_streams_unchanged=1\n",Sid,U->ContextBank,C->TableMemory.Physical));
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuOpen(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){return OpenResource(Fdt,C,D,0);}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){return OpenResource(Fdt,C,D,1);}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C) {
  if(C==NULL)return EFI_INVALID_PARAMETER;
  if(C->ExitRetained || C->TableMemory.Quarantined)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<ARRAY_SIZE(C->Mapping);++I)if(C->Mapping[I].Used)return EFI_ACCESS_DENIED;
  if(C->Attached) {
    UINT32 Native=((ATTACH)Functions(C)[3])(C->Domain,C->ResourceName==NULL?"UFS_MEM":C->ResourceName,
      C->DeviceIndex==1?0x03000000:0,0);
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_DETACH code=%u\n",Native));if(Native)return EFI_DEVICE_ERROR;
    C->Attached=FALSE;C->Verified=FALSE;
    C->TableMemory.Quarantined=TRUE;
    UINT16 OwnedSlot=C->After.Device[C->DeviceIndex].StreamIndex;
    // Read back the detached stream before freeing its page tables. A native
    // success code alone must not release memory still reachable by a device.
    EFI_STATUS Status=PianoSmmuCapture(C->Fdt,"owned-detached",&C->After);
    if(EFI_ERROR(Status) || C->After.Device[C->DeviceIndex].Present)return EFI_COMPROMISED_DATA;
    if(C->Before.Groups!=C->After.Groups)return EFI_COMPROMISED_DATA;
    for(UINTN I=0;I<C->Before.Groups;++I) {
      if(I==OwnedSlot)continue;
      if(C->Before.RawSmr[I]!=C->After.RawSmr[I] || C->Before.RawS2cr[I]!=C->After.RawS2cr[I])return EFI_COMPROMISED_DATA;
    }
    if(C->DeviceIndex==0)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DETACH_VERIFIED ufs_stream_absent=1 other_streams_unchanged=1 tables_retained_until_verified=1\n"));
    else DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DETACH_VERIFIED usb_stream_absent=1 other_streams_unchanged=1 tables_retained_until_verified=1\n"));
    C->TableMemory.Quarantined=FALSE;
  }
  if(C->Domain) {
    UINT32 Native=((DESTROY)Functions(C)[1])(C->Domain);
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_DESTROY code=%u\n",Native));if(Native)return EFI_DEVICE_ERROR;
    C->Domain=NULL;
  }
  if(C->TableMemory.Signature)return PianoDmaFree(&C->TableMemory);
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuRetainForExit(PIANO_OWNED_SMMU *C) {
  if(C==NULL)return EFI_INVALID_PARAMETER;
  if(!C->Attached || !C->Verified)return EFI_NOT_READY;
  EFI_STATUS Status=PianoDmaRetainForExit(&C->TableMemory);
  if(EFI_ERROR(Status))return Status;
  C->ExitRetained=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_EXIT_RETAIN sid=%x table_pa=%lx bytes=%lu attached=1 native_hal_called=0\n",
    C->TableMemory.Device->StreamId,C->TableMemory.Physical,(UINT64)C->TableMemory.ReservedBytes));
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuMemoryExperiment(CONST VOID *Fdt) {
  mDevice=(PIANO_DMA_DEVICE){.Name="ufs",.StreamId=0x60,.AddressBits=32,.CacheLine=64};
  EFI_STATUS Status=PianoOwnedSmmuOpen(Fdt,&mExperiment,&mDevice);
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_OPEN %r\n",Status));
  if(!EFI_ERROR(Status)) {
    PIANO_DMA_BUFFER Buffer;
    Status=PianoDmaAllocate(&mDevice,4096,4096,32,PianoDmaBidirectional,&Buffer);
    if(!EFI_ERROR(Status)) {
      Status=PianoDmaMap(&Buffer);
      if(!EFI_ERROR(Status)) {
        UINT64 Pa=0;EFI_STATUS Translate=PianoIoPageTableTranslate(&mExperiment.PageTable,Buffer.DeviceAddress,TRUE,&Pa);
        DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_TRANSLATE_SOFTWARE %r pa=%lx expected=%lx match=%u\n",Translate,Pa,Buffer.Physical,!EFI_ERROR(Translate)&&Pa==Buffer.Physical));
      }
      EFI_STATUS Free=PianoDmaFree(&Buffer);if(EFI_ERROR(Free))Status=Free;
    }
  }
  EFI_STATUS Close=PianoOwnedSmmuClose(&mExperiment);if(EFI_ERROR(Close))Status=Close;
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_MEMORY_EXPERIMENT_END status=%r hardware_dma_submitted=0\n",Status));return Status;
}
