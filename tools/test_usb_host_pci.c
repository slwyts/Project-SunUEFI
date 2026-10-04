// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual PCI I/O core, with ordinary heap storage and fake MMIO/DMA only.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUsbHostPci.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
typedef struct {VOID *Base;UINTN Pages,Live;UINT64 Bitmap;} FA;
typedef struct {BOOLEAN Used,Common,Done;PIANO_DMA_DIRECTION Direction;VOID *Caller,*Dma;UINTN Bytes;} FM;
typedef struct {
  PIANO_USB_HOST_PCI *Core;
  EFI_STATUS ReadyStatus;
  UINT64 MemoryAttributes;
  BOOLEAN Halted,BusMaster,FailEnable,FailDisable,FailMap,NoMapResource,BadIova,ShortMap,NullMap;
  BOOLEAN FailRetire,FailFree,MapDone,Reenter,SameIova,CrossPage,SameNative;
  UINTN Reads,Writes,Maps,Retires,Frees,Allocations,Flushes,Stalls,CopyBacks;
  UINTN WriteFailAt;UINT32 WriteOffset[128],WriteValue[128],Mmio[0x8000/4];
  FA Allocation[16];FM Mapping[16];
} FAKE;
static EFI_STATUS ready(VOID *P){return ((FAKE *)P)->ReadyStatus;}
static EFI_STATUS read32(VOID *P,UINT32 Offset,UINT32 *V) {
  FAKE *F=P;assert(!(Offset&3) && Offset<0x8000);++F->Reads;
  if(F->Reenter) {
    UINT8 data[16];UINTN bytes=sizeof(data);EFI_PHYSICAL_ADDRESS io;VOID *map;
    assert(F->Core->Pci.Map(&F->Core->Pci,EfiPciIoOperationBusMasterRead,data,&bytes,&io,&map)==EFI_NOT_READY);
  }
  if(Offset==0x100 && F->Reads==3)F->Mmio[Offset/4]=3;
  *V=F->Mmio[Offset/4];return EFI_SUCCESS;
}
static EFI_STATUS write32(VOID *P,UINT32 Offset,UINT32 V) {
  FAKE *F=P;assert(!(Offset&3) && Offset<0x8000);assert(F->Writes<128);
  F->WriteOffset[F->Writes]=Offset;F->WriteValue[F->Writes]=V;++F->Writes;
  if(F->WriteFailAt==F->Writes)return EFI_DEVICE_ERROR;
  F->Mmio[Offset/4]=V;if(Offset==0x40)F->Halted=!(V&1);return EFI_SUCCESS;
}
static EFI_STATUS bus(VOID *P,BOOLEAN Enable) {
  FAKE *F=P;
  if(Enable){F->BusMaster=TRUE;if(F->FailEnable){F->Halted=FALSE;return EFI_DEVICE_ERROR;}}
  else {if(F->FailDisable)return EFI_TIMEOUT;F->BusMaster=FALSE;F->Halted=TRUE;}
  return EFI_SUCCESS;
}
static EFI_STATUS quiet(VOID *P){return ((FAKE *)P)->Halted?EFI_SUCCESS:EFI_NOT_READY;}
static EFI_STATUS stall(VOID *P,UINTN Us){((FAKE *)P)->Stalls+=Us;return EFI_SUCCESS;}
static EFI_STATUS flush(VOID *P){++((FAKE *)P)->Flushes;return EFI_SUCCESS;}
static EFI_STATUS allocate(VOID *P,UINTN Pages,UINT64 Requested,VOID **Cpu,UINT64 *AttributesValue,VOID **Token) {
  FAKE *F=P;UINTN I=0;while(I<16 && F->Allocation[I].Live)++I;assert(I<16 && Pages<=16);
  FA *A=&F->Allocation[I];A->Base=aligned_alloc(4096,Pages*4096);assert(A->Base);
  memset(A->Base,0,Pages*4096);A->Pages=A->Live=Pages;A->Bitmap=(1ULL<<Pages)-1;
  *Cpu=A->Base;*Token=A;*AttributesValue=F->MemoryAttributes;++F->Allocations;return EFI_SUCCESS;
}
static EFI_STATUS free_pages(VOID *P,VOID *Token,VOID *Cpu,UINTN Pages) {
  FAKE *F=P;FA *A=Token;++F->Frees;if(F->FailFree)return EFI_DEVICE_ERROR;
  assert(A>=F->Allocation && A<F->Allocation+16 && A->Live);
  UINTN Start=((UINTN)Cpu-(UINTN)A->Base)/4096;
  assert(Start+Pages<=A->Pages && !((UINTN)Cpu&4095));
  UINT64 Mask=((1ULL<<Pages)-1)<<Start;assert((A->Bitmap&Mask)==Mask);
  A->Bitmap&=~Mask;A->Live-=Pages;
  if(!A->Live){free(A->Base);A->Base=NULL;}return EFI_SUCCESS;
}
static EFI_STATUS map_dma(VOID *P,PIANO_DMA_DIRECTION Direction,BOOLEAN Common,VOID *Cpu,
                           UINTN *Bytes,EFI_PHYSICAL_ADDRESS *Iova,VOID **Token) {
  FAKE *F=P;++F->Maps;if(F->NoMapResource)return EFI_OUT_OF_RESOURCES;
  UINTN I=0;while(I<16 && F->Mapping[I].Used)++I;assert(I<16);
  FM *M=&F->Mapping[I];*M=(FM){.Used=TRUE,.Common=Common,.Done=F->MapDone,.Direction=Direction,.Caller=Cpu,.Bytes=*Bytes};
  M->Dma=Common?Cpu:malloc(*Bytes);assert(M->Dma);
  if(!Common){if(Direction==PianoDmaToDevice)memcpy(M->Dma,Cpu,*Bytes);else memset(M->Dma,0xA5,*Bytes);}
  *Token=F->NullMap?NULL:M;*Iova=F->BadIova?0x100000000ULL:0x40000000U+(F->SameIova?0:I*0x10000U)+(Common?((UINTN)Cpu&4095):0);
  if(F->SameNative && I)*Token=&F->Mapping[0];
  if(F->CrossPage)*Iova=0x40000FFF;
  if(F->ShortMap)--*Bytes;
  return F->FailMap?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS retire(VOID *P,VOID *Token,BOOLEAN CopyBack) {
  FAKE *F=P;FM *M=Token;++F->Retires;
  assert(M>=F->Mapping && M<F->Mapping+16 && M->Used);
  if(F->FailRetire)return EFI_TIMEOUT;
  if(M->Common && !F->Halted)return EFI_NOT_READY;
  if(!M->Common && !M->Done && !F->Halted)return EFI_NOT_READY;
  if(CopyBack && M->Direction==PianoDmaFromDevice){memcpy(M->Caller,M->Dma,M->Bytes);++F->CopyBacks;}
  if(!M->Common)free(M->Dma);
  ZeroMem(M,sizeof(*M));return EFI_SUCCESS;
}
static void init(PIANO_USB_HOST_PCI *C,FAKE *F,UINT32 Caps,UINTN Bytes,UINTN Maps) {
  memset(C,0,sizeof(*C));memset(F,0,sizeof(*F));F->Core=C;F->Halted=TRUE;F->MapDone=TRUE;
  F->MemoryAttributes=EFI_MEMORY_UC;F->Mmio[0]=0x01100040;
  PIANO_USB_HOST_PCI_BACKEND B={F,Caps,ready,read32,write32,bus,quiet,stall,flush,allocate,free_pages,map_dma,retire};
  PIANO_USB_HOST_PCI_LIMITS L={Bytes,8,Maps};assert(PianoUsbHostPciInit(C,&B,&L)==EFI_SUCCESS);
}
static void enable(PIANO_USB_HOST_PCI *C){assert(C->Pci.Attributes(&C->Pci,EfiPciIoAttributeOperationEnable,HOST_PCI_ATTRIBUTES,NULL)==EFI_SUCCESS);}
static VOID *alloc(PIANO_USB_HOST_PCI *C,UINTN Pages) {
  VOID *Cpu=NULL;assert(C->Pci.AllocateBuffer(&C->Pci,AllocateAnyPages,EfiBootServicesData,Pages,&Cpu,0)==EFI_SUCCESS);return Cpu;
}
static VOID *mapping(PIANO_USB_HOST_PCI *C,EFI_PCI_IO_PROTOCOL_OPERATION Op,VOID *Cpu,UINTN Bytes) {
  VOID *Token=NULL;EFI_PHYSICAL_ADDRESS Iova=0;
  assert(C->Pci.Map(&C->Pci,Op,Cpu,&Bytes,&Iova,&Token)==EFI_SUCCESS);
  assert(Token && Iova<=MAX_UINT32 && Iova!=(UINTN)Cpu);return Token;
}
static void metadata_mmio(void) {
  PIANO_USB_HOST_PCI C;FAKE F;init(&C,&F,PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED,0x10000,8);
  UINT8 cls[3];assert(C.Pci.Pci.Read(&C.Pci,EfiPciIoWidthUint8,9,3,cls)==EFI_SUCCESS);
  assert(cls[0]==0x30 && cls[1]==3 && cls[2]==0x0C);
  UINT64 attrs=0;assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationSupported,0,&attrs)==EFI_SUCCESS && attrs==HOST_PCI_ATTRIBUTES);
  UINT32 v=MAX_UINT32;assert(C.Pci.Pci.Write(&C.Pci,EfiPciIoWidthUint32,0x10,1,&v)==EFI_SUCCESS);
  assert(C.Pci.Pci.Read(&C.Pci,EfiPciIoWidthUint32,0x10,1,&v)==EFI_SUCCESS && v==0xFFFF8000);
  v=PIANO_USB_HOST_MMIO_BASE;assert(C.Pci.Pci.Write(&C.Pci,EfiPciIoWidthUint32,0x10,1,&v)==EFI_SUCCESS);
  v=0x10000000;assert(C.Pci.Pci.Write(&C.Pci,EfiPciIoWidthUint32,0x10,1,&v)==EFI_WRITE_PROTECTED);
  assert(C.Pci.Pci.Write(&C.Pci,EfiPciIoWidthUint32,8,1,&v)==EFI_WRITE_PROTECTED);
  F.ReadyStatus=EFI_NOT_READY;assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationEnable,attrs,NULL)==EFI_NOT_READY && !C.Attributes);
  F.ReadyStatus=EFI_SUCCESS;enable(&C);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint8,0,0,1,cls)==EFI_SUCCESS && cls[0]==0x40);
  UINT16 version;assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint16,0,2,1,&version)==EFI_SUCCESS && version==0x110);
  UINTN reads=F.Reads,writes=F.Writes;
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0,0x7ffc,2,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0,MAX_UINT64-3,1,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0,0,MAX_UINTN,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0,1,1,&v)==EFI_INVALID_PARAMETER);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0xff,0,1,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthFifoUint32,0,0,1,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Write(&C.Pci,EfiPciIoWidthUint32,0,0x7ffc,2,&v)==EFI_UNSUPPORTED);
  assert(C.Pci.Mem.Write(&C.Pci,EfiPciIoWidthUint8,0,0x40,1,cls)==EFI_UNSUPPORTED);
  assert(F.Reads==reads && F.Writes==writes);
  assert(PianoUsbHostPciWrite64HiLo(&C,0x70,0x1122334455667788ULL)==EFI_SUCCESS);
  assert(F.WriteOffset[0]==0x74 && F.WriteValue[0]==0x11223344 && F.WriteOffset[1]==0x70 && F.WriteValue[1]==0x55667788);
  F.WriteFailAt=3;assert(PianoUsbHostPciWrite64HiLo(&C,0x78,1)==EFI_DEVICE_ERROR && F.Writes==3);
  F.WriteFailAt=0;v=8;assert(C.Pci.Mem.Write(&C.Pci,EfiPciIoWidthUint32,0,0x58,1,&v)==EFI_SUCCESS && F.Writes==4 && F.WriteOffset[3]==0x58);
  F.Reads=0;UINT64 result=0;assert(C.Pci.PollMem(&C.Pci,EfiPciIoWidthUint32,0,0x100,3,3,50,&result)==EFI_SUCCESS && F.Stalls==2);
  assert(C.Pci.PollMem(&C.Pci,EfiPciIoWidthUint32,0,0x100,3,2,0,&result)==EFI_TIMEOUT);
  F.Reenter=TRUE;assert(C.Pci.Mem.Read(&C.Pci,EfiPciIoWidthUint32,0,0,1,&v)==EFI_SUCCESS);F.Reenter=FALSE;
  VOID *resources=NULL;assert(C.Pci.GetBarAttributes(&C.Pci,0,&attrs,&resources)==EFI_SUCCESS && !attrs);
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=resources;assert(d->AddrRangeMin==PIANO_USB_HOST_MMIO_BASE && d->AddrLen==0x8000);free(resources);
  assert(C.Pci.Flush(&C.Pci)==EFI_SUCCESS && F.Flushes==1);C.Backend.Flush=NULL;assert(C.Pci.Flush(&C.Pci)==EFI_UNSUPPORTED);
  assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,NULL)==EFI_UNSUPPORTED);
  assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationSet,0,NULL)==EFI_SUCCESS && !F.BusMaster);
  assert(C.Pci.Mem.Write(&C.Pci,EfiPciIoWidthUint32,0,0x40,1,&v)==EFI_NOT_READY);
}
static void common_streaming_budget(void) {
  PIANO_USB_HOST_PCI C;FAKE F;init(&C,&F,PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED,8192,2);enable(&C);
  VOID *cpu=alloc(&C,1);VOID *common=mapping(&C,EfiPciIoOperationBusMasterCommonBuffer,(UINT8 *)cpu+64,512);
  assert(C.Allocation[0].References==1 && C.MappedBytes==4096);
  assert(F.Mapping[0].Common && F.Mapping[0].Dma==(UINT8 *)cpu+64 && F.Mapping[0].Direction==PianoDmaBidirectional);
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_ACCESS_DENIED);
  UINTN escaped=8192;EFI_PHYSICAL_ADDRESS escaped_io;VOID *escaped_token;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,cpu,&escaped,&escaped_io,&escaped_token)==EFI_BAD_BUFFER_SIZE);
  static UINT8 user[4096] __attribute__((aligned(4096)));memset(user,0x33,sizeof(user));
  VOID *in=mapping(&C,EfiPciIoOperationBusMasterWrite,user,16);assert(F.Mapping[1].Direction==PianoDmaFromDevice && user[0]==0x33);
  UINTN n=16;EFI_PHYSICAL_ADDRESS io;VOID *extra;UINTN calls=F.Maps;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,user,&n,&io,&extra)==EFI_OUT_OF_RESOURCES && F.Maps==calls);
  assert(C.Pci.Unmap(&C.Pci,in)==EFI_SUCCESS && user[0]==0xA5 && F.CopyBacks==1 && C.MappedBytes==4096);
  VOID *out=mapping(&C,EfiPciIoOperationBusMasterRead,user,16);assert(out!=in);
  assert(C.Pci.Unmap(&C.Pci,in)==EFI_INVALID_PARAMETER); // Cookie cannot be reused after a new map.
  assert(C.Pci.Unmap(&C.Pci,(VOID *)&F.Mapping[0])==EFI_INVALID_PARAMETER);
  assert(C.Pci.Unmap(&C.Pci,out)==EFI_SUCCESS && F.Mapping[1].Used==FALSE);
  F.Halted=FALSE;assert(C.Pci.Unmap(&C.Pci,common)==EFI_NOT_READY && PianoUsbHostPciHasQuarantine(&C));
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_ACCESS_DENIED);
  assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.MappedBytes && !PianoUsbHostPciHasQuarantine(&C));
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_SUCCESS && !C.AllocatedBytes);
}
static void missing_capability_and_errors(void) {
  PIANO_USB_HOST_PCI C;FAKE F;init(&C,&F,PIANO_USB_HOST_DMA_STREAMING,8192,2);
  VOID *cpu;UINTN n=8;EFI_PHYSICAL_ADDRESS io;VOID *token;
  static UINT8 data[4096] __attribute__((aligned(4096)));
  assert(C.Pci.AllocateBuffer(&C.Pci,AllocateAnyPages,EfiBootServicesData,1,&cpu,0)==EFI_UNSUPPORTED && !F.Allocations);
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterCommonBuffer,data,&n,&io,&token)==EFI_UNSUPPORTED && !F.Maps);
  assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationEnable,HOST_PCI_ATTRIBUTES,NULL)==EFI_UNSUPPORTED);
  C.Backend.Ready=NULL;assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_MEMORY,NULL)==EFI_NOT_READY);
  for(UINTN mode=0;mode<4;++mode) {
    init(&C,&F,PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED,8192,2);
    F.FailMap=mode==0;F.BadIova=mode==1;F.ShortMap=mode==2;F.NoMapResource=mode==3;
    n=8;EFI_STATUS s=C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterWrite,data,&n,&io,&token);
    assert(s==(mode==3?EFI_OUT_OF_RESOURCES:EFI_DEVICE_ERROR));
    if(mode==3){assert(!C.MappedBytes && !token && !PianoUsbHostPciHasQuarantine(&C));continue;}
    assert(C.MappedBytes==4096 && token && PianoUsbHostPciHasQuarantine(&C));
    assert(C.Pci.AllocateBuffer(&C.Pci,AllocateAnyPages,EfiBootServicesData,1,&cpu,0)==EFI_NOT_READY);
    assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.MappedBytes && !F.CopyBacks);
  }
  init(&C,&F,PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED,8192,2);
  token=mapping(&C,EfiPciIoOperationBusMasterWrite,data,8);F.FailRetire=TRUE;
  assert(C.Pci.Unmap(&C.Pci,token)==EFI_TIMEOUT && C.MappedBytes==4096);
  assert(PianoUsbHostPciRecover(&C)==EFI_TIMEOUT && C.MappedBytes==4096);
  F.FailRetire=FALSE;assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !F.CopyBacks && !C.MappedBytes);
  cpu=alloc(&C,1);F.FailFree=TRUE;
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_DEVICE_ERROR && C.AllocatedBytes==4096);
  assert(PianoUsbHostPciRecover(&C)==EFI_DEVICE_ERROR && C.AllocatedBytes==4096);
  F.FailFree=FALSE;assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.AllocatedBytes);
  F.MemoryAttributes=EFI_MEMORY_WB;
  assert(C.Pci.AllocateBuffer(&C.Pci,AllocateAnyPages,EfiBootServicesData,1,&cpu,0)==EFI_DEVICE_ERROR);
  assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.AllocatedBytes);
  F.MemoryAttributes=EFI_MEMORY_UC;F.FailEnable=TRUE;
  assert(C.Pci.Attributes(&C.Pci,EfiPciIoAttributeOperationEnable,HOST_PCI_ATTRIBUTES,NULL)==EFI_DEVICE_ERROR && C.ControllerUncertain && F.BusMaster);
  F.FailEnable=FALSE;F.FailDisable=TRUE;assert(PianoUsbHostPciRecover(&C)==EFI_TIMEOUT && C.ControllerUncertain);
  F.FailDisable=FALSE;assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.ControllerUncertain && !F.BusMaster);
}
static void invalid_native_contracts(void) {
  PIANO_USB_HOST_PCI C;FAKE F;
  static UINT8 data[8192] __attribute__((aligned(4096)));UINTN n=8;VOID *token;EFI_PHYSICAL_ADDRESS io;
  UINT32 caps=PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED;
  init(&C,&F,caps,8192,2);F.NullMap=TRUE;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,data,&n,&io,&token)==EFI_DEVICE_ERROR && token && C.MappedBytes==4096);
  assert(PianoUsbHostPciRecover(&C)==EFI_DEVICE_ERROR && C.MappedBytes==4096 && !F.Retires);
  // Simulate a cold reset of the fake hardware: core intentionally cannot retire
  // a NULL native token or authorize reuse based on a guessed success.
  free(F.Mapping[0].Dma);ZeroMem(&F.Mapping[0],sizeof(F.Mapping[0]));
  init(&C,&F,caps,8192,2);F.CrossPage=TRUE;n=8;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,data,&n,&io,&token)==EFI_DEVICE_ERROR && C.MappedBytes==8192);
  assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.MappedBytes);
  init(&C,&F,caps,8192,2);VOID *first=mapping(&C,EfiPciIoOperationBusMasterRead,data,8);
  F.SameIova=TRUE;n=8;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,data+4096,&n,&io,&token)==EFI_DEVICE_ERROR);
  assert(C.Mapping[0].Quarantined && C.Mapping[1].Quarantined && first!=token);
  assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS && !C.MappedBytes);
  init(&C,&F,caps,8192,2);mapping(&C,EfiPciIoOperationBusMasterRead,data,8);F.SameNative=TRUE;n=8;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,data+4096,&n,&io,&token)==EFI_DEVICE_ERROR);
  assert(PianoUsbHostPciRecover(&C)==EFI_DEVICE_ERROR && C.MappedBytes==8192 && !F.Retires);
  for(UINTN I=0;I<2;++I){free(F.Mapping[I].Dma);ZeroMem(&F.Mapping[I],sizeof(F.Mapping[I]));}
  init(&C,&F,caps,8192,2);F.MemoryAttributes=EFI_MEMORY_UC|EFI_MEMORY_WB;VOID *cpu;
  assert(C.Pci.AllocateBuffer(&C.Pci,AllocateAnyPages,EfiBootServicesData,1,&cpu,0)==EFI_DEVICE_ERROR);
  assert(PianoUsbHostPciRecover(&C)==EFI_SUCCESS);
  init(&C,&F,PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_COHERENT,8192,2);F.MemoryAttributes=EFI_MEMORY_WB;
  assert(C.Pci.AllocateBuffer(&C.Pci,AllocateAnyPages,EfiBootServicesData,1,&cpu,EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED)==EFI_SUCCESS);
  C.Backend.Quiesced=NULL;n=4096;
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterCommonBuffer,cpu,&n,&io,&token)==EFI_NOT_READY && !C.MappedBytes);
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_SUCCESS);
}
static void partial_free_and_foreign_tokens(void) {
  PIANO_USB_HOST_PCI C,D;FAKE F,G;
  UINT32 caps=PIANO_USB_HOST_DMA_STREAMING|PIANO_USB_HOST_COMMON_UNCACHED|PIANO_USB_HOST_PARTIAL_FREE;
  init(&C,&F,caps,0x10000,8);init(&D,&G,caps,0x10000,8);
  VOID *cpu=alloc(&C,4);assert(C.Pci.FreeBuffer(&C.Pci,1,(UINT8 *)cpu+4096)==EFI_SUCCESS);
  assert(C.AllocatedBytes==3*4096 && C.Allocation[0].Pages==1 && C.Allocation[1].Pages==2);
  VOID *token=mapping(&C,EfiPciIoOperationBusMasterCommonBuffer,(UINT8 *)cpu+2*4096,4096);
  assert(D.Pci.Unmap(&D.Pci,token)==EFI_INVALID_PARAMETER);
  assert(C.Pci.Unmap(&C.Pci,token)==EFI_SUCCESS);
  assert(C.Pci.FreeBuffer(&C.Pci,2,(UINT8 *)cpu+2*4096)==EFI_SUCCESS);
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_SUCCESS && !C.AllocatedBytes);
  init(&C,&F,caps&~PIANO_USB_HOST_PARTIAL_FREE,8192,2);cpu=alloc(&C,2);
  assert(C.Pci.FreeBuffer(&C.Pci,1,cpu)==EFI_UNSUPPORTED && C.AllocatedBytes==8192);
  assert(C.Pci.FreeBuffer(&C.Pci,2,cpu)==EFI_SUCCESS);
  // Re-initializing a released context may use its embedded backend/limits.
  assert(PianoUsbHostPciInit(&C,&C.Backend,&C.Limits)==EFI_SUCCESS && C.Backend.Map==map_dma);
  assert(C.Pci.Map(&C.Pci,EfiPciIoOperationBusMasterRead,(VOID *)(MAX_UINTN-3),&(UINTN){8},&(EFI_PHYSICAL_ADDRESS){0},&(VOID *){NULL})==EFI_INVALID_PARAMETER);
  PIANO_USB_HOST_PCI_BACKEND B=C.Backend;PIANO_USB_HOST_PCI_LIMITS L={4096,65,1};
  assert(PianoUsbHostPciInit(&C,&B,&L)==EFI_INVALID_PARAMETER);
}
int main(void) {
  metadata_mmio();common_streaming_budget();missing_capability_and_errors();invalid_native_contracts();partial_free_and_foreign_tokens();
  puts("USB0 PCI I/O: fixed config/BAR/attributes, bounded MMIO and hi-lo order, explicit common policy, nonidentity directions, budgets/cookies, partial frees, retirement failures/quarantine and halted recovery passed. No hardware accessed.");
  return 0;
}
