// SPDX-License-Identifier: BSD-2-Clause-Patent
// lwext4-backed persist. Only the boot-service variable store is writable.
#include "PianoExt4.h"
#include <Protocol/PartitionInfo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <ext4.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_fs.h>
#include <stdlib.h>
#include <string.h>

#define VOL_SIG SIGNATURE_32('P','E','X','4')
#define FILE_SIG SIGNATURE_32('E','X','4','F')
typedef struct {
  UINT32 Signature;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Sfs;
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DISK_IO_PROTOCOL *Disk;
  PIANO_PARTITION_IO_PROTOCOL *Io;
  UINT32 MediaId;
  CHAR8 Mount[24],Device[24];
  struct ext4_blockdev Dev;
  struct ext4_blockdev_iface Iface;
  UINT8 BlockBuffer[4096];
  BOOLEAN Writable,Registered,Mounted,Journal,Failed,Busy;
  UINTN OpenFiles;
} VOLUME;
typedef struct {
  UINT32 Signature;
  EFI_FILE_PROTOCOL File;
  VOLUME *Volume;
  CHAR8 Path[1024];
  BOOLEAN Directory,Writing;
  ext4_file Data;
  ext4_dir Dir;
} FILE_HANDLE;
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding;
STATIC EFI_GUID mIoGuid=PIANO_PARTITION_IO_PROTOCOL_GUID;
STATIC UINT32 mVolumeId;
STATIC BOOLEAN mExited;
STATIC EFI_EVENT mExit;
STATIC VOLUME *mVolumes[16];
STATIC EFI_FILE_PROTOCOL mFileTemplate;
STATIC EFI_STATUS Status(int R){
  switch(R){case EOK:return EFI_SUCCESS;case ENOENT:return EFI_NOT_FOUND;case ENOMEM:return EFI_OUT_OF_RESOURCES;
    case EROFS:return EFI_WRITE_PROTECTED;case ENOSPC:return EFI_VOLUME_FULL;case EINVAL:return EFI_INVALID_PARAMETER;
    case EACCES:case EPERM:return EFI_ACCESS_DENIED;case ENOTSUP:return EFI_UNSUPPORTED;default:return EFI_DEVICE_ERROR;}
}
STATIC FILE_HANDLE *File(EFI_FILE_PROTOCOL *F){
  if(!F)return NULL;FILE_HANDLE *H=BASE_CR(F,FILE_HANDLE,File);return H->Signature==FILE_SIG?H:NULL;
}
STATIC BOOLEAN Live(VOLUME *V){return V&&V->Mounted&&!V->Failed&&!mExited&&V->Block&&V->Block->Media&&
  V->Block->Media->MediaPresent&&V->Block->Media->MediaId==V->MediaId;}
