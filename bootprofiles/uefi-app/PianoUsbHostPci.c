// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB0-only PCI I/O core. Hardware readiness/DMA proof comes from the backend;
// this module neither installs a handle nor changes DWC3/PHY/Type-C roles.
#include "PianoUsbHostPci.h"
#include <IndustryStandard/Acpi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#define HOST_PCI_SIGNATURE SIGNATURE_32('P','H','P','C')
#define HOST_PCI_ATTRIBUTES (EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)
#define HOST_PCI_CAPABILITIES (PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_COHERENT|PIANO_USB_HOST_COMMON_UNCACHED|PIANO_USB_HOST_PARTIAL_FREE)
#define NO_ALLOCATION PIANO_USB_HOST_MAX_RECORDS
STATIC UINTN mNextMappingKey;
STATIC PIANO_USB_HOST_PCI *Device(EFI_PCI_IO_PROTOCOL *This) {
  if(This==NULL)return NULL;
  PIANO_USB_HOST_PCI *C=BASE_CR(This,PIANO_USB_HOST_PCI,Pci);
  return C->Signature==HOST_PCI_SIGNATURE?C:NULL;
}
BOOLEAN PianoUsbHostPciHasQuarantine(CONST PIANO_USB_HOST_PCI *C) {
  if(C==NULL || C->Signature!=HOST_PCI_SIGNATURE)return TRUE;
  if(C->ControllerUncertain)return TRUE;
  for(UINTN I=0;I<C->Limits.Mappings;++I)if(C->Mapping[I].Used && C->Mapping[I].Quarantined)return TRUE;
  for(UINTN I=0;I<C->Limits.Allocations;++I)if(C->Allocation[I].Used && C->Allocation[I].Quarantined)return TRUE;
  return FALSE;
}
STATIC EFI_STATUS Enter(PIANO_USB_HOST_PCI *C) {
  if(C==NULL)return EFI_INVALID_PARAMETER;
  if(C->Busy)return EFI_NOT_READY;
  C->Busy=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS Ready(PIANO_USB_HOST_PCI *C) {
  return C->Backend.Ready==NULL?EFI_NOT_READY:C->Backend.Ready(C->Backend.Context);
}
STATIC EFI_STATUS Quiet(PIANO_USB_HOST_PCI *C) {
  return C->Backend.Quiesced==NULL?EFI_NOT_READY:C->Backend.Quiesced(C->Backend.Context);
}
STATIC BOOLEAN CommonCap(PIANO_USB_HOST_PCI *C) {
  return (C->Backend.Capabilities&(PIANO_USB_HOST_COMMON_COHERENT|PIANO_USB_HOST_COMMON_UNCACHED))!=0;
}
STATIC BOOLEAN CommonMemory(PIANO_USB_HOST_PCI *C,UINT64 AttributesValue) {
  UINT64 Cache=AttributesValue&(EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB);
  if(!Cache || (Cache&(Cache-1)) || (AttributesValue&(EFI_MEMORY_RO|EFI_MEMORY_RP)))return FALSE;
  return (C->Backend.Capabilities&PIANO_USB_HOST_COMMON_COHERENT)!=0 || Cache==EFI_MEMORY_UC;
}
STATIC EFI_STATUS Range(EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT64 Offset,UINTN Count,UINTN Limit,UINTN *Unit) {
  if((UINTN)Width>=EfiPciIoWidthMaximum)return EFI_INVALID_PARAMETER;
  // XhciDxe uses ordinary widths. FIFO/fill and pass-through BAR are not exposed.
  if(Width>EfiPciIoWidthUint64)return EFI_UNSUPPORTED;
  *Unit=(UINTN)1<<Width;
  if(Offset&(*Unit-1))return EFI_INVALID_PARAMETER;
  if(Offset>Limit || Count>(Limit-(UINTN)Offset)/ *Unit)return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC VOID Put(UINT8 *P,UINT64 V,UINTN Bytes){for(UINTN I=0;I<Bytes;++I)P[I]=(UINT8)(V>>(I*8));}
STATIC UINT64 Get(CONST UINT8 *P,UINTN Bytes){UINT64 V=0;for(UINTN I=0;I<Bytes;++I)V|=(UINT64)P[I]<<(I*8);return V;}
STATIC VOID Config(PIANO_USB_HOST_PCI *C,UINT8 P[256]) {
  ZeroMem(P,256);Put(P,0xFFFF,2); // Emulated, not a fabricated hardware vendor ID.
  UINT16 Command=(C->Attributes&EFI_PCI_IO_ATTRIBUTE_MEMORY?2:0)|(C->Attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER?4:0);
  Put(P+4,Command,2);P[8]=1;P[9]=0x30;P[10]=3;P[11]=0x0C;
  Put(P+0x10,C->BarProbe?~(PIANO_USB_HOST_MMIO_BYTES-1U):PIANO_USB_HOST_MMIO_BASE,4);
  P[0x60]=0x30; // USB serial bus release number; no PCI capability/ROM advertised.
}
STATIC EFI_STATUS EFIAPI MemRead(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT8 Bar,
                                 UINT64 Offset,UINTN Count,VOID *Buffer) {
  PIANO_USB_HOST_PCI *C=Device(This);UINTN Unit;EFI_STATUS S;
  if(C==NULL || (Count && Buffer==NULL))return EFI_INVALID_PARAMETER;
  if(Bar!=0)return EFI_UNSUPPORTED;
  S=Range(Width,Offset,Count,PIANO_USB_HOST_MMIO_BYTES,&Unit);if(EFI_ERROR(S) || !Count)return S;
  S=Enter(C);if(EFI_ERROR(S))return S;
  if(!(C->Attributes&EFI_PCI_IO_ATTRIBUTE_MEMORY))S=EFI_NOT_READY;
  else S=Ready(C);
  if(!EFI_ERROR(S) && C->Backend.Read32==NULL)S=EFI_NOT_READY;
  for(UINTN I=0;!EFI_ERROR(S) && I<Count;++I) {
    UINT32 Address=(UINT32)Offset+(UINT32)(I*Unit),Low=0,High=0;UINT64 V=0;
    S=C->Backend.Read32(C->Backend.Context,Address&~3U,&Low);
    if(!EFI_ERROR(S) && Unit==8)S=C->Backend.Read32(C->Backend.Context,Address+4,&High);
    if(!EFI_ERROR(S)){V=Unit==8?((UINT64)High<<32)|Low:(UINT64)Low>>((Address&3)*8);CopyMem((UINT8 *)Buffer+I*Unit,&V,Unit);}
  }
  C->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI MemWrite(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT8 Bar,
                                  UINT64 Offset,UINTN Count,VOID *Buffer) {
  PIANO_USB_HOST_PCI *C=Device(This);UINTN Unit;EFI_STATUS S;
  if(C==NULL || (Count && Buffer==NULL))return EFI_INVALID_PARAMETER;
  if(Bar!=0)return EFI_UNSUPPORTED;
  S=Range(Width,Offset,Count,PIANO_USB_HOST_MMIO_BYTES,&Unit);if(EFI_ERROR(S) || !Count)return S;
  // Sub-DWORD RMW could acknowledge unrelated W1C status bits; reject it.
  if(Unit<4)return EFI_UNSUPPORTED;
  S=Enter(C);if(EFI_ERROR(S))return S;
  if((C->Attributes&HOST_PCI_ATTRIBUTES)!=HOST_PCI_ATTRIBUTES)S=EFI_NOT_READY;
  else S=Ready(C);
  if(!EFI_ERROR(S) && C->Backend.Write32==NULL)S=EFI_NOT_READY;
  for(UINTN I=0;!EFI_ERROR(S) && I<Count;++I) {
    UINT64 V=0;UINT32 Address=(UINT32)Offset+(UINT32)(I*Unit);CopyMem(&V,(UINT8 *)Buffer+I*Unit,Unit);
    if(Unit==8)S=C->Backend.Write32(C->Backend.Context,Address+4,(UINT32)(V>>32));
    if(!EFI_ERROR(S))S=C->Backend.Write32(C->Backend.Context,Address,(UINT32)V);
  }
  C->Busy=FALSE;return S;
}
EFI_STATUS PianoUsbHostPciWrite64HiLo(PIANO_USB_HOST_PCI *C,UINT32 Offset,UINT64 Value) {
  if(C==NULL || C->Signature!=HOST_PCI_SIGNATURE)return EFI_INVALID_PARAMETER;
  return MemWrite(&C->Pci,EfiPciIoWidthUint64,0,Offset,1,&Value);
}
STATIC EFI_STATUS EFIAPI PollMem(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT8 Bar,
                                 UINT64 Offset,UINT64 Mask,UINT64 Value,UINT64 Delay,UINT64 *Result) {
  PIANO_USB_HOST_PCI *C=Device(This);UINTN Unit;EFI_STATUS S;
  if(C==NULL || Result==NULL)return EFI_INVALID_PARAMETER;
  S=Range(Width,Offset,1,PIANO_USB_HOST_MMIO_BYTES,&Unit);if(EFI_ERROR(S))return S;
  if(Bar!=0)return EFI_UNSUPPORTED;
  if(Delay>10000000U)return EFI_UNSUPPORTED; // Bounded to one second, in 100 ns units.
  UINTN Remaining=(UINTN)((Delay+9)/10);
  for(;;) {
    UINT64 V=0;S=MemRead(This,Width,Bar,Offset,1,&V);if(EFI_ERROR(S))return S;
    *Result=V;if((V&Mask)==Value)return EFI_SUCCESS;
    if(!Remaining)return EFI_TIMEOUT;
    if(C->Backend.Stall==NULL)return EFI_UNSUPPORTED;
    S=Enter(C);if(EFI_ERROR(S))return S;
    S=C->Backend.Stall(C->Backend.Context,1);C->Busy=FALSE;if(EFI_ERROR(S))return S;
    --Remaining;
  }
}
STATIC EFI_STATUS EFIAPI UnsupportedMem(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT8 Bar,
                                        UINT64 Offset,UINTN Count,VOID *Buffer) {return Device(This)?EFI_UNSUPPORTED:EFI_INVALID_PARAMETER;}
STATIC EFI_STATUS EFIAPI UnsupportedPoll(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT8 Bar,
                                         UINT64 Offset,UINT64 Mask,UINT64 Value,UINT64 Delay,UINT64 *Result) {return Device(This)?EFI_UNSUPPORTED:EFI_INVALID_PARAMETER;}
STATIC EFI_STATUS EFIAPI UnsupportedCopy(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                         UINT8 DestBar,UINT64 Dest,UINT8 SrcBar,UINT64 Src,UINTN Count) {return Device(This)?EFI_UNSUPPORTED:EFI_INVALID_PARAMETER;}
STATIC EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Op,
                                    UINT64 Bits,UINT64 *Result) {
  PIANO_USB_HOST_PCI *C=Device(This);EFI_STATUS S;
  if(C==NULL || (UINTN)Op>=EfiPciIoAttributeOperationMaximum)return EFI_INVALID_PARAMETER;
  if(Op==EfiPciIoAttributeOperationGet || Op==EfiPciIoAttributeOperationSupported) {
    if(Result==NULL)return EFI_INVALID_PARAMETER;
    *Result=Op==EfiPciIoAttributeOperationGet?C->Attributes:HOST_PCI_ATTRIBUTES;return EFI_SUCCESS;
  }
  if(Bits&~(UINT64)HOST_PCI_ATTRIBUTES)return EFI_UNSUPPORTED;
  UINT64 Next=Op==EfiPciIoAttributeOperationSet?Bits:Op==EfiPciIoAttributeOperationEnable?C->Attributes|Bits:C->Attributes&~Bits;
  if((Next&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) && !(Next&EFI_PCI_IO_ATTRIBUTE_MEMORY))return EFI_INVALID_PARAMETER;
  S=Enter(C);if(EFI_ERROR(S))return S;
  if(Next&~C->Attributes) {
    S=PianoUsbHostPciHasQuarantine(C)?EFI_NOT_READY:Ready(C);
    if(!EFI_ERROR(S) && (Next&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) {
      if(!CommonCap(C) || !(C->Backend.Capabilities&PIANO_USB_HOST_DMA_STREAMING))S=EFI_UNSUPPORTED;
      else if(C->Backend.Map==NULL || C->Backend.Retire==NULL || C->Backend.Allocate==NULL ||
              C->Backend.Free==NULL || C->Backend.Quiesced==NULL)S=EFI_NOT_READY;
    }
  }
  if(!EFI_ERROR(S) && (((Next^C->Attributes)&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
     (C->ControllerUncertain && !(Next&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)))) {
    S=C->Backend.SetBusMaster==NULL?EFI_NOT_READY:C->Backend.SetBusMaster(C->Backend.Context,(Next&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)!=0);
    if(EFI_ERROR(S))C->ControllerUncertain=TRUE;
  }
  if(!EFI_ERROR(S))C->Attributes=Next;
  C->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI PciRead(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                 UINT32 Offset,UINTN Count,VOID *Buffer) {
  PIANO_USB_HOST_PCI *C=Device(This);UINTN Unit;EFI_STATUS S;
  if(C==NULL || (Count && Buffer==NULL))return EFI_INVALID_PARAMETER;
  S=Range(Width,Offset,Count,256,&Unit);if(EFI_ERROR(S) || !Count)return S;
  UINT8 P[256];Config(C,P);CopyMem(Buffer,P+Offset,Count*Unit);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PciWrite(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT32 Offset,UINTN Count,VOID *Buffer) {
  PIANO_USB_HOST_PCI *C=Device(This);UINTN Unit;EFI_STATUS S;
  if(C==NULL || (Count && Buffer==NULL))return EFI_INVALID_PARAMETER;
  S=Range(Width,Offset,Count,256,&Unit);if(EFI_ERROR(S) || !Count)return S;
  UINTN Bytes=Unit*Count;
  if(Offset==0x10 && Bytes==4 && Width==EfiPciIoWidthUint32) {
    UINT32 V=(UINT32)Get(Buffer,4);
    if(V!=MAX_UINT32 && V!=PIANO_USB_HOST_MMIO_BASE)return EFI_WRITE_PROTECTED;
    if(C->Busy)return EFI_NOT_READY;
    C->BarProbe=V==MAX_UINT32;return EFI_SUCCESS;
  }
  if(Offset>=4 && Offset<=5 && Bytes<=8-Offset) {
    UINT8 P[256];Config(C,P);CopyMem(P+Offset,Buffer,Bytes);
    UINT16 Command=(UINT16)Get(P+4,2);
    if(Get(P+6,2)!=0 || (Command&~6U))return EFI_UNSUPPORTED;
    return Attributes(This,EfiPciIoAttributeOperationSet,
      (Command&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0)|(Command&4?EFI_PCI_IO_ATTRIBUTE_BUS_MASTER:0),NULL);
  }
  return EFI_WRITE_PROTECTED;
}
STATIC BOOLEAN Contains(VOID *Base,UINTN Bytes,VOID *Address,UINTN Length) {
  UINTN B=(UINTN)Base,A=(UINTN)Address;
  return A>=B && A-B<Bytes && Length<=Bytes-(A-B);
}
STATIC BOOLEAN Overlap(VOID *A,UINTN An,VOID *B,UINTN Bn) {
  UINTN Ap=(UINTN)A,Bp=(UINTN)B;
  return Ap<=Bp?Bp-Ap<An:Ap-Bp<Bn;
}
STATIC UINTN AllocationFor(PIANO_USB_HOST_PCI *C,VOID *Cpu,UINTN Bytes) {
  for(UINTN I=0;I<C->Limits.Allocations;++I)if(C->Allocation[I].Used &&
    Contains(C->Allocation[I].Cpu,C->Allocation[I].Pages*4096,Cpu,Bytes))return I;
  return NO_ALLOCATION;
}
STATIC VOID ReleaseMap(PIANO_USB_HOST_PCI *C,PIANO_USB_HOST_MAPPING *M) {
  if(M->Allocation<C->Limits.Allocations && C->Allocation[M->Allocation].References)
    --C->Allocation[M->Allocation].References;
  C->MappedBytes-=M->Charge;ZeroMem(M,sizeof(*M));
}
STATIC EFI_STATUS RetireMap(PIANO_USB_HOST_PCI *C,PIANO_USB_HOST_MAPPING *M,BOOLEAN Recovery) {
  EFI_STATUS S=EFI_SUCCESS;
  if(M->Native!=NULL)for(UINTN I=0;I<C->Limits.Mappings;++I)
    if(&C->Mapping[I]!=M && C->Mapping[I].Used && C->Mapping[I].Native==M->Native) {
      C->Mapping[I].Quarantined=TRUE;M->Quarantined=TRUE;return EFI_DEVICE_ERROR;
    }
  if(M->Common || M->Quarantined || C->ControllerUncertain || Recovery)S=Quiet(C);
  if(!EFI_ERROR(S)) {
    if(M->Native==NULL)S=EFI_DEVICE_ERROR;
    else S=C->Backend.Retire==NULL?EFI_NOT_READY:C->Backend.Retire(C->Backend.Context,M->Native,!M->Common && !M->Quarantined && !Recovery);
  }
  if(EFI_ERROR(S)){M->Quarantined=TRUE;return S;}
  ReleaseMap(C,M);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *This,EFI_PCI_IO_PROTOCOL_OPERATION Op,
                             VOID *Cpu,UINTN *Bytes,EFI_PHYSICAL_ADDRESS *Iova,VOID **Token) {
  PIANO_USB_HOST_PCI *C=Device(This);EFI_STATUS S;
  if(C==NULL || Cpu==NULL || Bytes==NULL || Iova==NULL || Token==NULL || !*Bytes ||
     (UINTN)Op>=EfiPciIoOperationMaximum)return EFI_INVALID_PARAMETER;
  *Iova=0;*Token=NULL;UINTN Requested=*Bytes,PageOffset=(UINTN)Cpu&4095;
  if(Requested>MAX_UINTN-(UINTN)Cpu || Requested>MAX_UINTN-PageOffset-4095)return EFI_INVALID_PARAMETER;
  UINTN Charge=ALIGN_VALUE(Requested+PageOffset,4096),Owner=AllocationFor(C,Cpu,Requested);
  if(Owner==NO_ALLOCATION)for(UINTN I=0;I<C->Limits.Allocations;++I)
    if(C->Allocation[I].Used && Overlap(Cpu,Requested,C->Allocation[I].Cpu,C->Allocation[I].Pages*4096))
      return EFI_BAD_BUFFER_SIZE; // A range cannot escape a tracked allocation by becoming "external".
  BOOLEAN Common=Op==EfiPciIoOperationBusMasterCommonBuffer;
  if(Common) {
    if(!CommonCap(C))return EFI_UNSUPPORTED;
    if(Owner==NO_ALLOCATION)return EFI_INVALID_PARAMETER;
    if(C->Allocation[Owner].Quarantined)return EFI_NOT_READY;
    if(!CommonMemory(C,C->Allocation[Owner].MemoryAttributes))return EFI_UNSUPPORTED;
  } else if(!(C->Backend.Capabilities&PIANO_USB_HOST_DMA_STREAMING))return EFI_UNSUPPORTED;
  S=Enter(C);if(EFI_ERROR(S))return S;
  S=PianoUsbHostPciHasQuarantine(C)?EFI_NOT_READY:Ready(C);
  if(!EFI_ERROR(S) && (C->Backend.Map==NULL || C->Backend.Retire==NULL || (Common && C->Backend.Quiesced==NULL)))S=EFI_NOT_READY;
  UINTN Slot=0;while(Slot<C->Limits.Mappings && C->Mapping[Slot].Used)++Slot;
  if(!EFI_ERROR(S) && (Slot==C->Limits.Mappings || Charge>C->Limits.Bytes-C->MappedBytes || mNextMappingKey==MAX_UINTN))S=EFI_OUT_OF_RESOURCES;
  if(EFI_ERROR(S)){C->Busy=FALSE;return S;}
  PIANO_USB_HOST_MAPPING *M=&C->Mapping[Slot];
  *M=(PIANO_USB_HOST_MAPPING){.Used=TRUE,.Common=Common,.Key=++mNextMappingKey,.Charge=Charge,
    .Allocation=Owner,.Cpu=Cpu,.Bytes=Requested,.Iova=MAX_UINT64};
  C->MappedBytes+=Charge;if(Owner!=NO_ALLOCATION)++C->Allocation[Owner].References;
  PIANO_DMA_DIRECTION Direction=Common?PianoDmaBidirectional:Op==EfiPciIoOperationBusMasterRead?PianoDmaToDevice:PianoDmaFromDevice;
  UINTN Actual=Requested;
  S=C->Backend.Map(C->Backend.Context,Direction,Common,Cpu,&Actual,&M->Iova,&M->Native);
  if(EFI_ERROR(S) && M->Native==NULL && M->Iova==MAX_UINT64){ReleaseMap(C,M);C->Busy=FALSE;return S;}
  if(!EFI_ERROR(S) && (Actual!=Requested || M->Native==NULL || M->Iova>MAX_UINT32 ||
      Requested-1>MAX_UINT32-M->Iova || (Common && ((M->Iova^(UINTN)Cpu)&4095))))S=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(S)) {
    UINTN DeviceSpan=ALIGN_VALUE(Requested+(UINTN)(M->Iova&4095),4096);
    if(DeviceSpan>Charge) {
      // A backend violated its reservation allowance: retain the actual span,
      // even above the budget, and freeze new mappings until halted recovery.
      C->MappedBytes+=DeviceSpan-Charge;M->Charge=DeviceSpan;S=EFI_DEVICE_ERROR;
    }
    for(UINTN I=0;I<C->Limits.Mappings;++I)if(I!=Slot && C->Mapping[I].Used && C->Mapping[I].Iova<=MAX_UINT32) {
      PIANO_USB_HOST_MAPPING *Other=&C->Mapping[I];
      UINTN OtherSpan=ALIGN_VALUE(Other->Bytes+(UINTN)(Other->Iova&4095),4096);
      if(Overlap((VOID *)(UINTN)(M->Iova&~4095ULL),DeviceSpan,
                 (VOID *)(UINTN)(Other->Iova&~4095ULL),OtherSpan)) {
        Other->Quarantined=TRUE;S=EFI_DEVICE_ERROR;
      }
    }
    for(UINTN I=0;I<C->Limits.Mappings;++I)
      if(I!=Slot && C->Mapping[I].Used && C->Mapping[I].Native==M->Native) {
        C->Mapping[I].Quarantined=TRUE;S=EFI_DEVICE_ERROR;
      }
  }
  if(EFI_ERROR(S)) {
    // A failed/incomplete native map may still exist. Retain its accounting and
    // opaque cookie; only explicit halted recovery can retire uncertain state.
    M->Quarantined=TRUE;*Token=(VOID *)M->Key;C->Busy=FALSE;return S;
  }
  *Iova=M->Iova;*Token=(VOID *)M->Key;C->Busy=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *This,VOID *Token) {
  PIANO_USB_HOST_PCI *C=Device(This);EFI_STATUS S=Enter(C);if(EFI_ERROR(S))return S;
  UINTN I=0;while(I<C->Limits.Mappings && (!C->Mapping[I].Used || C->Mapping[I].Key!=(UINTN)Token))++I;
  S=Token==NULL || I==C->Limits.Mappings?EFI_INVALID_PARAMETER:RetireMap(C,&C->Mapping[I],FALSE);
  C->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Allocate(EFI_PCI_IO_PROTOCOL *This,EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE MemoryType,
                                  UINTN Pages,VOID **Cpu,UINT64 RequestedAttributes) {
  PIANO_USB_HOST_PCI *C=Device(This);EFI_STATUS S;
  if(C==NULL || Cpu==NULL || !Pages)return EFI_INVALID_PARAMETER;
  *Cpu=NULL;
  if(MemoryType!=EfiBootServicesData)return EFI_UNSUPPORTED;
  if(RequestedAttributes&~(UINT64)EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED)return EFI_UNSUPPORTED;
  if(!CommonCap(C) || ((RequestedAttributes&EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED) &&
     !(C->Backend.Capabilities&PIANO_USB_HOST_COMMON_COHERENT)))return EFI_UNSUPPORTED;
  if(Pages>C->Limits.Bytes/4096)return EFI_OUT_OF_RESOURCES;
  S=Enter(C);if(EFI_ERROR(S))return S;
  S=PianoUsbHostPciHasQuarantine(C)?EFI_NOT_READY:Ready(C);
  if(!EFI_ERROR(S) && (C->Backend.Allocate==NULL || C->Backend.Free==NULL))S=EFI_NOT_READY;
  UINTN Slot=0;while(Slot<C->Limits.Allocations && C->Allocation[Slot].Used)++Slot;
  UINTN Charge=Pages*4096;
  if(!EFI_ERROR(S) && (Slot==C->Limits.Allocations || Charge>C->Limits.Bytes-C->AllocatedBytes))S=EFI_OUT_OF_RESOURCES;
  if(EFI_ERROR(S)){C->Busy=FALSE;return S;}
  PIANO_USB_HOST_ALLOCATION *A=&C->Allocation[Slot];*A=(PIANO_USB_HOST_ALLOCATION){.Used=TRUE,.Pages=Pages};
  C->AllocatedBytes+=Charge;
  S=C->Backend.Allocate(C->Backend.Context,Pages,RequestedAttributes,&A->Cpu,&A->MemoryAttributes,&A->Native);
  if(EFI_ERROR(S) && A->Cpu==NULL && A->Native==NULL){C->AllocatedBytes-=Charge;ZeroMem(A,sizeof(*A));C->Busy=FALSE;return S;}
  if(!EFI_ERROR(S) && (A->Cpu==NULL || A->Native==NULL || ((UINTN)A->Cpu&4095) || Charge>MAX_UINTN-(UINTN)A->Cpu ||
      !CommonMemory(C,A->MemoryAttributes) ||
      ((RequestedAttributes&EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED) && !(A->MemoryAttributes&EFI_MEMORY_WB))))S=EFI_DEVICE_ERROR;
  for(UINTN I=0;!EFI_ERROR(S) && I<C->Limits.Allocations;++I)
    if(I!=Slot && C->Allocation[I].Used && Overlap(A->Cpu,Charge,C->Allocation[I].Cpu,C->Allocation[I].Pages*4096))S=EFI_DEVICE_ERROR;
  if(EFI_ERROR(S)){A->Quarantined=TRUE;C->Busy=FALSE;return S;}
  *Cpu=A->Cpu;C->Busy=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS FreeSpan(PIANO_USB_HOST_PCI *C,UINTN Index,VOID *Cpu,UINTN Pages) {
  PIANO_USB_HOST_ALLOCATION *A=&C->Allocation[Index];UINTN Bytes=Pages*4096;
  if(A->References)return EFI_ACCESS_DENIED;
  if(A->Native==NULL || A->Cpu==NULL || ((UINTN)A->Cpu&4095) || ((UINTN)Cpu&4095) ||
     A->Pages>MAX_UINTN/4096 || A->Pages*4096>MAX_UINTN-(UINTN)A->Cpu)return EFI_DEVICE_ERROR;
  for(UINTN I=0;I<C->Limits.Allocations;++I)
    if(I!=Index && C->Allocation[I].Used && Overlap(A->Cpu,A->Pages*4096,C->Allocation[I].Cpu,C->Allocation[I].Pages*4096))return EFI_DEVICE_ERROR;
  EFI_STATUS S=EFI_SUCCESS;
  if(A->Quarantined || C->ControllerUncertain)S=Quiet(C);
  if(EFI_ERROR(S))return S;
  UINTN Before=((UINTN)Cpu-(UINTN)A->Cpu)/4096,After=A->Pages-Before-Pages,Split=0;
  if(Before || After) {
    if(!(C->Backend.Capabilities&PIANO_USB_HOST_PARTIAL_FREE))return EFI_UNSUPPORTED;
    if(Before && After){while(Split<C->Limits.Allocations && C->Allocation[Split].Used)++Split;if(Split==C->Limits.Allocations)return EFI_OUT_OF_RESOURCES;}
  }
  S=C->Backend.Free==NULL?EFI_NOT_READY:C->Backend.Free(C->Backend.Context,A->Native,Cpu,Pages);
  if(EFI_ERROR(S)){A->Quarantined=TRUE;return S;}
  C->AllocatedBytes-=Bytes;
  if(!Before && !After)ZeroMem(A,sizeof(*A));
  else if(!Before){A->Cpu=(UINT8 *)Cpu+Bytes;A->Pages=After;A->Quarantined=FALSE;}
  else if(!After){A->Pages=Before;A->Quarantined=FALSE;}
  else {C->Allocation[Split]=*A;C->Allocation[Split].Cpu=(UINT8 *)Cpu+Bytes;C->Allocation[Split].Pages=After;C->Allocation[Split].Quarantined=FALSE;A->Pages=Before;A->Quarantined=FALSE;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Free(EFI_PCI_IO_PROTOCOL *This,UINTN Pages,VOID *Cpu) {
  PIANO_USB_HOST_PCI *C=Device(This);
  if(C==NULL || Cpu==NULL || !Pages || Pages>C->Limits.Bytes/4096 || ((UINTN)Cpu&4095))return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(C);if(EFI_ERROR(S))return S;
  UINTN I=AllocationFor(C,Cpu,Pages*4096);
  S=I==NO_ALLOCATION?EFI_INVALID_PARAMETER:FreeSpan(C,I,Cpu,Pages);C->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_PCI_IO_PROTOCOL *This) {
  PIANO_USB_HOST_PCI *C=Device(This);EFI_STATUS S=Enter(C);if(EFI_ERROR(S))return S;
  S=C->Backend.Flush==NULL?EFI_UNSUPPORTED:C->Backend.Flush(C->Backend.Context);C->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *This,UINTN *Segment,UINTN *Bus,UINTN *Dev,UINTN *Function) {
  if(Device(This)==NULL || Segment==NULL || Bus==NULL || Dev==NULL || Function==NULL)return EFI_INVALID_PARAMETER;
  *Segment=0xff;*Bus=0;*Dev=0;*Function=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetBar(EFI_PCI_IO_PROTOCOL *This,UINT8 Bar,UINT64 *Supports,VOID **Resources) {
  if(Device(This)==NULL || (Supports==NULL && Resources==NULL))return EFI_INVALID_PARAMETER;
  if(Bar!=0)return EFI_UNSUPPORTED;
  if(Supports!=NULL)*Supports=0;
  if(Resources!=NULL) {
    *Resources=NULL;
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *D=AllocateZeroPool(sizeof(*D)+sizeof(EFI_ACPI_END_TAG_DESCRIPTOR));
    if(D==NULL)return EFI_OUT_OF_RESOURCES;
    D->Desc=ACPI_ADDRESS_SPACE_DESCRIPTOR;D->Len=sizeof(*D)-3;D->ResType=ACPI_ADDRESS_SPACE_TYPE_MEM;
    D->AddrSpaceGranularity=32;D->AddrRangeMin=PIANO_USB_HOST_MMIO_BASE;
    D->AddrRangeMax=PIANO_USB_HOST_MMIO_BASE+PIANO_USB_HOST_MMIO_BYTES-1;D->AddrLen=PIANO_USB_HOST_MMIO_BYTES;
    EFI_ACPI_END_TAG_DESCRIPTOR *End=(VOID *)(D+1);End->Desc=ACPI_END_TAG_DESCRIPTOR;*Resources=D;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetBar(EFI_PCI_IO_PROTOCOL *This,UINT64 AttributesValue,UINT8 Bar,UINT64 *Offset,UINT64 *Length) {
  if(Device(This)==NULL || Offset==NULL || Length==NULL)return EFI_INVALID_PARAMETER;
  if(Bar!=0 || AttributesValue)return EFI_UNSUPPORTED;
  if(*Offset>PIANO_USB_HOST_MMIO_BYTES || *Length>PIANO_USB_HOST_MMIO_BYTES-*Offset)return EFI_UNSUPPORTED;
  return EFI_SUCCESS; // Zero attributes requests no change to the fixed BAR.
}
EFI_STATUS PianoUsbHostPciInit(PIANO_USB_HOST_PCI *C,CONST PIANO_USB_HOST_PCI_BACKEND *Backend,
                              CONST PIANO_USB_HOST_PCI_LIMITS *Limits) {
  if(C==NULL || Backend==NULL || (Backend->Capabilities&~HOST_PCI_CAPABILITIES))return EFI_INVALID_PARAMETER;
  if(C->Signature==HOST_PCI_SIGNATURE && (C->Busy || C->AllocatedBytes || C->MappedBytes || C->Attributes || PianoUsbHostPciHasQuarantine(C)))return EFI_ACCESS_DENIED;
  PIANO_USB_HOST_PCI_LIMITS L=Limits==NULL?(PIANO_USB_HOST_PCI_LIMITS){PIANO_USB_HOST_DMA_BUDGET,64,64}:*Limits;
  if(!L.Bytes || L.Bytes>PIANO_USB_HOST_DMA_BUDGET || (L.Bytes&4095) || !L.Allocations ||
     L.Allocations>64 || !L.Mappings || L.Mappings>64)return EFI_INVALID_PARAMETER;
  PIANO_USB_HOST_PCI_BACKEND B=*Backend;
  ZeroMem(C,sizeof(*C));C->Signature=HOST_PCI_SIGNATURE;C->Backend=B;C->Limits=L;
  C->Pci=(EFI_PCI_IO_PROTOCOL){.PollMem=PollMem,.PollIo=UnsupportedPoll,.Mem={MemRead,MemWrite},
    .Io={UnsupportedMem,UnsupportedMem},.Pci={PciRead,PciWrite},.CopyMem=UnsupportedCopy,.Map=Map,.Unmap=Unmap,
    .AllocateBuffer=Allocate,.FreeBuffer=Free,.Flush=Flush,.GetLocation=Location,.Attributes=Attributes,
    .GetBarAttributes=GetBar,.SetBarAttributes=SetBar};return EFI_SUCCESS;
}
EFI_STATUS PianoUsbHostPciRecover(PIANO_USB_HOST_PCI *C) {
  if(C==NULL || C->Signature!=HOST_PCI_SIGNATURE)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(C);if(EFI_ERROR(S))return S;
  if((C->Attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) || C->ControllerUncertain) {
    S=C->Backend.SetBusMaster==NULL?EFI_NOT_READY:C->Backend.SetBusMaster(C->Backend.Context,FALSE);
    if(EFI_ERROR(S)){C->ControllerUncertain=TRUE;C->Busy=FALSE;return S;}
    C->Attributes&=~(UINT64)EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  }
  S=Quiet(C);if(EFI_ERROR(S)){C->Busy=FALSE;return S;}
  EFI_STATUS Result=EFI_SUCCESS;C->ControllerUncertain=FALSE;
  for(UINTN I=0;I<C->Limits.Mappings;++I)if(C->Mapping[I].Used && C->Mapping[I].Quarantined) {
    S=RetireMap(C,&C->Mapping[I],TRUE);if(EFI_ERROR(S))Result=S;
  }
  for(UINTN I=0;I<C->Limits.Allocations;++I)if(C->Allocation[I].Used && C->Allocation[I].Quarantined) {
    S=FreeSpan(C,I,C->Allocation[I].Cpu,C->Allocation[I].Pages);if(EFI_ERROR(S))Result=S;
  }
  C->Busy=FALSE;return Result;
}
