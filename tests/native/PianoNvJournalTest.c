// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoNvFvb.h"
#include <Library/BaseMemoryLib.h>
static UINT8 disk[2][768][4096],stable[2][768][4096];
static UINTN operations,cut,writes,flushes;
static BOOLEAN runtime_io_forbidden;
static BOOLEAN torn_write;
VOID *EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID *EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}VOID *EFIAPI SetMem(VOID*p,UINTN n,UINT8 v){return memset(p,v,n);}INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
static EFI_STATUS step(VOID){assert(!runtime_io_forbidden);++operations;return cut&&operations==cut?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS read_io(VOID*c,UINT32 s,UINT32 b,UINTN n,VOID*out){assert(c==disk&&s<2&&b<768&&n==4096);EFI_STATUS e=step();if(e)return e;memcpy(out,disk[s][b],n);return EFI_SUCCESS;}
static EFI_STATUS write_io(VOID*c,UINT32 s,UINT32 b,UINTN n,CONST VOID*in){assert(c==disk&&s<2&&b<768&&n==4096);EFI_STATUS e=step();if(e){if(torn_write)memcpy(disk[s][b],in,n/2);return e;}++writes;memcpy(disk[s][b],in,n);return EFI_SUCCESS;}
static EFI_STATUS flush_io(VOID*c){assert(c==disk);EFI_STATUS e=step();if(!e)++flushes;return e;}
static PIANO_UFS_PRODUCT_NV_IO io={.Context=disk,.VolumeUuid={1,2,3,4},.LayoutId=1,.Read=read_io,.Write=write_io,.Flush=flush_io};
static VOID bind(PIANO_NV_JOURNAL*j,UINT8*m,UINT8*s){memset(j,0,sizeof(*j));assert(PianoNvJournalBind(j,&io,m,s)==EFI_SUCCESS);}
static UINTN conversions;static EFI_STATUS EFIAPI convert(UINTN flags,VOID**p){assert(p&&*p);++conversions;return EFI_SUCCESS;}
static VOID w32(UINT8*p,UINT32 v){for(UINTN i=0;i<4;++i)p[i]=(UINT8)(v>>(i*8));}
static VOID checksum(UINT8*p){w32(p+24,0);w32(p+24,PianoNvJournalCrc(p,4096));}
int main(void){
 UINT8*m=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES),*s=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES),*candidate=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES);assert(m&&s&&candidate);
 PIANO_NV_JOURNAL*j=calloc(1,sizeof(*j));assert(j);
 bind(j,m,s);assert(PianoNvJournalCommit(j,candidate)==EFI_NOT_READY&&!writes);assert(PianoNvJournalRecover(j)==EFI_NOT_FOUND&&!writes);
 assert(PianoNvFvbBuildBlank(candidate,PIANO_NV_SNAPSHOT_BYTES)==EFI_SUCCESS);assert(PianoNvJournalCommit(j,candidate)==EFI_SUCCESS&&j->Sequence==1&&j->ActiveSlot==0);memcpy(stable,disk,sizeof(disk));
 candidate[4096+10]=0x7a;operations=0;assert(PianoNvJournalCommit(j,candidate)==EFI_SUCCESS);UINTN complete=operations;assert(j->Sequence==2&&j->ActiveSlot==1&&!memcmp(m,candidate,PIANO_NV_SNAPSHOT_BYTES));
 // Every callback interruption keeps at least the previous complete snapshot;
 // a committed-but-unacknowledged new snapshot is also a valid recovery result.
 for(UINTN torn=0;torn<2;++torn)for(UINTN n=1;n<=complete;++n){memcpy(disk,stable,sizeof(disk));cut=0;torn_write=torn!=0;bind(j,m,s);assert(PianoNvJournalRecover(j)==EFI_SUCCESS&&j->Sequence==1);operations=0;cut=n;
  assert(PianoNvJournalCommit(j,candidate)!=EFI_SUCCESS&&j->Quarantined&&j->Dirty&&!j->Ready);assert(PianoNvJournalCommit(j,candidate)==EFI_ACCESS_DENIED);
  cut=0;bind(j,m,s);assert(PianoNvJournalRecover(j)==EFI_SUCCESS);assert(j->Sequence==1||j->Sequence==2);if(j->Sequence==2)assert(!memcmp(m,candidate,PIANO_NV_SNAPSHOT_BYTES));
 }
 torn_write=FALSE;
 // CRC-valid same-generation different payload is a conflict, not "pick A".
 memcpy(disk,stable,sizeof(disk));bind(j,m,s);assert(PianoNvJournalRecover(j)==EFI_SUCCESS);assert(PianoNvJournalCommit(j,candidate)==EFI_SUCCESS);
 w32(disk[1][0]+40,1);w32(disk[1][0]+44,0);checksum(disk[1][0]);w32(disk[1][767]+40,1);w32(disk[1][767]+44,0);memcpy(disk[1][767]+56,disk[1][0]+24,4);checksum(disk[1][767]);
 bind(j,m,s);assert(PianoNvJournalRecover(j)==EFI_COMPROMISED_DATA&&j->Quarantined&&!j->Ready);assert(PianoNvJournalCommit(j,candidate)==EFI_ACCESS_DENIED);
 // Damaged only copy refuses auto-format and never changes media.
 memcpy(disk,stable,sizeof(disk));disk[0][0][5]^=1;bind(j,m,s);UINTN prior=writes;assert(PianoNvJournalRecover(j)==EFI_VOLUME_CORRUPTED&&j->Quarantined&&writes==prior);
 // Real power cycle is represented by fresh context/empty RAM, same media.
 memcpy(disk,stable,sizeof(disk));bind(j,m,s);memset(m,0,PIANO_NV_SNAPSHOT_BYTES);assert(PianoNvJournalRecover(j)==EFI_SUCCESS&&j->Sequence==1);
 PIANO_NV_FVB f={0};assert(PianoNvFvbInitialize(&f,j)==EFI_SUCCESS);EFI_PHYSICAL_ADDRESS base;assert(f.Protocol.GetPhysicalAddress(&f.Protocol,&base)==EFI_SUCCESS&&base==(UINTN)m);
 UINTN bytes=2;UINT8 value[2]={0xfe,0xfb};assert(f.Protocol.Write(&f.Protocol,1,12,&bytes,value)==EFI_SUCCESS&&m[4096+12]==0xfe);
 value[0]=0xff;bytes=1;UINTN before=writes;assert(f.Protocol.Write(&f.Protocol,1,12,&bytes,value)==EFI_WRITE_PROTECTED&&writes==before);
 assert(f.Protocol.EraseBlocks(&f.Protocol,(EFI_LBA)1,(UINTN)1,EFI_LBA_LIST_TERMINATOR)==EFI_SUCCESS&&m[4096+12]==0xff);
 before=writes;assert(f.Protocol.EraseBlocks(&f.Protocol,(EFI_LBA)1,(UINTN)1,(EFI_LBA)144,(UINTN)1,EFI_LBA_LIST_TERMINATOR)==EFI_INVALID_PARAMETER&&writes==before);
 bytes=3;assert(f.Protocol.Read(&f.Protocol,1,4095,&bytes,value)==EFI_BAD_BUFFER_SIZE&&bytes==1);
 // NV runtime guard blocks create, update and zero-attribute delete, while
 // volatile RT variables remain in the standard driver's responsibility.
 assert(PianoNvRuntimeWriteCheck(TRUE,EFI_VARIABLE_NON_VOLATILE,FALSE)==EFI_UNSUPPORTED);assert(PianoNvRuntimeWriteCheck(TRUE,0,TRUE)==EFI_UNSUPPORTED);assert(PianoNvRuntimeWriteCheck(TRUE,EFI_VARIABLE_RUNTIME_ACCESS,FALSE)==EFI_SUCCESS);assert(PianoNvRuntimeWriteCheck(FALSE,EFI_VARIABLE_NON_VOLATILE,TRUE)==EFI_SUCCESS);
 PianoNvFvbFenceRuntime(&f);runtime_io_forbidden=TRUE;bytes=1;assert(f.Protocol.Write(&f.Protocol,1,0,&bytes,value)==EFI_UNSUPPORTED);assert(f.Protocol.EraseBlocks(&f.Protocol,(EFI_LBA)1,(UINTN)1,EFI_LBA_LIST_TERMINATOR)==EFI_UNSUPPORTED);
 assert(PianoNvFvbConvertVirtual(&f,convert)==EFI_SUCCESS&&conversions==9&&!j->Io.Read&&!j->Io.Write&&!j->Scratch);bytes=1;assert(f.Protocol.Read(&f.Protocol,1,12,&bytes,value)==EFI_SUCCESS);assert(f.Protocol.GetPhysicalAddress(&f.Protocol,&base)==EFI_SUCCESS&&base==(UINTN)m);
 free(j);free(m);free(s);free(candidate);printf("Actual NV journal+FVB passed %lu power-failure boundaries with atomic/torn writes, media recovery, flash semantics, EBS no-IO and VA callbacks\n",(unsigned long)(complete*2));return 0;
}
