// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual provider and GPT gates with synthetic memory-backed reserved media.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUfsProductVolume.h"
#include "../bootprofiles/uefi-app/PianoGpt.h"
#include "PianoUfsWriteTestBaseline.h"
static PIANO_UFS_PRODUCT_VOLUME volume;
static UINT8 disk[14680064],primary[4096],entries[12288],backup[4096],backup_entries[12288],pattern[8192],received[8192];
static PIANO_UFS_PRODUCT_ORIGINAL_GPT original;
static PIANO_UFS_WINDOW_IO io;
static BOOLEAN held,wp,queue_bad,write_error,write_warning,short_write,sync_error,stale_read,release_error;
static UINTN acquires,releases,reads,writes,syncs,cases;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Digest){return SHA256(P,N,Digest)!=NULL;}
static VOID put32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static VOID put64(UINT8 *P,UINT64 V){for(UINTN I=0;I<8;++I)P[I]=(UINT8)(V>>(8*I));}
static EFI_STATUS acquire(VOID *Context){assert(Context==&volume && volume.State.Busy && !held);held=TRUE;++acquires;return EFI_SUCCESS;}
static EFI_STATUS release(VOID *Context,BOOLEAN Quarantined){(void)Quarantined;assert(Context==&volume && !volume.State.Busy && held);held=FALSE;++releases;return release_error?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS guard(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G){assert(Context==&volume && held && Lun==4);*G=(PIANO_UFS_WRITE_GUARD){.Lun=4,.Collected=31,.CapacityBytes=1551892480,.Fua=!wp,.UnitWriteProtect=1};return EFI_SUCCESS;}
static EFI_STATUS quiet(VOID *Context,BOOLEAN *Q){assert(Context==&volume && held);*Q=!queue_bad;return EFI_SUCCESS;}
static EFI_STATUS read(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){
  assert(Context==&volume && Lun==4 && held && Buffer!=volume.Tx);++reads;*Got=Bytes;
  if(Lba==1){assert(Bytes==4096);memcpy(Buffer,primary,Bytes);}
  else if(Lba==2){assert(Bytes==12288);memcpy(Buffer,entries,Bytes);}
  else if(Lba==378879){assert(Bytes==4096);memcpy(Buffer,backup,Bytes);}
  else if(Lba==378873){assert(Bytes==12288);memcpy(Buffer,backup_entries,Bytes);}
  else {assert(Lba>=375040 && Lba<=378623 && Bytes==4096);if(!(stale_read && Lba>=375042))memcpy(Buffer,disk+(Lba-375040)*4096,Bytes);}
  return EFI_SUCCESS;
}
static EFI_STATUS write(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Buffer,UINTN *Done){
  assert(Context==&volume && Lun==4 && held && Lba>=375042 && Lba<=378623 && Bytes==4096 && Buffer==volume.Tx && volume.State.PendingWrite && volume.State.NeedsRecovery);
  ++writes;*Done=short_write?512:4096;memcpy(disk+(Lba-375040)*4096,Buffer,*Done);return write_warning?EFI_WARN_STALE_DATA:write_error?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS sync(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){assert(Context==&volume && held && Lun==4 && Lba==375040 && Bytes==14680064);++syncs;return sync_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static VOID reseal_gpt(VOID){
  memcpy(backup_entries,entries,12288);UINT32 C=PianoGptCrc32(entries,12288);put32(primary+88,C);put32(backup+88,C);
  put32(primary+16,0);put32(backup+16,0);put32(primary+16,PianoGptCrc32(primary,92));put32(backup+16,PianoGptCrc32(backup,92));
}
static VOID header(VOID){
  UINT8 *H=disk;memcpy(H,"PIANO-VOLUME-v1",16);put32(H+16,1);put32(H+20,128);put32(H+28,1);
  memcpy(H+32,entries+95*128+16,16);memcpy(H+48,primary+56,16);EFI_GUID Type=PIANO_PRODUCT_STORAGE_TYPE_GUID;memcpy(H+64,&Type,16);
  put32(H+80,4);put32(H+84,4096);put64(H+88,375040);put32(H+96,3584);put32(H+100,2);put32(H+104,2046);put32(H+108,2048);put32(H+112,2816);put32(H+116,768);put32(H+120,95);
  put32(H+24,0);put32(H+24,PianoGptCrc32(H,4096));memcpy(H+4096,H,4096);
}
static VOID fresh(BOOLEAN Provisioned){
  ++cases;memset(&volume,0,sizeof(volume));memset(disk,0,sizeof(disk));memset(pattern,0x71,sizeof(pattern));
  held=wp=queue_bad=write_error=write_warning=short_write=sync_error=stale_read=release_error=FALSE;acquires=releases=reads=writes=syncs=0;
  memcpy(primary,mPianoUfsWriteTestPrimaryHeader,4096);memcpy(entries,mPianoUfsWriteTestPrimaryEntries,12288);memcpy(backup,mPianoUfsWriteTestBackupHeader,4096);memcpy(backup_entries,entries,12288);
  original=(PIANO_UFS_PRODUCT_ORIGINAL_GPT){.PrimaryHeader={mPianoUfsWriteTestPrimaryHeader,4096},.PrimaryEntries={mPianoUfsWriteTestPrimaryEntries,12288},.BackupHeader={mPianoUfsWriteTestBackupHeader,4096}};
  io=(PIANO_UFS_WINDOW_IO){.Io={&volume,read,write,sync,guard,quiet},.Acquire=acquire,.Release=release};
  if(Provisioned){UINT8 *E=entries+95*128;EFI_GUID Type=PIANO_PRODUCT_STORAGE_TYPE_GUID,Uuid={0x01234567,0x89ab,0x4cde,{0x81,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}};memcpy(E,&Type,16);memcpy(E+16,&Uuid,16);put64(E+32,375040);put64(E+40,378623);put64(E+48,2);const CHAR16 Name[]=L"PianoUEFI Storage";memcpy(E+56,Name,sizeof(Name));reseal_gpt();header();}
}
static VOID open(void){assert(PianoUfsProductVolumeOpen(&volume,&original,&io)==EFI_SUCCESS && volume.State.Provisioned && !volume.Media.ReadOnly && volume.Media.MediaPresent && !writes && !syncs && !held && acquires==releases);}
static VOID fenced(void){assert(volume.State.Quarantined && volume.State.NeedsRecovery && volume.Media.ReadOnly && !held && !PianoUfsProductVolumeNvIo(&volume));UINTN Before=writes;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)!=EFI_SUCCESS && writes==Before && PianoUfsProductVolumeClose(&volume)==EFI_ACCESS_DENIED);}
static void load_file(const char *Directory,const char *Name,void *Buffer,size_t Bytes){
  char Path[4096];int N=snprintf(Path,sizeof(Path),"%s/%s",Directory,Name);assert(N>0&&(size_t)N<sizeof(Path));
  FILE *F=fopen(Path,"rb");assert(F&&fread(Buffer,1,Bytes,F)==Bytes&&fgetc(F)==EOF);fclose(F);
}
int main(int argc,char **argv){
  if(argc==2){
    fresh(FALSE);
    load_file(argv[1],"primary-header.bin",primary,sizeof(primary));
    load_file(argv[1],"primary-entries.bin",entries,sizeof(entries));
    load_file(argv[1],"backup-header.bin",backup,sizeof(backup));
    load_file(argv[1],"backup-entries.bin",backup_entries,sizeof(backup_entries));
    load_file(argv[1],"PianoUEFI-storage.img",disk,sizeof(disk));
    open();assert(!writes&&!syncs&&PianoUfsProductVolumeNvIo(&volume));
    assert(volume.Block.ReadBlocks(&volume.Block,1,0,4096,received)==EFI_SUCCESS);
    assert(received[510]==0x55&&received[511]==0xaa);
    puts("Actual product provider accepted PC-generated GPT/container and read bounded FAT; no device or mutation.");
    return 0;
  }
  assert(argc==1);
  fresh(FALSE);assert(PianoUfsProductVolumeOpen(&volume,&original,&io)==EFI_NOT_FOUND && !volume.State.Provisioned && !volume.State.Quarantined && !writes && !syncs && !PianoUfsProductVolumeNvIo(&volume));
  fresh(TRUE);original.PrimaryHeader.Bytes=92;assert(PianoUfsProductVolumeOpen(&volume,&original,&io)==EFI_SECURITY_VIOLATION && !acquires);
  for(UINTN I=0;I<7;++I){fresh(TRUE);if(I==0)disk[4096+32]^=1;if(I==1){disk[32]^=1;header();disk[32]^=1;}if(I==2)disk[100]^=1;if(I==3)entries[56]^=1;if(I==4)entries[95*128+48]=0;if(I==5)put64(entries+95*128+40,378624);if(I==6)backup[56]^=1;reseal_gpt();assert(PianoUfsProductVolumeOpen(&volume,&original,&io)!=EFI_SUCCESS && !writes && !syncs);fenced();}
  fresh(TRUE);open();UINT8 Meta[8192];memcpy(Meta,disk,8192);assert(volume.Media.LastBlock==2045);
  assert(volume.Block.WriteBlocks(&volume.Block,1,0,8192,pattern)==EFI_SUCCESS && writes==2 && syncs==2 && volume.State.VerifiedWrites==2 && !volume.State.NeedsRecovery && !volume.State.PendingWrite);
  assert(volume.State.LastPhysical==375043 && !memcmp(disk,Meta,8192));assert(volume.Block.ReadBlocks(&volume.Block,1,0,8192,received)==EFI_SUCCESS && !memcmp(pattern,received,8192));
  const PIANO_UFS_PRODUCT_NV_IO *Nv=PianoUfsProductVolumeNvIo(&volume);assert(Nv && Nv->LayoutId==1);
  assert(Nv->Write(Nv->Context,1,767,4096,pattern)==EFI_SUCCESS && volume.State.LastPhysical==378623 && writes==3);
  assert(Nv->Read(Nv->Context,1,767,4096,received)==EFI_SUCCESS && !memcmp(pattern,received,4096));
  UINTN Before=writes;assert(Nv->Write(Nv->Context,2,0,4096,pattern)==EFI_INVALID_PARAMETER && Nv->Write(Nv->Context,0,768,4096,pattern)==EFI_INVALID_PARAMETER && Nv->Write(Nv->Context,1,767,8192,pattern)==EFI_INVALID_PARAMETER && volume.Block.WriteBlocks(&volume.Block,1,2046,4096,pattern)==EFI_INVALID_PARAMETER && writes==Before);
  assert(volume.Block.WriteBlocks(&volume.Block,2,0,4096,pattern)==EFI_MEDIA_CHANGED && volume.Block.WriteBlocks(&volume.Block,1,0,512,pattern)==EFI_BAD_BUFFER_SIZE && volume.Block.WriteBlocks(&volume.Block,1,0,4096,volume.Tx)==EFI_INVALID_PARAMETER);
  assert(Nv->Flush(Nv->Context)==EFI_SUCCESS && syncs==4);assert(PianoUfsProductVolumeClose(&volume)==EFI_SUCCESS && syncs==5 && !PianoUfsProductVolumeNvIo(&volume));assert(Nv->Read(Nv->Context,0,0,4096,received)==EFI_NO_MEDIA);
  for(UINTN I=0;I<8;++I){fresh(TRUE);open();if(I==0)write_error=TRUE;if(I==1)write_warning=TRUE;if(I==2)short_write=TRUE;if(I==3)sync_error=TRUE;if(I==4){stale_read=TRUE;memset(pattern,0xCC,4096);}if(I==5)release_error=TRUE;if(I==6)wp=TRUE;if(I==7)queue_bad=TRUE;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)!=EFI_SUCCESS);fenced();if(I>=6)assert(!writes);}
  fresh(TRUE);open();entries[128+56]^=1;reseal_gpt();assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)==EFI_SECURITY_VIOLATION && !writes);fenced();
  fresh(TRUE);open();disk[4096+32]^=1;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)==EFI_SECURITY_VIOLATION && !writes);fenced();
  printf("Product UFS provider: %llu memory-backed cases PASS; original GPT NOT_FOUND zeroWRITE/SYNC, real reservation/header pins, FAT/NV span isolation, FUA/Sync/inverse-poison RX, successful persistent writes, sticky warnings/faults.\n",(unsigned long long)cases);
  return 0;
}
