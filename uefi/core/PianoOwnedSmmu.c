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
#include <Library/PrintLib.h>

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
STATIC UINT32 mDiagnosticSequence;
STATIC UINT32 DiagnosticCrc(CONST CHAR8 *Body,UINTN Bytes) {
  UINT32 C=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I){C^=(UINT8)Body[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xEDB88320U:0);}
  return ~C;
}
STATIC VOID DiagnosticEmit(CONST CHAR8 *Body,UINTN Bytes) {
  // Fixed BaseDebugLibSerialPort formats at most 255 bytes per call. Include
  // the mirror prefix/CRC/newline in that budget; never seal a truncated line.
  if(Bytes>=191)return;
  UINT32 Crc=DiagnosticCrc(Body,Bytes);
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_DIAG %a crc32=%08x\n",Body,Crc));
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_DIAG_COPY %a crc32=%08x\n",Body,Crc));
}
STATIC VOID RememberSlot(PIANO_OWNED_SMMU *C,UINT32 Index) {
  if(Index>=C->Before.Groups || Index>=ARRAY_SIZE(C->Before.RawSmr))return;
  for(UINTN I=0;I<C->BaselineSlotCount;++I)if(C->BaselineSlots[I]==Index)return;
  if(C->BaselineSlotCount<ARRAY_SIZE(C->BaselineSlots))C->BaselineSlots[C->BaselineSlotCount++]=(UINT16)Index;
}
STATIC VOID RememberBaseline(PIANO_OWNED_SMMU *C) {
  if(!C->Before.Valid)return;
  if(!C->BaselineSaved) {
    C->BaselineSaved=TRUE;C->DiagnosticOwnedSlot=MAX_UINT16;
    RememberSlot(C,0);RememberSlot(C,1);RememberSlot(C,113);
  }
  if(C->DeviceIndex>=PIANO_SMMU_DEVICE_COUNT)return;
  UINTN Peer=C->DeviceIndex==0?1:0;
  if(C->Before.Device[Peer].Present)RememberSlot(C,C->Before.Device[Peer].StreamIndex);
  if(C->After.Valid && C->After.Device[C->DeviceIndex].Present) {
    C->DiagnosticOwnedSlot=C->After.Device[C->DeviceIndex].StreamIndex;
    RememberSlot(C,C->DiagnosticOwnedSlot);
  }
  if(C->After.Valid && C->After.Device[Peer].Present)RememberSlot(C,C->After.Device[Peer].StreamIndex);
}
STATIC VOID BaselineReport(CONST PIANO_OWNED_SMMU *C,CONST CHAR8 *Phase) {
  if(!C->BaselineSaved || !DebugPrintEnabled() || !DebugPrintLevelEnabled(DEBUG_WARN))return;
  CHAR8 Body[192];UINTN Bytes;
  for(UINTN I=0;I<C->BaselineSlotCount;++I) {
    UINT32 Slot=C->BaselineSlots[I];
    Bytes=AsciiSPrint(Body,sizeof(Body),
      "phase=%a seq=%u owner=%u idx=%u before_valid=%u groups=%u base=%lx window=%lx smr=%08x s2cr=%08x owned_idx=%u",
      Phase,mDiagnosticSequence++,C->DeviceIndex,Slot,C->Before.Valid,C->Before.Groups,(UINT64)C->Before.Base,
      (UINT64)C->Before.Window,C->Before.RawSmr[Slot],C->Before.RawS2cr[Slot],C->DiagnosticOwnedSlot);
    DiagnosticEmit(Body,Bytes);
  }
}
VOID PianoOwnedSmmuReport(CONST PIANO_OWNED_SMMU *C) {
  if(C==NULL || !DebugPrintEnabled() || !DebugPrintLevelEnabled(DEBUG_WARN))return;
  BaselineReport(C,"baseline-final");
  CHAR8 Body[192];UINTN Bytes;
  if(C->DeviceIndex<2) {
    UINTN Peer=C->DeviceIndex==0?1:0;
    CONST PIANO_SMMU_DEVICE *B=&C->Before.Device[Peer],*A=&C->After.Device[Peer];
    Bytes=AsciiSPrint(Body,sizeof(Body),
      "phase=peer-before seq=%u owner=%u expected_sid=%x valid=%u present=%u idx=%u sid=%x smr=%08x s2cr=%08x",
      mDiagnosticSequence++,C->DeviceIndex,Peer==1?0x40:0x60,C->Before.Valid,
      B->Present,B->Present?B->StreamIndex:MAX_UINT16,B->Sid,B->Smr,B->S2cr);
    DiagnosticEmit(Body,Bytes);
    Bytes=AsciiSPrint(Body,sizeof(Body),
      "phase=peer-final seq=%u owner=%u expected_sid=%x valid=%u present=%u idx=%u sid=%x smr=%08x s2cr=%08x type=%u cb=%u identity_proven=%u",
      mDiagnosticSequence++,C->DeviceIndex,Peer==1?0x40:0x60,C->After.Valid,
      A->Present,A->Present?A->StreamIndex:MAX_UINT16,A->Sid,A->Smr,A->S2cr,A->Type,A->ContextBank,C->After.Valid && A->Present);
    DiagnosticEmit(Body,Bytes);
  }
  CONST PIANO_SMMU_CLOSE_DIAGNOSTIC *D=&C->CloseDiagnostic;
  if(!D->Valid)return;
  UINT32 Group=mDiagnosticSequence;
  Bytes=AsciiSPrint(Body,sizeof(Body),
    "phase=close-rejected seq=%u owner=%u reason=%a idx=%u before_valid=%u after_valid=%u capture=%lx strict=1 retained=1",
    mDiagnosticSequence++,C->DeviceIndex,D->Reason,D->Index,C->Before.Valid,C->After.Valid,(UINT64)D->CaptureStatus);
  DiagnosticEmit(Body,Bytes);
  Bytes=AsciiSPrint(Body,sizeof(Body),"phase=close-before seq=%u group=%u owner=%u idx=%u smr=%08x s2cr=%08x",
    mDiagnosticSequence++,Group,C->DeviceIndex,D->Index,D->BeforeSmr,D->BeforeS2cr);DiagnosticEmit(Body,Bytes);
  Bytes=AsciiSPrint(Body,sizeof(Body),"phase=close-after seq=%u group=%u owner=%u idx=%u smr=%08x s2cr=%08x",
    mDiagnosticSequence++,Group,C->DeviceIndex,D->Index,D->AfterSmr,D->AfterS2cr);DiagnosticEmit(Body,Bytes);
  for(UINTN I=0;I<2;++I) {
    Bytes=AsciiSPrint(Body,sizeof(Body),"phase=close-live seq=%u group=%u owner=%u idx=%u sample=%u live_read=%u smr=%08x s2cr=%08x",
      mDiagnosticSequence++,Group,C->DeviceIndex,D->Index,(UINT32)I+1,D->LiveRead,D->LiveSmr[I],D->LiveS2cr[I]);DiagnosticEmit(Body,Bytes);
  }
}
STATIC EFI_STATUS CloseRejected(PIANO_OWNED_SMMU *C,CONST CHAR8 *Reason,UINT32 Index,EFI_STATUS Capture) {
  C->CloseLedger.Uncertain=TRUE;
  PIANO_SMMU_CLOSE_DIAGNOSTIC *D=&C->CloseDiagnostic;
  *D=(PIANO_SMMU_CLOSE_DIAGNOSTIC){.Valid=TRUE,.Reason=Reason,.Index=Index,.CaptureStatus=Capture};
  if(Index<ARRAY_SIZE(C->Before.RawSmr)) {
    D->BeforeSmr=C->Before.RawSmr[Index];D->BeforeS2cr=C->Before.RawS2cr[Index];
    D->AfterSmr=C->After.RawSmr[Index];D->AfterS2cr=C->After.RawS2cr[Index];
  }
  // Only the validated, exact apps-SMMU window permits these additional reads.
  // Neither stable nor corrected live values relax the snapshot rejection.
  if(C->Before.Valid && C->Before.Base==0x15000000U && C->Before.Window==0x100000U &&
     Index<C->Before.Groups && Index<ARRAY_SIZE(C->Before.RawSmr)) {
    D->LiveRead=TRUE;
    for(UINTN I=0;I<2;++I) {
      MemoryFence();D->LiveSmr[I]=MmioRead32(C->Before.Base+0x800+4*Index);
      D->LiveS2cr[I]=MmioRead32(C->Before.Base+0xC00+4*Index);MemoryFence();
    }
  }
  PianoOwnedSmmuReport(C);return EFI_COMPROMISED_DATA;
}
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
STATIC VOID ClearOwnedStickyFault(CONST PIANO_OWNED_SMMU *C) {
  CONST PIANO_SMMU_DEVICE *U=&C->After.Device[C->DeviceIndex];
  if(U->Fsr&PIANO_SMMU_FSR_FAULT_MASK) {
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_STICKY_FAULT bank=%u fsr=%08x far=%lx fsynr=%08x\n",U->ContextBank,U->Fsr,U->Far,U->Fsynr));
    UINTN Bank=C->After.Base+C->After.ContextBase+((UINTN)U->ContextBank<<C->After.PageShift);
    MmioWrite32(Bank+0x58,U->Fsr&PIANO_SMMU_FSR_FAULT_MASK);MemoryFence();
  }
}
STATIC EFI_STATUS OpenResource(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D,UINT8 Index) {
  if(C==NULL || D==NULL)return EFI_INVALID_PARAMETER;
  UINT16 Sid=Index==0?0x60:0x40;
  if(Index>1 || D->StreamId!=Sid)return EFI_UNSUPPORTED;
  ZeroMem(C,sizeof(*C));C->Fdt=Fdt;
  C->DeviceIndex=Index;C->ResourceName=Index==0?"UFS_MEM":"USB0";
  EFI_STATUS Status=PianoSmmuCapture(Fdt,"owned-before",&C->Before);
  if(EFI_ERROR(Status))return Status;
  RememberBaseline(C);BaselineReport(C,"baseline-open");
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
  RememberBaseline(C);BaselineReport(C,"baseline-owned");
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
  ClearOwnedStickyFault(C);
  BOOLEAN Reserve=Index==0 || D->ReserveAcrossExit;
  *D=(PIANO_DMA_DEVICE){.Name=Index==0?"ufs":"usb",.StreamId=Sid,.AddressBits=32,.CacheLine=64,.Context=C,.Map=Map,.Unmap=Unmap,.Fault=Fault,.ReserveAcrossExit=Reserve};
  C->OwnedIdentitySaved=TRUE;C->OwnedTablePhysical=C->TableMemory.Physical;C->AttachedSnapshot=C->After;
  C->Verified=TRUE;DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_OWNED_READY sid=%x bank=%u root=%lx other_streams_unchanged=1\n",Sid,U->ContextBank,C->TableMemory.Physical));
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuOpen(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){return OpenResource(Fdt,C,D,0);}
EFI_STATUS PianoOwnedSmmuOpenUsb(CONST VOID *Fdt,PIANO_OWNED_SMMU *C,PIANO_DMA_DEVICE *D){return OpenResource(Fdt,C,D,1);}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C) {
  if(C==NULL)return EFI_INVALID_PARAMETER;
  if(C->ExitRetained || C->TableMemory.Quarantined)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<ARRAY_SIZE(C->Mapping);++I)if(C->Mapping[I].Used)return EFI_ACCESS_DENIED;
  RememberBaseline(C);
  if(C->Attached) {
    C->CloseLedger.DetachAttempted=TRUE;
    UINT32 Native=((ATTACH)Functions(C)[3])(C->Domain,C->ResourceName==NULL?"UFS_MEM":C->ResourceName,
      C->DeviceIndex==1?0x03000000:0,0);
    C->CloseLedger.DetachCode=Native;if(Native)C->CloseLedger.Uncertain=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_DETACH code=%u\n",Native));if(Native){PianoOwnedSmmuReport(C);return EFI_DEVICE_ERROR;}
    C->Attached=FALSE;C->Verified=FALSE;
    C->TableMemory.Quarantined=TRUE;
    UINT16 OwnedSlot=C->After.Device[C->DeviceIndex].StreamIndex;
    // Read back the detached stream before freeing its page tables. A native
    // success code alone must not release memory still reachable by a device.
    C->CloseLedger.DetachedCaptureAttempted=TRUE;
    EFI_STATUS Status=PianoSmmuCapture(C->Fdt,"owned-detached",&C->After);C->CloseLedger.DetachedCaptureStatus=Status;
    if(Status!=EFI_SUCCESS)C->CloseLedger.Uncertain=TRUE;
    if(Status!=EFI_SUCCESS || !C->After.Valid || C->After.Device[C->DeviceIndex].Present)
      return CloseRejected(C,Status!=EFI_SUCCESS || !C->After.Valid?"capture-error":"owned-still-present",OwnedSlot,Status);
    if(C->Before.Groups!=C->After.Groups)return CloseRejected(C,"group-count",OwnedSlot,Status);
    CONST PIANO_SMMU_RETIRED_USB_CONTRACT *Retired=C->RetiredUsbContract;
    if(Retired!=NULL && PianoOwnedSmmuCheckRetiredUsbContract(C,Retired,&C->After)!=EFI_SUCCESS)
      return CloseRejected(C,"peer-contract",Retired->PeerSlot,Status);
    for(UINTN I=0;I<C->Before.Groups;++I) {
      if(I==OwnedSlot)continue;
      if(Retired!=NULL && I==Retired->PeerSlot)continue; // exact retired row already validated; baseline remains untouched
      if(C->Before.RawSmr[I]!=C->After.RawSmr[I] || C->Before.RawS2cr[I]!=C->After.RawS2cr[I])return CloseRejected(C,"other-raw",(UINT32)I,Status);
    }
    if(Retired!=NULL)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_PEER_RETIRE_VERIFIED peer_sid=40 peer_slot=%u original_smr=%08x original_s2cr=%08x retired_smr=0 retired_s2cr=0 baseline_unchanged=1\n",
      Retired->PeerSlot,Retired->BaselineSmr,Retired->BaselineS2cr));
    if(C->DeviceIndex==0)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DETACH_VERIFIED ufs_stream_absent=1 other_streams_unchanged=1 tables_retained_until_verified=1\n"));
    else DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DETACH_VERIFIED usb_stream_absent=1 other_streams_unchanged=1 tables_retained_until_verified=1\n"));
    C->TableMemory.Quarantined=FALSE;
  }
  if(C->Domain) {
    C->CloseLedger.DestroyAttempted=TRUE;
    UINT32 Native=((DESTROY)Functions(C)[1])(C->Domain);
    C->CloseLedger.DestroyCode=Native;if(Native)C->CloseLedger.Uncertain=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_DOMAIN_DESTROY code=%u\n",Native));if(Native){PianoOwnedSmmuReport(C);return EFI_DEVICE_ERROR;}
    C->Domain=NULL;
  }
  C->CloseLedger.TableFreeAttempted=C->TableMemory.Signature!=0;
  EFI_STATUS Status=C->TableMemory.Signature?PianoDmaFree(&C->TableMemory):EFI_SUCCESS;
  C->CloseLedger.TableFreeStatus=Status;if(Status!=EFI_SUCCESS)C->CloseLedger.Uncertain=TRUE;
  C->CloseLedger.ExactClose=Status==EFI_SUCCESS && !C->CloseLedger.Uncertain;
  PianoOwnedSmmuReport(C);return Status;
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

