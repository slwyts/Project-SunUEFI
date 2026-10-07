// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared DMA lifecycle. A device-specific verified SMMU backend is mandatory:
// absence of a backend never falls back to an assumed identity DMA mapping.
#include "PianoDma.h"
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>

#define DMA_SIGNATURE SIGNATURE_32('S','D','M','A')
#define MAX_BUFFER (16U*1024U*1024U)
STATIC UINT32 mRecordSequence;
STATIC BOOLEAN PowerOfTwo(UINTN N){return N && !(N&(N-1));}
STATIC EFI_STATUS Heap(EFI_PHYSICAL_ADDRESS *Base,UINT64 *Length) {
  EFI_MEMORY_REGION_DESCRIPTOR *Map;UINT8 Count;GetMemoryMap(&Map,&Count);
  for(UINT8 I=0;I<Count;++I)if(!AsciiStrCmp(Map[I].Name,"DXE_Heap")) {
    if(Map[I].Address>MAX_UINT64-Map[I].Length || Map[I].Length==0)return EFI_COMPROMISED_DATA;
    *Base=Map[I].Address;*Length=Map[I].Length;return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
#ifndef PIANO_DMA_HOST_TEST
EFI_STATUS PianoDmaPhysicalAddress(CONST VOID *Cpu,EFI_PHYSICAL_ADDRESS *Physical) {
  if(Cpu==NULL || Physical==NULL)return EFI_INVALID_PARAMETER;
#ifdef __aarch64__
  UINT64 Par;
  __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(Par):"r"(Cpu):"memory");
  if(Par&1)return EFI_NO_MAPPING;
  *Physical=(Par&0x0000FFFFFFFFF000ULL)|((UINTN)Cpu&0xFFF);return EFI_SUCCESS;
#else
  return EFI_UNSUPPORTED;
#endif
}
#endif
STATIC BOOLEAN Valid(PIANO_DMA_BUFFER *B){return B!=NULL && B->Signature==DMA_SIGNATURE && B->Device!=NULL;}
STATIC EFI_STATUS Translate(CONST PIANO_DMA_BUFFER *B,UINT64 Address,UINTN Bytes,UINT64 Source,UINT64 Target,UINT64 *Out) {
  if(B==NULL || B->Signature!=DMA_SIGNATURE || B->Device==NULL || Out==NULL || !Bytes)return EFI_INVALID_PARAMETER;
  if(!B->Mapped || B->Quarantined)return EFI_NOT_READY;
  if(Address<Source || Address-Source>=B->Bytes || Bytes>B->Bytes-(Address-Source))return EFI_BAD_BUFFER_SIZE;
  if(Target>MAX_UINT64-(Address-Source))return EFI_BAD_BUFFER_SIZE;
  *Out=Target+(Address-Source);return EFI_SUCCESS;
}
EFI_STATUS PianoDmaPhysicalToDevice(CONST PIANO_DMA_BUFFER *B,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes,EFI_PHYSICAL_ADDRESS *Out) {
  if(B==NULL)return EFI_INVALID_PARAMETER;
  return Translate(B,Physical,Bytes,B->Physical,B->DeviceAddress,Out);
}
EFI_STATUS PianoDmaDeviceToPhysical(CONST PIANO_DMA_BUFFER *B,EFI_PHYSICAL_ADDRESS DeviceAddress,UINTN Bytes,EFI_PHYSICAL_ADDRESS *Out) {
  if(B==NULL)return EFI_INVALID_PARAMETER;
  return Translate(B,DeviceAddress,Bytes,B->DeviceAddress,B->Physical,Out);
}
STATIC CONST CHAR8 *Direction(PIANO_DMA_DIRECTION D) {
  return D==PianoDmaToDevice?"to-device":D==PianoDmaFromDevice?"from-device":"bidirectional";
}
STATIC UINT32 RecordCrc(CONST VOID *Data,UINTN Bytes) {
  CONST UINT8 *P=Data;UINT32 C=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I){C^=P[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xEDB88320U:0);}
  return ~C;
}
STATIC VOID Log(PIANO_DMA_BUFFER *B,CONST CHAR8 *Phase,CONST CHAR8 *Cache,EFI_STATUS Status) {
  if(!DebugPrintEnabled() || !DebugPrintLevelEnabled(DEBUG_WARN))return;
  CHAR8 Body[384];UINTN Bytes=AsciiSPrint(Body,sizeof(Body),
    "phase=%a seq=%u dev=%a sid=%x pa=%lx iova=%lx bytes=%lu reserved=%lu dir=%a align=%lu attrs=%lx cache=%a status=%r cmd=%a",
    Phase,mRecordSequence++,B->Device->Name,B->Device->StreamId,B->Physical,B->DeviceAddress,(UINT64)B->Bytes,
    (UINT64)B->ReservedBytes,Direction(B->Direction),(UINT64)B->Alignment,B->MemoryAttributes,Cache,Status,
    B->Command==NULL?"-":B->Command);
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA %a crc32=%08x\n",Body,RecordCrc(Body,Bytes)));
  // Preserve an independently checksummed mirror. The collector selects an
  // intact sequence record; it never edits a damaged pstore capture.
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_COPY %a crc32=%08x\n",Body,RecordCrc(Body,Bytes)));
}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *D,UINTN Bytes,UINTN Align,UINT8 Bits,
                           PIANO_DMA_DIRECTION Dir,PIANO_DMA_BUFFER *B) {
  if(D==NULL || D->Name==NULL || B==NULL || Bytes==0 || Bytes>MAX_BUFFER ||
     !PowerOfTwo(Align) || Align>0x100000 || !PowerOfTwo(D->CacheLine) || D->CacheLine>4096 ||
     (Bits!=32 && Bits!=64) || (D->AddressBits!=32 && D->AddressBits!=64) || Dir>PianoDmaBidirectional)
    return EFI_INVALID_PARAMETER;
  ZeroMem(B,sizeof(*B));Align=MAX(Align,MAX(D->CacheLine,(UINTN)EFI_PAGE_SIZE));
  // Own complete pages for SMMU mappings and complete cache lines for sync.
  UINTN Rounded=EFI_PAGES_TO_SIZE(EFI_SIZE_TO_PAGES(Bytes)),Pages=EFI_SIZE_TO_PAGES(Rounded+Align-1);
  EFI_PHYSICAL_ADDRESS Base,Address,Pa;UINT64 Length;
  EFI_STATUS Status=Heap(&Base,&Length);if(EFI_ERROR(Status))return Status;
  Address=Base+Length-1;if(Bits==32)Address=MIN(Address,(EFI_PHYSICAL_ADDRESS)MAX_UINT32);
  EFI_MEMORY_TYPE Type=D->ReserveAcrossExit?EfiReservedMemoryType:EfiBootServicesData;
  Status=gBS->AllocatePages(AllocateMaxAddress,Type,Pages,&Address);
  if(EFI_ERROR(Status))return Status;
  UINT64 PhysicalLimit=Bits==32?MAX_UINT32:MAX_UINT64;
  if(Address<Base || Address-Base>=Length || EFI_PAGES_TO_SIZE(Pages)>Length-(Address-Base) ||
     Address>PhysicalLimit || EFI_PAGES_TO_SIZE(Pages)-1>PhysicalLimit-Address) {
    gBS->FreePages(Address,Pages);return EFI_ACCESS_DENIED;
  }
  EFI_PHYSICAL_ADDRESS Aligned=ALIGN_VALUE(Address,Align);
  Status=PianoDmaPhysicalAddress((VOID *)(UINTN)Aligned,&Pa);
  if(!EFI_ERROR(Status) && Pa!=Aligned)Status=EFI_UNSUPPORTED;
  // Verify every allocated data page, not just one translated endpoint.
  for(UINTN I=0;!EFI_ERROR(Status) && I<EFI_SIZE_TO_PAGES(Rounded);++I) {
    EFI_PHYSICAL_ADDRESS Next;
    Status=PianoDmaPhysicalAddress((VOID *)(UINTN)(Aligned+EFI_PAGES_TO_SIZE(I)),&Next);
    if(!EFI_ERROR(Status) && Next!=Aligned+EFI_PAGES_TO_SIZE(I))Status=EFI_UNSUPPORTED;
  }
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  if(!EFI_ERROR(Status))Status=gDS->GetMemorySpaceDescriptor(Aligned,&Descriptor);
  if(!EFI_ERROR(Status) && (Descriptor.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory ||
     Aligned<Descriptor.BaseAddress || Aligned-Descriptor.BaseAddress>=Descriptor.Length ||
     Rounded>Descriptor.Length-(Aligned-Descriptor.BaseAddress) ||
     !(Descriptor.Attributes&EFI_MEMORY_WB)))Status=EFI_UNSUPPORTED;
  if(EFI_ERROR(Status)){gBS->FreePages(Address,Pages);return Status;}
  B->Signature=DMA_SIGNATURE;B->Device=D;B->Allocation=Address;B->AllocationPages=Pages;
  B->Physical=Pa;B->Cpu=(VOID *)(UINTN)Aligned;B->Bytes=Bytes;B->ReservedBytes=Rounded;
  B->Alignment=Align;B->Direction=Dir;B->MemoryAttributes=Descriptor.Attributes;
  B->MemoryType=Type;
  if(Type==EfiReservedMemoryType)DEBUG((DEBUG_WARN,"SUNUEFI_DMA_RESERVED dev=%a allocation=%lx pages=%lu efi_map_required=1 raw_dtb_reservation_verified=0\n",
    D->Name,Address,(UINT64)Pages));
  ZeroMem(B->Cpu,Rounded);Log(B,"allocate","none",EFI_SUCCESS);return EFI_SUCCESS;
}
EFI_STATUS PianoDmaMap(PIANO_DMA_BUFFER *B) {
  if(!Valid(B) || B->Mapped || B->Active || B->Quarantined || B->ExitRetained)return EFI_INVALID_PARAMETER;
  if(B->Device->Map==NULL || B->Device->Unmap==NULL){Log(B,"map","none",EFI_NOT_READY);return EFI_NOT_READY;}
  EFI_PHYSICAL_ADDRESS Iova=0;VOID *Mapping=NULL;
  EFI_STATUS Status=B->Device->Map(B->Device,B->Physical,B->ReservedBytes,B->Direction,B->Alignment,&Iova,&Mapping);
  if(EFI_ERROR(Status)) {
    if(Mapping!=NULL){B->Mapping=Mapping;B->DeviceAddress=Iova;B->Mapped=TRUE;B->Quarantined=TRUE;}
    Log(B,"map",Mapping?"retained":"none",Status);return Status;
  }
  UINT64 Limit=B->Device->AddressBits==32?MAX_UINT32:MAX_UINT64;
  if((Iova&(B->Alignment-1)) || Iova>Limit || B->ReservedBytes-1>Limit-Iova) {
    EFI_STATUS Undo=B->Device->Unmap(B->Device,Mapping);
    if(EFI_ERROR(Undo)) {
      B->Mapping=Mapping;B->DeviceAddress=Iova;B->Mapped=TRUE;B->Quarantined=TRUE;
      Log(B,"map-rollback-failed","retained",Undo);return Undo;
    }
    Log(B,"map","none",EFI_BAD_BUFFER_SIZE);return EFI_BAD_BUFFER_SIZE;
  }
  B->Mapping=Mapping;B->DeviceAddress=Iova;B->Mapped=TRUE;Log(B,"map","none",EFI_SUCCESS);return EFI_SUCCESS;
}
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Command) {
  if(!Valid(B) || Command==NULL || !B->Mapped || B->Active || B->Quarantined || B->ExitRetained)return EFI_NOT_READY;
  CONST CHAR8 *Cache;
  if(B->Direction==PianoDmaToDevice){WriteBackDataCacheRange(B->Cpu,B->ReservedBytes);Cache="clean";}
  else {WriteBackInvalidateDataCacheRange(B->Cpu,B->ReservedBytes);Cache="clean-invalidate";}
  MemoryFence();B->Active=TRUE;B->Command=Command;
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_COMMAND dev=%a command=%a\n",B->Device->Name,Command));
  Log(B,"submit",Cache,EFI_SUCCESS);return EFI_SUCCESS;
}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiesced) {
  if(!Valid(B) || !B->Active)return EFI_INVALID_PARAMETER;
  if(!Quiesced) {
    B->Quarantined=TRUE;Log(B,"unquiesced","retained",EFI_DEVICE_ERROR);
    if(B->Device->Fault!=NULL)B->Device->Fault(B->Device);
    return EFI_DEVICE_ERROR;
  }
  MemoryFence();CONST CHAR8 *Cache="none";
  if(B->Direction!=PianoDmaToDevice){InvalidateDataCacheRange(B->Cpu,B->ReservedBytes);MemoryFence();Cache="invalidate";}
  B->Active=FALSE;B->Quarantined=FALSE;Log(B,"complete",Cache,Status);
  if(EFI_ERROR(Status) && B->Device->Fault!=NULL)B->Device->Fault(B->Device);
  return Status;
}
STATIC EFI_STATUS SyncForCpu(PIANO_DMA_BUFFER *B,BOOLEAN Quiet) {
  if(!Valid(B) || !B->Mapped || !B->Active || B->Quarantined || B->ExitRetained)return EFI_NOT_READY;
  if(B->Direction==PianoDmaToDevice)return EFI_ACCESS_DENIED;
  InvalidateDataCacheRange(B->Cpu,B->ReservedBytes);MemoryFence();
  if(Quiet){if(B->QuietSyncs!=MAX_UINT64)++B->QuietSyncs;}
  else Log(B,"sync-cpu","invalidate-active",EFI_SUCCESS);
  return EFI_SUCCESS;
}
EFI_STATUS PianoDmaSyncForCpu(PIANO_DMA_BUFFER *B){return SyncForCpu(B,FALSE);}
EFI_STATUS PianoDmaSyncForCpuQuiet(PIANO_DMA_BUFFER *B){return SyncForCpu(B,TRUE);}
EFI_STATUS PianoDmaReportQuietSync(PIANO_DMA_BUFFER *B) {
  if(!Valid(B))return EFI_INVALID_PARAMETER;
  if(B->QuietSyncs==B->QuietSyncReported || !DebugPrintEnabled() || !DebugPrintLevelEnabled(DEBUG_WARN))return EFI_SUCCESS;
  CHAR8 Body[384];UINTN Bytes=AsciiSPrint(Body,sizeof(Body),
    "phase=sync-cpu-quiet-summary seq=%u dev=%a sid=%x pa=%lx iova=%lx bytes=%lu reserved=%lu dir=%a align=%lu attrs=%lx cache=invalidate-active status=Success cmd=%a polls=%lu delta=%lu active=%u",
    mRecordSequence++,B->Device->Name,B->Device->StreamId,B->Physical,B->DeviceAddress,(UINT64)B->Bytes,
    (UINT64)B->ReservedBytes,Direction(B->Direction),(UINT64)B->Alignment,B->MemoryAttributes,
    B->Command==NULL?"-":B->Command,B->QuietSyncs,B->QuietSyncs-B->QuietSyncReported,B->Active);
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA %a crc32=%08x\n",Body,RecordCrc(Body,Bytes)));
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_COPY %a crc32=%08x\n",Body,RecordCrc(Body,Bytes)));
  B->QuietSyncReported=B->QuietSyncs;return EFI_SUCCESS;
}
EFI_STATUS PianoDmaUnmap(PIANO_DMA_BUFFER *B) {
  if(!Valid(B))return EFI_INVALID_PARAMETER;
  if(B->Active || B->Quarantined || B->ExitRetained)return EFI_ACCESS_DENIED;
  if(!B->Mapped)return EFI_SUCCESS;
  EFI_STATUS Status=B->Device->Unmap(B->Device,B->Mapping);
  if(EFI_ERROR(Status)){Log(B,"unmap","none",Status);return Status;}
  B->Mapped=FALSE;B->Mapping=NULL;B->DeviceAddress=0;Log(B,"unmap","none",EFI_SUCCESS);return EFI_SUCCESS;
}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B) {
  if(!Valid(B))return EFI_INVALID_PARAMETER;
  if(B->Active || B->Quarantined || B->ExitRetained)return EFI_ACCESS_DENIED;
  EFI_STATUS Status=PianoDmaUnmap(B);if(EFI_ERROR(Status))return Status;
  ZeroMem(B->Cpu,B->ReservedBytes);WriteBackDataCacheRange(B->Cpu,B->ReservedBytes);MemoryFence();
  Status=gBS->FreePages(B->Allocation,B->AllocationPages);
  if(!EFI_ERROR(Status))ZeroMem(B,sizeof(*B));
  return Status;
}
EFI_STATUS PianoDmaRetainForExit(PIANO_DMA_BUFFER *B) {
  if(!Valid(B) || !B->AllocationPages)return EFI_INVALID_PARAMETER;
  if(B->MemoryType!=EfiReservedMemoryType || B->Active || B->Quarantined)return EFI_ACCESS_DENIED;
  B->ExitRetained=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_EXIT_RETAIN dev=%a allocation=%lx pages=%lu memory_type=reserved efi_map_required=1 raw_dtb_reservation_verified=0\n",
    B->Device->Name,B->Allocation,(UINT64)B->AllocationPages));
  return EFI_SUCCESS;
}
