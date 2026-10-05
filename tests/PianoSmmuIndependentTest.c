// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "PianoSmmuNativeIndependent.h"
static UINT32 Registers[0x100000/4],Before[0x100000/4];
static PIANO_SMMU_INDEPENDENT C;static PIANO_IO_PAGE_TABLE Pt;static PIANO_DMA_BUFFER Tables;
static UINTN Writes,Cleans;static UINT64 Time;
static BOOLEAN MasterBusy,TlbBusy,PeerChange,WriteWarning,ReadFailure,BadFormat;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_STATUS read32(VOID *Ctx,UINTN Address,UINT32 *Value){assert(Ctx==(VOID *)0x123);
  if(Address>=0x15000000 && Address<0x15100000){UINTN Off=Address-0x15000000;if(ReadFailure && Off==0x83000)return EFI_ACCESS_DENIED;
    *Value=Off==0x807f4 && TlbBusy?1:Registers[Off/4];return EFI_SUCCESS;}
  if(Address>=0x1d84000 && Address<0x1d84400){*Value=MasterBusy?1:0;return EFI_SUCCESS;}
  assert(Address==0xa60c704 || Address==0xa60c70c);*Value=Address==0xa60c704?(MasterBusy?BIT31:0):(MasterBusy?0:BIT22);return EFI_SUCCESS;
}
static EFI_STATUS write32(VOID *Ctx,UINTN Address,UINT32 Value){assert(Ctx==(VOID *)0x123);
  UINTN Off=Address-0x15000000;
  assert(Off==0x800 || Off==0xc00 || Off==0x1000 || Off==0x1800 || (Off>=0x80000 && Off<0x81000));++Writes;
  if(WriteWarning)return EFI_WARN_STALE_DATA;
  if(Off==0x80618 || Off==0x807f0)return EFI_SUCCESS;
  Registers[Off/4]=Off==0x80010?Value|0x60:Value;
  if(Off==0x1800)Registers[0x80058/4]=(Value&1)?(BadFormat?BIT9:BIT10):0;
  if(PeerChange)Registers[0x808/4]|=1;
  return EFI_SUCCESS;
}
static EFI_STATUS fence(VOID *Ctx){assert(Ctx==(VOID *)0x123);return EFI_SUCCESS;}
static EFI_STATUS clean(VOID *Ctx,CONST VOID *Memory,UINTN Bytes){assert(Ctx==(VOID *)0x123 && Memory==Tables.Cpu && Bytes==PIANO_IO_PT_BYTES);++Cleans;return EFI_SUCCESS;}
static EFI_STATUS now(VOID *Ctx,UINT64 *V){assert(Ctx==(VOID *)0x123);*V=Time++;return EFI_SUCCESS;}
static EFI_STATUS pause_cpu(VOID *Ctx){assert(Ctx==(VOID *)0x123);return EFI_SUCCESS;}
static PIANO_SMMU_INDEPENDENT_OPS Ops={(VOID *)0x123,read32,write32,fence,clean,now,pause_cpu};
static VOID fresh(void){
  memset(&C,0,sizeof(C));memset(Registers,0,sizeof(Registers));Writes=Cleans=Time=0;MasterBusy=TlbBusy=PeerChange=WriteWarning=ReadFailure=BadFormat=FALSE;
  Registers[0]=0x00290406;Registers[0x20/4]=0x4c017e7f;Registers[0x24/4]=0x60000053;Registers[0x28/4]=0x5111;
  Registers[0x800/4]=0x60;Registers[0xc00/4]=0x20000;
  Registers[0x808/4]=0x80020800;Registers[0xc08/4]=2;Registers[0x1008/4]=0x1f000;Registers[0x1808/4]=1;Registers[0x82000/4]=0x1e5;
  for(UINTN I=0;I<83;++I)Registers[(0x80010+I*4096)/4]=0x60;
  memcpy(Before,Registers,sizeof(Before));Tables.Quarantined=FALSE;
  assert(PianoIoPageTableInit(&Pt,Tables.Cpu,Tables.Physical,Tables.ReservedBytes)==EFI_SUCCESS);
}
int main(void){
  VOID *Memory=NULL;assert(posix_memalign(&Memory,4096,PIANO_IO_PT_BYTES)==0);
  Tables=(PIANO_DMA_BUFFER){.Signature=1,.Cpu=Memory,.Physical=0xd3000000,.ReservedBytes=PIANO_IO_PT_BYTES,.MemoryType=EfiReservedMemoryType,.MemoryAttributes=EFI_MEMORY_WB};
  for(UINTN I=0;I<2;++I){fresh();assert(PianoSmmuIndependentOpen(&C,(PIANO_SMMU_INDEPENDENT_MASTER)I,&Ops,&Pt,&Tables)==EFI_SUCCESS && C.Attached && C.RegisterConfigurationVerified && !C.Retained && Cleans==1);
    assert(C.Bank==0 && C.Slot==0 && Registers[0x808/4]==Before[0x808/4] && Registers[0x82000/4]==Before[0x82000/4]);
    assert(PianoIoPageTableMap(&Pt,PIANO_IOVA_BASE,0xc0000000,4096,PianoDmaBidirectional)==EFI_SUCCESS);
    assert(PianoSmmuIndependentSync(&C)==EFI_SUCCESS);
    assert(PianoIoPageTableUnmap(&Pt,PIANO_IOVA_BASE,4096)==EFI_SUCCESS && PianoSmmuIndependentSync(&C)==EFI_SUCCESS);
    assert(PianoSmmuIndependentClose(&C)==EFI_SUCCESS && C.Closed && C.TablesUnreachable && !C.Attached && !memcmp(Before,Registers,sizeof(Before)));
  }
  fresh();assert(PianoSmmuIndependentOpen(&C,(PIANO_SMMU_INDEPENDENT_MASTER)-1,&Ops,&Pt,&Tables)==EFI_INVALID_PARAMETER && !Writes);
  fresh();BadFormat=TRUE;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_COMPROMISED_DATA && C.Retained && Tables.Quarantined);
  fresh();MasterBusy=TRUE;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_NOT_READY && !Writes && !Tables.Quarantined);
  fresh();for(UINTN I=0;I<83;++I)Registers[(0x80000+I*4096)/4]|=1;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_OUT_OF_RESOURCES && !Writes);
  fresh();Registers[0x80c/4]=0x80000060;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_ALREADY_STARTED && !Writes);
  fresh();ReadFailure=TRUE;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_ACCESS_DENIED && !Writes);
  fresh();WriteWarning=TRUE;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_DEVICE_ERROR && C.Retained && Tables.Quarantined && Writes==1);
  fresh();PeerChange=TRUE;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)!=EFI_SUCCESS && C.Retained && Tables.Quarantined);
  fresh();assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_SUCCESS);TlbBusy=TRUE;
  assert(PianoSmmuIndependentSync(&C)==EFI_TIMEOUT && C.Retained && Tables.Quarantined && PianoSmmuIndependentClose(&C)==EFI_ACCESS_DENIED);
  fresh();Registers[0]|=1;assert(PianoSmmuIndependentOpen(&C,PianoIndependentUfs,&Ops,&Pt,&Tables)==EFI_UNSUPPORTED && !Writes);
  free(Memory);puts("Independent SMMU actual source: 12 injected-register cases PASS; UFS/USB actual idle, referenced/live CB exclusion, display/peer preservation, configure-before-route, shared PT clean/TLB, exact close restore, warning/secure-read/timeout retention; no device or global reset.");return 0;
}
