// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/memory/PianoIoPageTable.c"
#include "../../uefi/core/memory/PianoOwnedSmmu.c"
static unsigned fsr_writes;static UINT32 fsr_clear_value;
static unsigned sync_calls;static int sync_fail;
static unsigned detaches,destroys,releases,exit_retains;static int retained_stream,changed_other;
static int gpi_case;static unsigned route_writes;static PIANO_SMMU_SNAPSHOT gpi_capture,gpi_live;
static unsigned qup_detaches;static int qup_detach_failure;
static EFI_STATUS gpi_quiet_status;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Size,CONST CHAR8 *Format,...){abort();return 0;}
UINT32 EFIAPI MmioRead32(UINTN Address){
  if(gpi_case && Address>=0x15000800 && Address<=0x15000808)return gpi_capture.RawSmr[(Address-0x15000800)/4];
  if(gpi_case && Address>=0x15000C00 && Address<=0x15000C08)return gpi_capture.RawS2cr[(Address-0x15000C00)/4];
  abort();return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  if(gpi_case) {
    assert(Address==0x15000800 || Address==0x15000C00 || Address==0x15000808 || Address==0x15000C08);++route_writes;
    if(Address<0x15000C00)gpi_capture.RawSmr[(Address-0x15000800)/4]=Value;
    else gpi_capture.RawS2cr[(Address-0x15000C00)/4]=Value;
    return Value;
  }
  assert(Address==0x15080058);++fsr_writes;fsr_clear_value=Value;return Value;
}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){return P;}
VOID EFIAPI MemoryFence(VOID){ }
static UINT32 sync(VOID *Domain){assert(Domain==(void *)123);++sync_calls;return sync_fail?1:0;}
static UINT32 detach(VOID *Domain,CONST CHAR8 *Name,UINT32 Arid,UINT32 Flags){
  assert(Domain==(void *)123 && !strcmp(Name,gpi_case?"QUP1":"UFS_MEM") && !Flags);
  if(Arid==0x03000004){++qup_detaches;return qup_detach_failure?1:0;}
  assert(Arid==(gpi_case?0x03000001:0));++detaches;return 0;
}
static UINT32 destroy(VOID *Domain){assert(Domain==(void *)123);++destroys;return 0;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!B->Quarantined);++releases;B->Signature=0;return EFI_SUCCESS;}
EFI_STATUS PianoDmaRetainForExit(PIANO_DMA_BUFFER *B){
  if(B->MemoryType!=EfiReservedMemoryType || B->Quarantined)return EFI_ACCESS_DENIED;
  ++exit_retains;B->ExitRetained=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *S){
  if(gpi_case){*S=gpi_capture;return EFI_SUCCESS;}
  memset(S,0,sizeof(*S));S->Valid=TRUE;S->Groups=2;S->Device[0].Present=retained_stream;
  S->RawSmr[1]=changed_other?124:123;return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCaptureQuiet(CONST VOID *Fdt,PIANO_SMMU_SNAPSHOT *S){
  assert(gpi_case);*S=gpi_live;return gpi_quiet_status;
}
int main(void){
  assert(OFFSET_OF(DOMAIN_CONFIG,Mair0)==0x18 && OFFSET_OF(DOMAIN_CONFIG,Sctlr)==0x20 &&
         OFFSET_OF(DOMAIN_CONFIG,Tcr)==0x24 && OFFSET_OF(DOMAIN_CONFIG,Tcr2)==0x28);
  PIANO_OWNED_SMMU *c=calloc(1,sizeof(*c));assert(c);void *memory=NULL;
  assert(posix_memalign(&memory,4096,PIANO_IO_PT_BYTES)==0);
  assert(PianoIoPageTableInit(&c->PageTable,memory,0xd7000000,PIANO_IO_PT_BYTES)==EFI_SUCCESS);
  c->TableMemory.Cpu=memory;c->TableMemory.ReservedBytes=PIANO_IO_PT_BYTES;
  UINTN functions[15]={0};functions[5]=(UINTN)sync;c->Api=functions;c->Domain=(void *)123;c->Attached=c->Verified=TRUE;
  PIANO_DMA_DEVICE d={.Name="ufs",.StreamId=0x60,.Context=c};EFI_PHYSICAL_ADDRESS a,b,pa;VOID *x=NULL,*y=NULL;
  assert(Map(&d,0x1000000000ULL,4096,PianoDmaToDevice,4096,&a,&x)==EFI_BAD_BUFFER_SIZE);
  assert(Map(&d,0xc0000000,4096,PianoDmaToDevice,0,&a,&x)==EFI_INVALID_PARAMETER);
  assert(Map(&d,0xc0000000,PIANO_IOVA_BYTES+4096,PianoDmaToDevice,4096,&a,&x)==EFI_INVALID_PARAMETER);
  assert(Map(&d,0xc0000000,4096,PianoDmaBidirectional,4096,&a,&x)==EFI_SUCCESS && a==PIANO_IOVA_BASE);
  assert(Map(&d,0xc1000000,8192,PianoDmaToDevice,65536,&b,&y)==EFI_SUCCESS && !(b&65535));
  assert(PianoIoPageTableTranslate(&c->PageTable,b,FALSE,&pa)==EFI_SUCCESS && pa==0xc1000000);
  assert(PianoIoPageTableTranslate(&c->PageTable,b,TRUE,&pa)==EFI_ACCESS_DENIED);
  assert(Unmap(&d,x)==EFI_SUCCESS && Unmap(&d,x)==EFI_INVALID_PARAMETER);
  assert(Unmap(&d,y)==EFI_SUCCESS);
  sync_fail=1;x=NULL;
  assert(Map(&d,0xc0000000,4096,PianoDmaBidirectional,4096,&a,&x)==EFI_DEVICE_ERROR && x!=NULL);
  assert(c->Mapping[0].Used); // A failed rollback must not silently recycle IOVA or RAM.
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED);
  memset(c->Mapping,0,sizeof(c->Mapping));functions[3]=(UINTN)detach;functions[1]=(UINTN)destroy;
  c->Before.Groups=2;c->Before.RawSmr[1]=123;c->After.Device[0].StreamIndex=0;c->TableMemory.Signature=123;
  retained_stream=1;
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && c->TableMemory.Quarantined && !destroys && !releases);
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED && detaches==1);
  retained_stream=0;c->TableMemory.Quarantined=FALSE;c->Attached=TRUE;
  assert(PianoOwnedSmmuClose(c)==EFI_SUCCESS && destroys==1 && releases==1);
  c->Domain=(void *)123;c->Attached=TRUE;c->TableMemory.Signature=123;changed_other=1;
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && c->TableMemory.Quarantined && releases==1);
  c->TableMemory.Quarantined=FALSE;c->Attached=c->Verified=TRUE;c->TableMemory.Device=&d;
  c->TableMemory.MemoryType=EfiBootServicesData;
  assert(PianoOwnedSmmuRetainForExit(c)==EFI_ACCESS_DENIED && !c->ExitRetained);
  c->TableMemory.MemoryType=EfiReservedMemoryType;
  unsigned old_detaches=detaches,old_destroys=destroys,old_releases=releases;
  assert(PianoOwnedSmmuRetainForExit(c)==EFI_SUCCESS && c->ExitRetained && exit_retains==1);
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED && detaches==old_detaches && destroys==old_destroys && releases==old_releases);
  assert(Map(&d,0xc0000000,4096,PianoDmaFromDevice,4096,&a,&x)==EFI_NOT_READY);
  assert(Unmap(&d,x)==EFI_ACCESS_DENIED);
  c->DeviceIndex=0;c->After.Base=0x15000000;c->After.ContextBase=0x80000;c->After.PageShift=12;c->After.Device[0].ContextBank=0;
  c->After.Device[0].Fsr=0x400;ClearOwnedStickyFault(c);assert(!fsr_writes && c->After.Device[0].Fsr==0x400);
  c->After.Device[0].Fsr=0x402;ClearOwnedStickyFault(c);assert(fsr_writes==1 && fsr_clear_value==2 && c->After.Device[0].Fsr==0x402);
  // GPI retirement restores its original invalid route before older domains
  // close. A changed/reused route is not permission to overwrite that slot.
  gpi_case=1;
  for(UINT32 Case=0;Case<8;++Case) {
    memset(c,0,sizeof(*c));memset(&gpi_capture,0,sizeof(gpi_capture));route_writes=0;
    gpi_quiet_status=EFI_SUCCESS;
    c->DeviceIndex=2;c->ResourceName="QUP1";c->Attached=c->Verified=c->OwnedIdentitySaved=TRUE;
    c->Api=functions;c->Domain=(VOID *)123;c->TableMemory.Signature=1;
    c->Before.Valid=TRUE;c->Before.Groups=2;c->Before.Base=0x15000000;c->Before.Window=0x100000;
    c->Before.ContextBase=0x80000;c->Before.PageShift=12;c->Before.Banks=83;
    c->Before.RawSmr[0]=0x5B6;c->Before.RawS2cr[0]=0x20001;c->Before.RawSmr[1]=0x80000060;
    c->After=c->Before;
    c->After.Device[2]=(PIANO_SMMU_DEVICE){.Sid=0xB6,.StreamIndex=0,.ContextBank=2,.Present=TRUE,
      .Enabled=TRUE,.Smr=0x800000B6,.S2cr=2,.Sctlr=0x1E5,.Cba2r=1,.Tcr=0x802519,.Tcr2=0x38001,
      .Mair0=0xFF,.Ttbr0=0x82000000};
    c->After.RawSmr[0]=c->After.Device[2].Smr;c->After.RawS2cr[0]=c->After.Device[2].S2cr;
    c->OwnedTablePhysical=0x82000000;c->AttachedSnapshot=gpi_live=c->After;gpi_capture=c->Before;
    gpi_capture.RawSmr[0]=gpi_capture.RawS2cr[0]=0;
    if(Case==1)gpi_capture.RawSmr[1]^=1;
    if(Case==2)gpi_capture.RawSmr[0]=0x123;
    if(Case==3)gpi_live.Device[2].Ttbr0^=4096;
    if(Case==4)gpi_live.Device[0].Tcr^=1;
    if(Case==5)gpi_live.Groups=3;
    if(Case==6)gpi_quiet_status=EFI_DEVICE_ERROR;
    if(Case==7)gpi_live.Device[2].Tcr^=1;
    unsigned Freed=releases,Destroyed=destroys,Detached=detaches;
    if(!Case || Case>=3) {
      c->Mapping[0].Used=TRUE;
      assert(PianoOwnedSmmuCheckGpi(c)==(!Case?EFI_SUCCESS:EFI_COMPROMISED_DATA));
      assert(releases==Freed && destroys==Destroyed && detaches==Detached && !route_writes);
      c->Mapping[0].Used=FALSE;
      // A rejected check quarantines the table; only the successful check may
      // continue into the normal unmap, free and detach path below.
      if(Case>=3) {
        assert(c->TableMemory.Quarantined && PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED);
        continue;
      }
    }
    if(!Case) {
      assert(PianoOwnedSmmuClose(c)==EFI_SUCCESS && route_writes==2);
      assert(gpi_capture.RawSmr[0]==0x5B6 && gpi_capture.RawS2cr[0]==0x20001);
      assert(releases==Freed+1 && destroys==Destroyed+1);
    } else {
      assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && !route_writes);
      assert(c->TableMemory.Quarantined && releases==Freed && destroys==Destroyed);
    }
  }
  for(UINT32 Case=0;Case<5;++Case) {
    memset(c,0,sizeof(*c));route_writes=qup_detaches=0;qup_detach_failure=Case==3;
    gpi_quiet_status=EFI_SUCCESS;
    c->DeviceIndex=2;c->ResourceName="QUP1";c->QupAttached=TRUE;
    c->Attached=c->Verified=c->OwnedIdentitySaved=TRUE;c->Api=functions;c->Domain=(VOID *)123;
    c->TableMemory.Signature=1;c->OwnedTablePhysical=0x82000000;
    c->Before.Valid=TRUE;c->Before.Groups=3;c->Before.Base=0x15000000;c->Before.Window=0x100000;
    c->Before.ContextBase=0x80000;c->Before.PageShift=12;c->Before.Banks=83;
    c->Before.RawSmr[1]=0x80000060;c->Before.RawS2cr[0]=c->Before.RawS2cr[2]=0x300006E;
    c->After=c->Before;
    for(UINTN I=2;I<4;++I) {
      UINT16 Slot=I==2?0:2,Sid=I==2?0xB6:0xA3;
      c->After.Device[I]=(PIANO_SMMU_DEVICE){.Sid=Sid,.StreamIndex=Slot,.ContextBank=2,.Present=TRUE,
        .Enabled=TRUE,.Smr=0x80000000U|Sid,.S2cr=2,.Sctlr=0x1E5,.Cba2r=1,
        .Tcr=0x802519,.Tcr2=0x38001,.Mair0=0xFF,.Ttbr0=0x82000000};
      c->After.RawSmr[Slot]=c->After.Device[I].Smr;c->After.RawS2cr[Slot]=2;
    }
    c->AttachedSnapshot=gpi_live=c->After;gpi_capture=c->Before;
    gpi_capture.RawS2cr[0]=gpi_capture.RawS2cr[2]=0;
    if(!Case){
      c->TableMemory.Physical=c->OwnedTablePhysical;
      mActiveGpi=c;
      PIANO_SMMU_SNAPSHOT Expected=c->Before;
      assert(PianoOwnedSmmuExpectActiveGpi(&Expected)==EFI_SUCCESS && PeerSnapshot(&Expected,&c->AttachedSnapshot));
    }
    if(Case==1)gpi_live.Device[3].Ttbr0^=4096;
    if(Case==2)gpi_live.RawSmr[2]^=1;
    if(Case==4)gpi_capture.Device[3]=c->After.Device[3];
    unsigned Freed=releases,Destroyed=destroys,Detached=detaches;
    EFI_STATUS Status=PianoOwnedSmmuClose(c);
    if(!Case) {
      assert(Status==EFI_SUCCESS && qup_detaches==1 && detaches==Detached+1 && route_writes==4 && !mActiveGpi);
      assert(releases==Freed+1 && destroys==Destroyed+1);
      assert(!memcmp(c->Before.RawSmr,gpi_capture.RawSmr,sizeof(gpi_capture.RawSmr)));
      assert(!memcmp(c->Before.RawS2cr,gpi_capture.RawS2cr,sizeof(gpi_capture.RawS2cr)));
    } else {
      assert(Status!=EFI_SUCCESS && releases==Freed && destroys==Destroyed && !route_writes);
      if(Case==1 || Case==2)assert(!qup_detaches && detaches==Detached);
      if(Case==3)assert(qup_detaches==1 && detaches==Detached);
    }
  }
  free(memory);free(c);puts("Owned SMMU backend: nonidentity arena, alignment, per-direction PTEs, unmap and retained rollback failure passed.");return 0;
}