// Default-unwired retirement contract. None of these pure helpers is called
// by Open/Close, and none may normalize the original baseline or hardware.
STATIC BOOLEAN PeerShape(CONST PIANO_SMMU_SNAPSHOT *S) {
  return S!=NULL && S->Valid && S->Base==0x15000000U && S->Window==0x100000U &&
    S->Groups && S->Groups<=ARRAY_SIZE(S->RawSmr) && S->Banks && S->Banks<=256 &&
    (S->PageShift==12 || S->PageShift==16) && S->ContextBase<=S->Window &&
    ((UINT64)S->Banks<<S->PageShift)<=S->Window-S->ContextBase;
}
STATIC BOOLEAN PeerGeometry(CONST PIANO_SMMU_SNAPSHOT *A,CONST PIANO_SMMU_SNAPSHOT *B) {
  return PeerShape(A) && PeerShape(B) && A->Base==B->Base && A->Window==B->Window && A->ContextBase==B->ContextBase &&
    A->PageShift==B->PageShift && A->Groups==B->Groups && A->Banks==B->Banks && A->ExtendedIds==B->ExtendedIds &&
    A->Id0==B->Id0 && A->Id1==B->Id1 && A->Id2==B->Id2 && A->GlobalControl==B->GlobalControl && A->GlobalFault==B->GlobalFault;
}
STATIC BOOLEAN PeerDevice(CONST PIANO_SMMU_DEVICE *A,CONST PIANO_SMMU_DEVICE *B) {
  // Name is a diagnostic string, not a physical identity or routing register.
  return A->Sid==B->Sid && A->Mask==B->Mask && A->StreamIndex==B->StreamIndex && A->Type==B->Type &&
    A->ContextBank==B->ContextBank && A->Present==B->Present && A->Enabled==B->Enabled && A->Smr==B->Smr && A->S2cr==B->S2cr &&
    A->Sctlr==B->Sctlr && A->Cbar==B->Cbar && A->Cba2r==B->Cba2r && A->Tcr==B->Tcr && A->Tcr2==B->Tcr2 &&
    A->Mair0==B->Mair0 && A->Mair1==B->Mair1 && A->Fsr==B->Fsr && A->Fsynr==B->Fsynr &&
    A->Ttbr0==B->Ttbr0 && A->Ttbr1==B->Ttbr1 && A->Far==B->Far;
}
STATIC BOOLEAN PeerMatches(UINT32 Smr,UINT32 S2cr,BOOLEAN Extended,UINT16 Sid) {
  BOOLEAN Valid=Extended?(S2cr&BIT10)!=0:(Smr&BIT31)!=0;
  UINT16 Mask=(UINT16)((Smr>>16)&0x7fff);return Valid && ((Sid^(UINT16)Smr)&~Mask)==0;
}
STATIC BOOLEAN PeerAbsent(CONST PIANO_SMMU_SNAPSHOT *S,UINTN Index,UINT16 Sid) {
  if(S->Device[Index].Present)return FALSE;
  for(UINTN I=0;I<S->Groups;++I)if(PeerMatches(S->RawSmr[I],S->RawS2cr[I],S->ExtendedIds,Sid))return FALSE;
  return TRUE;
}
STATIC BOOLEAN PeerIdentity(CONST PIANO_SMMU_SNAPSHOT *S,UINTN Index,UINT16 Sid,UINT64 Root) {
  CONST PIANO_SMMU_DEVICE *D=&S->Device[Index];UINTN Matches=0;
  if(!PeerShape(S) || !D->Present || !D->Enabled || D->Sid!=Sid || D->Mask!=0 || D->Type!=0 ||
     D->StreamIndex>=S->Groups || D->ContextBank>=S->Banks || !(D->Sctlr&1) || !(D->Cba2r&1) ||
     !Root || (Root&4095) || (D->Ttbr0&0x0000fffffffff000ULL)!=Root ||
     D->Smr!=S->RawSmr[D->StreamIndex] || D->S2cr!=S->RawS2cr[D->StreamIndex] ||
     ((D->Smr>>16)&0x7fff)!=0 || (UINT16)D->Smr!=Sid || ((D->S2cr>>16)&3)!=0 || (UINT8)D->S2cr!=D->ContextBank)return FALSE;
  for(UINTN I=0;I<S->Groups;++I)if(PeerMatches(S->RawSmr[I],S->RawS2cr[I],S->ExtendedIds,Sid))++Matches;
  return Matches==1 && PeerMatches(D->Smr,D->S2cr,S->ExtendedIds,Sid);
}
STATIC BOOLEAN PeerRows(CONST PIANO_SMMU_SNAPSHOT *A,CONST PIANO_SMMU_SNAPSHOT *B,UINT16 Skip,UINT16 Own) {
  if(!PeerGeometry(A,B))return FALSE;
  for(UINTN I=0;I<A->Groups;++I)if(I!=Skip && I!=Own && (A->RawSmr[I]!=B->RawSmr[I] || A->RawS2cr[I]!=B->RawS2cr[I]))return FALSE;
  for(UINTN I=2;I<PIANO_SMMU_DEVICE_COUNT;++I)if(!PeerDevice(&A->Device[I],&B->Device[I]))return FALSE;
  return TRUE;
}
STATIC BOOLEAN PeerSnapshot(CONST PIANO_SMMU_SNAPSHOT *A,CONST PIANO_SMMU_SNAPSHOT *B) {
  if(!PeerRows(A,B,MAX_UINT16,MAX_UINT16))return FALSE;
  return PeerDevice(&A->Device[0],&B->Device[0]) && PeerDevice(&A->Device[1],&B->Device[1]);
}
STATIC EFI_STATUS PeerRetired(CONST PIANO_OWNED_SMMU *U,CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *E,CONST PIANO_SMMU_SNAPSHOT *Now) {
  if(U==NULL || E==NULL || Now==NULL)return EFI_INVALID_PARAMETER;
  BOOLEAN BuffersRetired=E->Kind==PIANO_USB_RETIRE_RUNNING?E->DmaBuffersFreed==9:
    E->Kind==PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN && E->StartupStatus==EFI_TIMEOUT &&
    E->DmaBuffersAllocated==0 && E->DmaBuffersFreed==0;
  if(E->Revision!=PIANO_SMMU_USB_RETIRE_REVISION || E->DeviceCleanupStatus!=EFI_SUCCESS || E->ControllerCleanupStatus!=EFI_SUCCESS ||
     E->DeviceHalted!=TRUE || E->DmaFreed!=TRUE || E->ClocksReleased!=TRUE || E->GdscReleased!=TRUE ||
     !BuffersRetired || E->ClockReleaseMask!=0xff || U->DeviceIndex!=1 || !U->OwnedIdentitySaved ||
     U->Attached || U->Verified || U->ExitRetained || U->Domain!=NULL || U->TableMemory.Signature || U->TableMemory.Quarantined ||
     U->TableMemory.Active || U->TableMemory.Mapped || U->TableMemory.Cpu!=NULL || U->TableMemory.AllocationPages)return EFI_NOT_READY;
  CONST PIANO_SMMU_CLOSE_LEDGER *L=&U->CloseLedger;
  if(!L->ExactClose || L->Uncertain || !L->DetachAttempted || L->DetachCode!=0 || !L->DetachedCaptureAttempted ||
     L->DetachedCaptureStatus!=EFI_SUCCESS || !L->DestroyAttempted || L->DestroyCode!=0 ||
     !L->TableFreeAttempted || L->TableFreeStatus!=EFI_SUCCESS)return EFI_NOT_READY;
  for(UINTN I=0;I<ARRAY_SIZE(U->Mapping);++I)if(U->Mapping[I].Used)return EFI_NOT_READY;
  for(UINTN I=0;I<ARRAY_SIZE(U->Used);++I)if(U->Used[I])return EFI_NOT_READY;
  if(!PeerIdentity(&U->AttachedSnapshot,1,0x40,U->OwnedTablePhysical))return EFI_COMPROMISED_DATA;
  UINT16 Slot=U->AttachedSnapshot.Device[1].StreamIndex;
  if(!PeerRows(&U->Before,&U->AttachedSnapshot,Slot,MAX_UINT16) || !PeerRows(&U->Before,&U->After,Slot,MAX_UINT16) ||
     !PeerRows(&U->Before,Now,Slot,MAX_UINT16) || !PeerAbsent(&U->Before,1,0x40) || !PeerAbsent(&U->After,1,0x40) ||
     !PeerAbsent(Now,1,0x40) || (U->Before.RawSmr[Slot]&BIT31) || (U->Before.RawS2cr[Slot]&BIT10) ||
     U->After.RawSmr[Slot]!=0 || U->After.RawS2cr[Slot]!=0 || Now->RawSmr[Slot]!=0 || Now->RawS2cr[Slot]!=0)return EFI_COMPROMISED_DATA;
  CONST PIANO_SMMU_DEVICE *F=&U->Before.Device[0];
  if(!PeerIdentity(&U->Before,0,0x60,F->Ttbr0&0x0000fffffffff000ULL) ||
     !PeerDevice(F,&U->AttachedSnapshot.Device[0]) || !PeerDevice(F,&U->After.Device[0]) || !PeerDevice(F,&Now->Device[0]) ||
     F->ContextBank==U->AttachedSnapshot.Device[1].ContextBank ||
     (F->Ttbr0&0x0000fffffffff000ULL)==U->OwnedTablePhysical)return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuMakeRetiredUsbProof(CONST PIANO_OWNED_SMMU *Usb,CONST PIANO_SMMU_USB_RETIRE_EVIDENCE *Execution,
  CONST PIANO_SMMU_SNAPSHOT *Current,PIANO_SMMU_RETIRED_USB_PROOF *Proof) {
  if(Proof==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Proof,sizeof(*Proof));
  EFI_STATUS S=PeerRetired(Usb,Execution,Current);if(S!=EFI_SUCCESS)return S;
  Proof->Revision=PIANO_SMMU_USB_RETIRE_REVISION;Proof->UsbContext=Usb;Proof->Execution=*Execution;
  Proof->Slot=Usb->AttachedSnapshot.Device[1].StreamIndex;Proof->ContextBank=Usb->AttachedSnapshot.Device[1].ContextBank;
  Proof->TablePhysical=Usb->OwnedTablePhysical;Proof->UsbBaseline=Usb->Before;Proof->Attached=Usb->AttachedSnapshot;Proof->Retired=*Current;
  Proof->Valid=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuPrepareRetiredUsbContract(CONST PIANO_OWNED_SMMU *Ufs,CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof,
  CONST PIANO_SMMU_SNAPSHOT *Current,PIANO_SMMU_RETIRED_USB_CONTRACT *Contract) {
  if(Contract==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Contract,sizeof(*Contract));
  if(Ufs==NULL || Proof==NULL || Current==NULL)return EFI_INVALID_PARAMETER;
  if(!Proof->Valid || Proof->Revision!=PIANO_SMMU_USB_RETIRE_REVISION || Proof->UsbContext==NULL)return EFI_NOT_READY;
  CONST PIANO_OWNED_SMMU *Usb=Proof->UsbContext;EFI_STATUS S=PeerRetired(Usb,&Proof->Execution,Current);if(S!=EFI_SUCCESS)return S;
  if(Proof->Slot!=Usb->AttachedSnapshot.Device[1].StreamIndex || Proof->ContextBank!=Usb->AttachedSnapshot.Device[1].ContextBank ||
     Proof->TablePhysical!=Usb->OwnedTablePhysical || !PeerSnapshot(&Proof->UsbBaseline,&Usb->Before) ||
     !PeerSnapshot(&Proof->Attached,&Usb->AttachedSnapshot) || !PeerSnapshot(&Proof->Retired,Current))return EFI_COMPROMISED_DATA;
  if(Ufs->DeviceIndex!=0 || !Ufs->Attached || !Ufs->Verified || Ufs->ExitRetained || !Ufs->OwnedIdentitySaved ||
     Ufs->Domain==NULL || !Ufs->TableMemory.Signature || Ufs->TableMemory.Quarantined || Ufs->CloseLedger.Uncertain)return EFI_NOT_READY;
  if(!PeerIdentity(&Ufs->AttachedSnapshot,0,0x60,Ufs->OwnedTablePhysical) ||
     !PeerDevice(&Ufs->AttachedSnapshot.Device[0],&Current->Device[0]) || !PeerDevice(&Ufs->After.Device[0],&Current->Device[0]) ||
     Ufs->AttachedSnapshot.Device[0].StreamIndex==Proof->Slot ||
     !PeerRows(&Ufs->Before,Current,Proof->Slot,Ufs->AttachedSnapshot.Device[0].StreamIndex) ||
     !PeerAbsent(&Ufs->Before,1,0x40) || Ufs->Before.RawSmr[Proof->Slot]!=Proof->UsbBaseline.RawSmr[Proof->Slot] ||
     Ufs->Before.RawS2cr[Proof->Slot]!=Proof->UsbBaseline.RawS2cr[Proof->Slot])return EFI_COMPROMISED_DATA;
  Contract->Revision=PIANO_SMMU_USB_RETIRE_REVISION;Contract->Owner=Ufs;Contract->Peer=Usb;Contract->PeerSlot=Proof->Slot;
  Contract->OwnerSlot=Ufs->AttachedSnapshot.Device[0].StreamIndex;Contract->BaselineSmr=Ufs->Before.RawSmr[Proof->Slot];
  Contract->BaselineS2cr=Ufs->Before.RawS2cr[Proof->Slot];Contract->PeerTablePhysical=Proof->TablePhysical;
  Contract->OwnerIdentity=Current->Device[0];Contract->OwnerBaseline=Ufs->Before;Contract->Authorized=*Current;
  Contract->Valid=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuCheckRetiredUsbContract(CONST PIANO_OWNED_SMMU *Ufs,CONST PIANO_SMMU_RETIRED_USB_CONTRACT *Contract,
  CONST PIANO_SMMU_SNAPSHOT *Current) {
  if(Ufs==NULL || Contract==NULL || Current==NULL)return EFI_INVALID_PARAMETER;
  if(!Contract->Valid || Contract->Revision!=PIANO_SMMU_USB_RETIRE_REVISION || Contract->Owner!=Ufs ||
     Ufs->DeviceIndex!=0 || Ufs->ExitRetained || Ufs->CloseLedger.Uncertain || !Ufs->OwnedIdentitySaved)return EFI_NOT_READY;
  CONST PIANO_OWNED_SMMU *Peer=Contract->Peer;
  if(Peer==NULL || Peer->DeviceIndex!=1 || !Peer->OwnedIdentitySaved || !Peer->CloseLedger.ExactClose || Peer->CloseLedger.Uncertain ||
     Peer->Attached || Peer->Verified || Peer->ExitRetained || Peer->Domain!=NULL || Peer->TableMemory.Signature || Peer->TableMemory.Quarantined ||
     Contract->PeerSlot!=Peer->AttachedSnapshot.Device[1].StreamIndex || Contract->PeerTablePhysical!=Peer->OwnedTablePhysical ||
     !PeerIdentity(&Peer->AttachedSnapshot,1,0x40,Peer->OwnedTablePhysical))return EFI_COMPROMISED_DATA;
  if(!PeerSnapshot(&Contract->OwnerBaseline,&Ufs->Before) || Contract->PeerSlot>=Ufs->Before.Groups ||
     Contract->OwnerSlot>=Ufs->Before.Groups || Contract->PeerSlot==Contract->OwnerSlot ||
     Contract->OwnerSlot!=Ufs->AttachedSnapshot.Device[0].StreamIndex ||
     !PeerIdentity(&Ufs->AttachedSnapshot,0,0x60,Ufs->OwnedTablePhysical) ||
     !PeerDevice(&Contract->OwnerIdentity,&Ufs->AttachedSnapshot.Device[0]) ||
     Contract->BaselineSmr!=Ufs->Before.RawSmr[Contract->PeerSlot] || Contract->BaselineS2cr!=Ufs->Before.RawS2cr[Contract->PeerSlot] ||
     Contract->RetiredSmr!=0 || Contract->RetiredS2cr!=0 ||
     !PeerRows(&Contract->Authorized,Current,Contract->PeerSlot,Contract->OwnerSlot) ||
     !PeerAbsent(Current,0,0x60) || !PeerAbsent(Current,1,0x40) || Current->RawSmr[Contract->PeerSlot]!=0 || Current->RawS2cr[Contract->PeerSlot]!=0)
    return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
