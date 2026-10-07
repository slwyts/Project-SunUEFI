// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoUfsBlockIo.c"
static UINT8 disk[256*512];static unsigned commands,writes,flushes;
static int fail,short_read,bad_target;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI AllocateAlignedPages(UINTN Pages,UINTN Align){
  void *p=NULL;assert(posix_memalign(&p,Align,Pages*EFI_PAGE_SIZE)==0);return p;
}
VOID EFIAPI FreeAlignedPages(VOID *P,UINTN Pages){assert(Pages==1);free(P);}
static EFI_STATUS EFIAPI pass(EFI_EXT_SCSI_PASS_THRU_PROTOCOL *T,UINT8 *Target,
                             UINT64 Lun,EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *P,EFI_EVENT Event) {
  assert(!Event && Lun==0 && P->Timeout==50000000);
  for(unsigned i=0;i<TARGET_MAX_BYTES;++i)assert(Target[i]==0);
  ++commands;if(fail)return EFI_TIMEOUT;
  UINT8 *c=P->Cdb;P->TargetStatus=bad_target?2:0;
  if(c[0]==0x9e){assert(c[1]==0x10 && P->CdbLength==16 && P->InTransferLength==32);
    Be(P->InDataBuffer,255,8);Be((UINT8 *)P->InDataBuffer+8,512,4);return EFI_SUCCESS;}
  if(c[0]==0x35){assert(P->CdbLength==10 && !P->OutTransferLength && !P->InTransferLength);
    ++flushes;return EFI_SUCCESS;}
  assert(c[0]==0x88 || c[0]==0x8a);assert(P->CdbLength==16);
  UINT64 l=FromBe(c+2,8),blocks=FromBe(c+10,4);
  assert(l<256 && blocks>0 && l+blocks<=256 && blocks*512<=65536);
  if(c[0]==0x88){assert(P->DataDirection==0 && P->InTransferLength==blocks*512 && !P->OutTransferLength);
    memcpy(P->InDataBuffer,disk+l*512,blocks*512);if(short_read)--P->InTransferLength;
  } else {assert(P->DataDirection==1 && c[1]==8 && P->OutTransferLength==blocks*512 && !P->InTransferLength);
    memcpy(disk+l*512,P->OutDataBuffer,blocks*512);++writes;
  }
  return EFI_SUCCESS;
}
int main(void){
  EFI_EXT_SCSI_PASS_THRU_MODE mode={.IoAlign=1};
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL transport={.Mode=&mode,.PassThru=pass};
  PIANO_UFS_BLOCK d;UINT8 *data=malloc(81920),*out=malloc(81920);assert(data && out);
  memset(data,0xa5,81920);memset(disk,0x33,sizeof(disk));
  assert(PianoUfsBlockInit(&d,&transport,0xc4)==EFI_INVALID_PARAMETER); // RPMB never addressed.
  assert(PianoUfsBlockInit(&d,&transport,0)==EFI_SUCCESS && d.Media.ReadOnly && d.Media.BlockSize==512);
  unsigned before=commands;d.Media.ReadOnly=FALSE;
  assert(d.Block.WriteBlocks(&d.Block,1,40,512,data)==EFI_WRITE_PROTECTED && commands==before);
  assert(PianoUfsEnableWrites(&d,0,30)==EFI_INVALID_PARAMETER);
  assert(PianoUfsEnableWrites(&d,34,255)==EFI_INVALID_PARAMETER);
  assert(PianoUfsEnableWrites(&d,34,222)==EFI_SUCCESS);
  assert(d.Block.WriteBlocks(&d.Block,1,40,81920,data)==EFI_SUCCESS && writes==2);
  assert(d.Block.ReadBlocks(&d.Block,1,40,81920,out)==EFI_SUCCESS && memcmp(data,out,81920)==0);
  for(unsigned i=0;i<40*512;++i)assert(disk[i]==0x33);
  for(unsigned i=200*512;i<sizeof(disk);++i)assert(disk[i]==0x33);
  before=commands;
  assert(d.Block.WriteBlocks(&d.Block,1,220,2048,data)==EFI_WRITE_PROTECTED && commands==before);
  assert(d.Block.WriteBlocks(&d.Block,1,255,1024,data)==EFI_INVALID_PARAMETER && commands==before);
  assert(d.Block.WriteBlocks(&d.Block,1,40,513,data)==EFI_BAD_BUFFER_SIZE && commands==before);
  assert(d.Block.ReadBlocks(&d.Block,2,40,512,out)==EFI_MEDIA_CHANGED);
  assert(d.Block.ReadBlocks(&d.Block,1,MAX_UINT64,512,out)==EFI_INVALID_PARAMETER);
  assert(d.Block.FlushBlocks(&d.Block)==EFI_SUCCESS && flushes==1);
  short_read=1;assert(d.Block.ReadBlocks(&d.Block,1,40,512,out)==EFI_DEVICE_ERROR);short_read=0;
  bad_target=1;assert(d.Block.ReadBlocks(&d.Block,1,40,512,out)==EFI_DEVICE_ERROR);bad_target=0;
  fail=1;assert(d.Block.ReadBlocks(&d.Block,1,40,512,out)==EFI_TIMEOUT);fail=0;
  assert(d.Block.Reset(&d.Block,TRUE)==EFI_UNSUPPORTED);
  PianoUfsDisableWrites(&d);before=commands;
  assert(d.Block.WriteBlocks(&d.Block,1,40,512,data)==EFI_WRITE_PROTECTED && commands==before);
  assert(d.Block.FlushBlocks(&d.Block)==EFI_SUCCESS && commands==before);
  free(data);free(out);
  puts("UFS block layer: RAM disk multi-chunk write/readback, FUA, flush, GPT boundary, denied write ranges and transport errors passed. Physical UFS not accessed.");
  return 0;
}