STATIC int OpenDevice(struct ext4_blockdev *D){VOLUME *V=D->bdif->p_user;return !mExited&&V->Block->Media->MediaId==V->MediaId?0:EIO;}
STATIC int CloseDevice(struct ext4_blockdev *D){(VOID)D;return 0;}
STATIC int ReadDevice(struct ext4_blockdev *D,void *Buffer,uint64_t At,uint32_t Count){
  VOLUME *V=D->bdif->p_user;if(mExited||V->Failed||At>D->bdif->ph_bcnt||Count>D->bdif->ph_bcnt-At)return EIO;
  EFI_STATUS S=V->Block->ReadBlocks(V->Block,V->MediaId,At,(UINTN)Count*4096,Buffer);
  if(S!=EFI_SUCCESS){V->Failed=TRUE;return EIO;}return 0;
}
STATIC int WriteDevice(struct ext4_blockdev *D,const void *Buffer,uint64_t At,uint32_t Count){
  VOLUME *V=D->bdif->p_user;
  if(!V->Writable||!V->Io)return EROFS;
  if(mExited||V->Failed||At>D->bdif->ph_bcnt||Count>D->bdif->ph_bcnt-At)return EIO;
  EFI_STATUS S=V->Io->Write(V->Io,V->Handle,At,(UINTN)Count*4096,Buffer);
  // lwext4 writes journal commits synchronously. Every physical write is FUA;
  // also complete the owner's cache barrier before returning to its journal.
  if(S==EFI_SUCCESS)S=V->Io->Flush(V->Io,V->Handle);
  if(S!=EFI_SUCCESS){V->Failed=TRUE;return EIO;}return 0;
}
STATIC EFI_STATUS Enter(VOLUME *V){
  if(!Live(V))return EFI_DEVICE_ERROR;
  if(V->Busy)return EFI_NOT_READY;
  EFI_TPL T=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(T);
  if(T>TPL_CALLBACK)return EFI_UNSUPPORTED;V->Busy=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS Leave(VOLUME *V,int R){V->Busy=FALSE;return V->Failed?EFI_DEVICE_ERROR:Status(R);}
STATIC EFI_STATUS Barrier(VOLUME *V){
  EFI_STATUS S=V->Io->Flush(V->Io,V->Handle);if(S!=EFI_SUCCESS)V->Failed=TRUE;return S;
}
STATIC EFI_STATUS FlushVolume(VOLUME *V){
  if(!Live(V))return EFI_DEVICE_ERROR;if(!V->Writable)return EFI_SUCCESS;
  int R=ext4_cache_flush(V->Mount);if(R)return Status(R);
  return Barrier(V);
}
STATIC BOOLEAN NvPath(CONST CHAR8 *Path){
  return !AsciiStrCmp(Path,"sunuefi")||!AsciiStrCmp(Path,"sunuefi/nvram-a.bin")||
    !AsciiStrCmp(Path,"sunuefi/nvram-b.bin");
}
STATIC BOOLEAN WritablePath(VOLUME *V,CONST CHAR8 *Path){
  return V->Writable&&NvPath(Path+AsciiStrLen(V->Mount));
}
STATIC BOOLEAN ReadablePath(VOLUME *V,CONST CHAR8 *Path){
  CONST CHAR8 *Name=Path+AsciiStrLen(V->Mount);
  return !*Name||NvPath(Name)||!AsciiStrCmp(Name,"bluetooth/.bt_nv.bin")||!AsciiStrCmp(Name,"time/ats_2");
}
STATIC EFI_STATUS Path(FILE_HANDLE *H,CONST CHAR16 *Name,CHAR8 Out[1024]){
  UINTN N=0;
  if(Name[0]!='\\'&&Name[0]!='/'){
    N=AsciiStrLen(H->Path);if(N+1>=1024)return EFI_BAD_BUFFER_SIZE;CopyMem(Out,H->Path,N);if(Out[N-1]!='/')Out[N++]='/';
  }else{N=AsciiStrLen(H->Volume->Mount);CopyMem(Out,H->Volume->Mount,N);}
  while(*Name){while(*Name=='\\'||*Name=='/')++Name;if(!*Name)break;
    CHAR8 Part[256];UINTN L=0;while(*Name&&*Name!='\\'&&*Name!='/'){
      if(*Name>127||L>=255)return EFI_UNSUPPORTED;Part[L++]=(CHAR8)*Name++;}Part[L]=0;
    if(!AsciiStrCmp(Part,"."))continue;
    if(!AsciiStrCmp(Part,"..")){
      UINTN Min=AsciiStrLen(H->Volume->Mount);if(N>Min){if(Out[N-1]=='/')--N;while(N>Min&&Out[N-1]!='/')--N;}continue;
    }
    if(N&&Out[N-1]!='/')Out[N++]='/';if(L>=1024-N)return EFI_BAD_BUFFER_SIZE;CopyMem(Out+N,Part,L);N+=L;
  }Out[N]=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS NewFile(VOLUME *V,CONST CHAR8 *P,UINT64 Mode,BOOLEAN Directory,EFI_FILE_PROTOCOL **Out){
  if((Mode&EFI_FILE_MODE_WRITE)&&!WritablePath(V,P))return EFI_WRITE_PROTECTED;
  FILE_HANDLE *H=AllocateZeroPool(sizeof(*H));if(!H)return EFI_OUT_OF_RESOURCES;
  H->Signature=FILE_SIG;H->File=mFileTemplate;H->Volume=V;H->Directory=Directory;H->Writing=(Mode&EFI_FILE_MODE_WRITE)!=0;
  AsciiStrCpyS(H->Path,sizeof(H->Path),P);int R=Directory?ext4_dir_open(&H->Dir,P):ext4_fopen(&H->Data,P,H->Writing?"r+":"r");
  if(R==ENOENT&&(Mode&EFI_FILE_MODE_CREATE)&&WritablePath(V,P)){
    if(Directory){R=ext4_dir_mk(P);if(!R)R=ext4_dir_open(&H->Dir,P);}
    else R=ext4_fopen(&H->Data,P,"w+");
  }
  if(R||V->Failed){FreePool(H);return V->Failed?EFI_DEVICE_ERROR:Status(R);}
  ++V->OpenFiles;*Out=&H->File;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **Out,CHAR16 *Name,UINT64 Mode,UINT64 Attr){
  FILE_HANDLE *H=File(This);if(!H||!Out||!Name||!(Mode&EFI_FILE_MODE_READ)||!H->Directory)return EFI_INVALID_PARAMETER;
  VOLUME *V=H->Volume;EFI_STATUS S=Enter(V);if(S!=EFI_SUCCESS)return S;CHAR8 P[1024];
  S=Path(H,Name,P);if(S!=EFI_SUCCESS){V->Busy=FALSE;return S;}
  if(!ReadablePath(V,P)){V->Busy=FALSE;return EFI_ACCESS_DENIED;}
  CONST CHAR8 *Relative=P+AsciiStrLen(V->Mount);
  BOOLEAN Directory=!*Relative||!AsciiStrCmp(Relative,"sunuefi");
  if((Mode&EFI_FILE_MODE_CREATE)&&Directory!=((Attr&EFI_FILE_DIRECTORY)!=0)){
    V->Busy=FALSE;
    return EFI_INVALID_PARAMETER;
  }
  struct ext4_inode Inode;
  int R=ext4_raw_inode_fill(P,NULL,&Inode);
  if(!R){
    UINT16 Type=to_le16(Inode.mode)&0xF000;
    if(Type!=(Directory?0x4000:0x8000)||((Mode&EFI_FILE_MODE_WRITE)&&!Directory&&to_le16(Inode.links_count)!=1)){
      V->Busy=FALSE;
      return EFI_ACCESS_DENIED;
    }
  }else if(R!=ENOENT){V->Busy=FALSE;return Status(R);}
  S=NewFile(V,P,Mode,Directory,Out);V->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This){
  FILE_HANDLE *H=File(This);if(!H)return EFI_INVALID_PARAMETER;VOLUME *V=H->Volume;
  if(mExited)return EFI_UNSUPPORTED;int R=H->Directory?ext4_dir_close(&H->Dir):ext4_fclose(&H->Data);
  if(V->OpenFiles)--V->OpenFiles;H->Signature=0;FreePool(H);return Status(R);
}
STATIC EFI_STATUS EFIAPI Delete(EFI_FILE_PROTOCOL *This){
  FILE_HANDLE *H=File(This);if(!H)return EFI_INVALID_PARAMETER;EFI_STATUS S=EFI_WRITE_PROTECTED;
  if(H->Writing&&WritablePath(H->Volume,H->Path)&&!H->Directory){
    S=Enter(H->Volume);if(S==EFI_SUCCESS){int R=ext4_fremove(H->Path);S=Leave(H->Volume,R);}}
  EFI_STATUS C=Close(This);return S==EFI_SUCCESS?C:EFI_WARN_DELETE_FAILURE;
}
STATIC EFI_STATUS FileInfo(VOLUME *V,CONST CHAR8 *Path,CONST CHAR8 *Name,UINTN *Bytes,VOID *Buffer){
  UINTN Len=AsciiStrLen(Name),Need=SIZE_OF_EFI_FILE_INFO+(Len+1)*2;
  if(*Bytes<Need){*Bytes=Need;return EFI_BUFFER_TOO_SMALL;}*Bytes=Need;
  struct ext4_inode Inode;int R=ext4_raw_inode_fill(Path,NULL,&Inode);if(R)return Status(R);
  EFI_FILE_INFO *Info=Buffer;ZeroMem(Info,Need);Info->Size=Need;Info->FileSize=(UINT64)Inode.size_lo|((UINT64)Inode.size_hi<<32);
  Info->PhysicalSize=(UINT64)Inode.blocks_count_lo*512;
  if((Inode.mode&0xF000)==0x4000)Info->Attribute|=EFI_FILE_DIRECTORY;
  if(!WritablePath(V,Path))Info->Attribute|=EFI_FILE_READ_ONLY;
  AsciiStrToUnicodeStrS(Name,Info->FileName,Len+1);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This,UINTN *Bytes,VOID *Buffer){
  FILE_HANDLE *H=File(This);if(!H||!Bytes||(!Buffer&&*Bytes))return EFI_INVALID_PARAMETER;VOLUME *V=H->Volume;
  EFI_STATUS S=Enter(V);if(S!=EFI_SUCCESS)return S;
  if(!H->Directory){size_t Done=0;int R=ext4_fread(&H->Data,Buffer,*Bytes,&Done);*Bytes=Done;return Leave(V,R);}
  UINT64 Before=H->Dir.next_off;CONST ext4_direntry *D=ext4_dir_entry_next(&H->Dir);
  while(D&&((D->name_length==1&&D->name[0]=='.')||(D->name_length==2&&D->name[0]=='.'&&D->name[1]=='.')))D=ext4_dir_entry_next(&H->Dir);
  if(!D){*Bytes=0;V->Busy=FALSE;return V->Failed?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  CHAR8 Name[256],P[1024];CopyMem(Name,D->name,D->name_length);Name[D->name_length]=0;
  UINTN A=AsciiStrLen(H->Path),B=AsciiStrLen(Name);if(A+B+2>sizeof(P)){V->Busy=FALSE;return EFI_BAD_BUFFER_SIZE;}
  CopyMem(P,H->Path,A);if(P[A-1]!='/')P[A++]='/';CopyMem(P+A,Name,B+1);
  S=FileInfo(V,P,Name,Bytes,Buffer);if(S==EFI_BUFFER_TOO_SMALL)H->Dir.next_off=Before;
  V->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *This,UINTN *Bytes,VOID *Buffer){
  FILE_HANDLE *H=File(This);if(!H||!Bytes||(!Buffer&&*Bytes))return EFI_INVALID_PARAMETER;
  if(!H->Writing||H->Directory||!WritablePath(H->Volume,H->Path))return EFI_ACCESS_DENIED;
  EFI_STATUS S=Enter(H->Volume);if(S!=EFI_SUCCESS)return S;size_t Done=0;
  int R=ext4_fwrite(&H->Data,Buffer,*Bytes,&Done);UINTN Wanted=*Bytes;*Bytes=Done;
  S=Leave(H->Volume,R);if(S==EFI_SUCCESS&&Done!=Wanted)S=EFI_DEVICE_ERROR;
  if(S==EFI_SUCCESS)S=FlushVolume(H->Volume);return S;
}
STATIC EFI_STATUS EFIAPI Position(EFI_FILE_PROTOCOL *This,UINT64 *At){FILE_HANDLE *H=File(This);if(!H||!At)return EFI_INVALID_PARAMETER;*At=H->Directory?H->Dir.next_off:H->Data.fpos;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Seek(EFI_FILE_PROTOCOL *This,UINT64 At){
  FILE_HANDLE *H=File(This);if(!H)return EFI_INVALID_PARAMETER;if(!Live(H->Volume))return EFI_DEVICE_ERROR;
  if(H->Directory){if(At)return EFI_UNSUPPORTED;ext4_dir_entry_rewind(&H->Dir);return EFI_SUCCESS;}
  if(At==MAX_UINT64)At=H->Data.fsize;if(At>H->Data.fsize)return EFI_UNSUPPORTED;H->Data.fpos=At;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN *Bytes,VOID *Buffer){
  FILE_HANDLE *H=File(This);if(!H||!Guid||!Bytes)return EFI_INVALID_PARAMETER;VOLUME *V=H->Volume;EFI_STATUS S=Enter(V);if(S!=EFI_SUCCESS)return S;
  if(CompareGuid(Guid,&gEfiFileInfoGuid)){
    CONST CHAR8 *Name=H->Path;for(CONST CHAR8 *P=Name;*P;++P)if(*P=='/')Name=P+1;S=FileInfo(V,H->Path,Name,Bytes,Buffer);
  }else if(CompareGuid(Guid,&gEfiFileSystemInfoGuid)){
    UINTN Need=SIZE_OF_EFI_FILE_SYSTEM_INFO+sizeof(L"persist");if(*Bytes<Need){*Bytes=Need;S=EFI_BUFFER_TOO_SMALL;}
    else{struct ext4_mount_stats Stats;int R=ext4_mount_point_stats(V->Mount,&Stats);S=Status(R);if(!R){EFI_FILE_SYSTEM_INFO *I=Buffer;ZeroMem(I,Need);I->Size=Need;I->ReadOnly=!V->Writable;I->BlockSize=Stats.block_size;I->VolumeSize=Stats.blocks_count*Stats.block_size;I->FreeSpace=Stats.free_blocks_count*Stats.block_size;StrCpyS(I->VolumeLabel,8,L"persist");*Bytes=Need;}}
  }else if(CompareGuid(Guid,&gEfiFileSystemVolumeLabelInfoIdGuid)){
    UINTN Need=sizeof(L"persist");if(*Bytes<Need){*Bytes=Need;S=EFI_BUFFER_TOO_SMALL;}else{CopyMem(Buffer,L"persist",Need);*Bytes=Need;S=EFI_SUCCESS;}
  }else S=EFI_UNSUPPORTED;V->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI SetInfo(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN Bytes,VOID *Buffer){
  FILE_HANDLE *H=File(This);if(!H||!Guid||!Buffer)return EFI_INVALID_PARAMETER;
  if(!H->Writing||!WritablePath(H->Volume,H->Path))return EFI_WRITE_PROTECTED;
  if(!CompareGuid(Guid,&gEfiFileInfoGuid)||Bytes<SIZE_OF_EFI_FILE_INFO+2||H->Directory)return EFI_UNSUPPORTED;
  EFI_FILE_INFO *I=Buffer;if(I->FileSize>H->Data.fsize)return EFI_UNSUPPORTED;
  EFI_STATUS S=Enter(H->Volume);if(S!=EFI_SUCCESS)return S;int R=ext4_ftruncate(&H->Data,I->FileSize);S=Leave(H->Volume,R);
  if(S==EFI_SUCCESS)S=FlushVolume(H->Volume);return S;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *This){FILE_HANDLE *H=File(This);return H?FlushVolume(H->Volume):EFI_INVALID_PARAMETER;}
STATIC EFI_FILE_PROTOCOL mFileTemplate={.Revision=EFI_FILE_PROTOCOL_REVISION,.Open=Open,.Close=Close,.Delete=Delete,.Read=Read,.Write=Write,
  .GetPosition=Position,.SetPosition=Seek,.GetInfo=Info,.SetInfo=SetInfo,.Flush=Flush};
STATIC EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Out){
  if(!This||!Out)return EFI_INVALID_PARAMETER;*Out=NULL;
  VOLUME *V=BASE_CR(This,VOLUME,Sfs);EFI_STATUS S=Enter(V);if(S!=EFI_SUCCESS)return S;
  S=NewFile(V,V->Mount,EFI_FILE_MODE_READ,TRUE,Out);V->Busy=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Supported(EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path){
  (VOID)Path;EFI_PARTITION_INFO_PROTOCOL *P=NULL;EFI_BLOCK_IO_PROTOCOL *Io=NULL;
  if(mExited||gBS->HandleProtocol(Handle,&gEfiPartitionInfoProtocolGuid,(VOID **)&P)!=EFI_SUCCESS||!P||P->Type!=PARTITION_TYPE_GPT||
     StrnCmp(P->Info.Gpt.PartitionName,L"persist",36))return EFI_UNSUPPORTED;
  if(gBS->HandleProtocol(Handle,&gEfiBlockIoProtocolGuid,(VOID **)&Io)!=EFI_SUCCESS||!Io||!Io->Media||Io->Media->BlockSize!=4096)return EFI_UNSUPPORTED;
  EFI_DISK_IO_PROTOCOL *Disk=NULL;EFI_STATUS S=gBS->OpenProtocol(Handle,&gEfiDiskIoProtocolGuid,(VOID **)&Disk,B->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if(S==EFI_SUCCESS)gBS->CloseProtocol(Handle,&gEfiDiskIoProtocolGuid,B->DriverBindingHandle,Handle);
  return S;
}
STATIC BOOLEAN CleanPersist(VOLUME *V){
  struct ext4_sblock Super;
  if(ReadDevice(&V->Dev,V->BlockBuffer,0,1))return FALSE;
  CopyMem(&Super,V->BlockBuffer+EXT4_SUPERBLOCK_OFFSET,sizeof(Super));
  return ext4_sb_check(&Super)&&to_le16(Super.state)==EXT4_SUPERBLOCK_STATE_VALID_FS&&
    !ext4_sb_feature_incom(&Super,EXT4_FINCOM_RECOVER);
}
STATIC EFI_STATUS EFIAPI Start(EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path){
  (VOID)Path;UINTN Slot=0;while(Slot<ARRAY_SIZE(mVolumes)&&mVolumes[Slot])++Slot;if(Slot==ARRAY_SIZE(mVolumes))return EFI_OUT_OF_RESOURCES;
  VOLUME *V=AllocateZeroPool(sizeof(*V));if(!V)return EFI_OUT_OF_RESOURCES;
  EFI_STATUS S=gBS->OpenProtocol(Handle,&gEfiDiskIoProtocolGuid,(VOID **)&V->Disk,B->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if(S==EFI_SUCCESS){S=gBS->HandleProtocol(Handle,&gEfiBlockIoProtocolGuid,(VOID **)&V->Block);
    if(S!=EFI_SUCCESS)gBS->CloseProtocol(Handle,&gEfiDiskIoProtocolGuid,B->DriverBindingHandle,Handle);}
  if(S!=EFI_SUCCESS){FreePool(V);return S;}V->Handle=Handle;V->MediaId=V->Block->Media->MediaId;
  V->Signature=VOL_SIG;V->Sfs=(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL){EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION,OpenVolume};
  S=gBS->LocateProtocol(&mIoGuid,NULL,(VOID **)&V->Io);V->Writable=S==EFI_SUCCESS&&V->Io&&V->Io->Revision==2&&
    V->Io->CanWrite(V->Io,Handle)==EFI_SUCCESS;
  UINT32 Id=mVolumeId++;AsciiSPrint(V->Mount,sizeof(V->Mount),"/ext4-%u/",Id);AsciiSPrint(V->Device,sizeof(V->Device),"ext4-%u",Id);
  V->Iface=(struct ext4_blockdev_iface){.open=OpenDevice,.close=CloseDevice,.bread=ReadDevice,.bwrite=WriteDevice,
    .ph_bsize=4096,.ph_bcnt=V->Block->Media->LastBlock+1,.ph_bbuf=V->BlockBuffer,.p_user=V};
  V->Dev=(struct ext4_blockdev){.bdif=&V->Iface,.part_size=V->Iface.ph_bcnt*4096};
  // Android owns recovery of a dirty persist filesystem.
  V->Writable=V->Writable&&CleanPersist(V);
  int R=V->Failed?EIO:ext4_device_register(&V->Dev,V->Device);V->Registered=R==0;
  if(!R){R=ext4_mount(V->Device,V->Mount,!V->Writable);V->Mounted=R==0;}
  if(!R&&V->Dev.fs->read_only)V->Writable=FALSE;
  if(!R&&V->Writable){R=ext4_journal_start(V->Mount);V->Journal=R==0;}
  if(R||V->Failed){
    DEBUG((DEBUG_WARN,"PIANO_EXT4_MOUNT errno=%d io_failed=%u mounted=%u\n",R,V->Failed,V->Mounted));
    if(V->Mounted){
      // A recovery/start failure may have changed journal state. Keep the
      // driver and its buffers; Stop must not mark this filesystem clean.
      V->Failed=TRUE;mVolumes[Slot]=V;return EFI_DEVICE_ERROR;
    }
    if(V->Registered)ext4_device_unregister(V->Device);
    gBS->CloseProtocol(Handle,&gEfiDiskIoProtocolGuid,B->DriverBindingHandle,Handle);FreePool(V);return Status(R?R:EIO);
  }
  mVolumes[Slot]=V;
  S=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiSimpleFileSystemProtocolGuid,&V->Sfs,NULL);
  DEBUG((DEBUG_WARN,"PIANO_EXT4_MOUNT status=%r writable=%u partition=persist path_prefix=sunuefi\n",S,V->Writable));
  if(S!=EFI_SUCCESS){
    int Cleanup=V->Journal?ext4_journal_stop(V->Mount):0;V->Journal=FALSE;
    if(!Cleanup)Cleanup=ext4_umount(V->Mount);
    if(Cleanup||V->Failed){V->Failed=TRUE;return S;}
    V->Mounted=FALSE;
    if(V->Writable&&Barrier(V)!=EFI_SUCCESS)return S;
    ext4_device_unregister(V->Device);mVolumes[Slot]=NULL;
    gBS->CloseProtocol(Handle,&gEfiDiskIoProtocolGuid,B->DriverBindingHandle,Handle);FreePool(V);
  }
  return S;
}
STATIC EFI_STATUS EFIAPI Stop(EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE Handle,UINTN Children,EFI_HANDLE *Handles){
  (VOID)Handles;if(Children)return EFI_UNSUPPORTED;
  UINTN Slot=0;while(Slot<ARRAY_SIZE(mVolumes)&&(!mVolumes[Slot]||mVolumes[Slot]->Handle!=Handle))++Slot;
  if(Slot==ARRAY_SIZE(mVolumes))return EFI_NOT_FOUND;VOLUME *V=mVolumes[Slot];
  if(V->OpenFiles||V->Busy||V->Failed){
    DEBUG((DEBUG_WARN,"PIANO_EXT4_STOP_REFUSED open_files=%u busy=%u failed=%u\n",(UINT32)V->OpenFiles,V->Busy,V->Failed));
    return EFI_ACCESS_DENIED;
  }
  EFI_STATUS S=FlushVolume(V);if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"PIANO_EXT4_STOP stage=flush status=%r\n",S));return S;}
  V->Busy=TRUE;
  // Do not leave a published protocol pointing at an unmounted filesystem if
  // uninstall is refused. No live file handles are forcibly closed here.
  S=gBS->UninstallMultipleProtocolInterfaces(Handle,&gEfiSimpleFileSystemProtocolGuid,&V->Sfs,NULL);
  if(S!=EFI_SUCCESS){V->Busy=FALSE;return S;}
  int R=V->Journal?ext4_journal_stop(V->Mount):0;V->Journal=FALSE;
  if(!R)R=ext4_umount(V->Mount);
  if(R){V->Failed=TRUE;V->Busy=FALSE;DEBUG((DEBUG_WARN,"PIANO_EXT4_STOP stage=unmount errno=%d\n",R));return Status(R);}
  V->Mounted=FALSE;
  // umount writes the clean superblock after stopping the journal. Complete
  // its final barrier while the sole UFS owner is still live.
  if(V->Writable){S=Barrier(V);if(S!=EFI_SUCCESS)return S;}
  DEBUG((DEBUG_WARN,"PIANO_EXT4_UNMOUNT status=%r open_files=%u journal_stopped=1\n",S,(UINT32)V->OpenFiles));
  ext4_device_unregister(V->Device);for(UINTN I=0;I<ARRAY_SIZE(mVolumes);++I)if(mVolumes[I]==V)mVolumes[I]=NULL;
  gBS->CloseProtocol(Handle,&gEfiDiskIoProtocolGuid,B->DriverBindingHandle,Handle);FreePool(V);return EFI_SUCCESS;
}
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding={Supported,Start,Stop,0x10,NULL,NULL};
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;mExited=TRUE;}
EFI_STATUS EFIAPI PianoExt4Entry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table){
  EFI_STATUS S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,&gEfiEventExitBootServicesGuid,&mExit);if(S!=EFI_SUCCESS)return S;
  S=EfiLibInstallDriverBindingComponentName2(Image,Table,&mBinding,Image,NULL,NULL);
  if(S!=EFI_SUCCESS){gBS->CloseEvent(mExit);mExit=NULL;}return S;
}
