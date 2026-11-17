// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual patched FatAbandonVolume, memory-only EFI handles and a fake disk.
#include <assert.h>
#undef NULL
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PianoRawPartitionWrite.h>
#include <Protocol/SimpleFileSystem.h>

typedef struct {
  EFI_HANDLE Handle;
  VOID *Root;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL VolumeInterface;
  BOOLEAN Valid;
} FAT_VOLUME;
STATIC EFI_GUID mPianoRawPartitionWriteGuid=PIANO_RAW_PARTITION_WRITE_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
STATIC EFI_BOOT_SERVICES Bs,*gBS=&Bs;
STATIC EFI_BLOCK_IO_MEDIA Media;
STATIC EFI_BLOCK_IO_PROTOCOL Block={.Media=&Media};
STATIC FAT_VOLUME Volume;
STATIC BOOLEAN Marked,Locked;
STATIC EFI_STATUS LockStatus,FlushStatus,UninstallStatus;
STATIC UINTN Flushes,Uninstalls,Cleanups,Errors,Releases;

STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Device,EFI_GUID *Guid,VOID **Interface){
  assert(Device==Volume.Handle && Guid==&mPianoRawPartitionWriteGuid);
  *Interface=NULL;return Marked?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE Device,...){
  assert(Device==Volume.Handle && Volume.Valid);++Uninstalls;
  if(Marked)assert(Flushes==1 && Locked && !Volume.Root);
  else assert(!Locked && !Flushes);
  return UninstallStatus;
}
STATIC EFI_STATUS FatAcquireLockOrFail(VOID){
  assert(!Locked);
  if(LockStatus==EFI_SUCCESS)Locked=TRUE;
  return LockStatus;
}
STATIC VOID FatReleaseLock(VOID){assert(Locked);Locked=FALSE;++Releases;}
STATIC EFI_STATUS FatFlushDirtyCache(FAT_VOLUME *V,VOID *Task){
  assert(V==&Volume && !Task && Locked && !Uninstalls && !V->Root && V->Valid);
  ++Flushes;return FlushStatus;
}
STATIC VOID FatSetVolumeError(VOID *Root,EFI_STATUS Status){
  assert(Root==Volume.Root && Locked && !Marked && Status==EFI_NO_MEDIA);++Errors;
}
STATIC EFI_STATUS FatCleanupVolume(FAT_VOLUME *V,VOID *File,EFI_STATUS Status,VOID *Task){
  assert(V==&Volume && !File && !Task && Status==EFI_SUCCESS && Locked && !V->Valid);
  ++Cleanups;return EFI_SUCCESS;
}

#include "PianoActualFatAbandon.h"

STATIC VOID Fresh(BOOLEAN Marker,BOOLEAN OpenFiles){
  Volume=(FAT_VOLUME){.Handle=&Volume,.Root=OpenFiles?(VOID *)1:NULL,.BlockIo=&Block,.Valid=TRUE};
  Marked=Marker;Locked=FALSE;
  LockStatus=FlushStatus=UninstallStatus=EFI_SUCCESS;
  Flushes=Uninstalls=Cleanups=Errors=Releases=0;
}
int main(void){
  Bs=(EFI_BOOT_SERVICES){.HandleProtocol=Handle,.UninstallMultipleProtocolInterfaces=Uninstall};
  Fresh(TRUE,TRUE);
  assert(FatAbandonVolume(&Volume)==EFI_ACCESS_DENIED && Volume.Valid && Volume.Root &&
    !Flushes && !Errors && !Uninstalls && !Locked && Releases==1);
  Fresh(TRUE,FALSE);LockStatus=EFI_ACCESS_DENIED;
  assert(FatAbandonVolume(&Volume)==EFI_ACCESS_DENIED && Volume.Valid && !Uninstalls && !Flushes);
  Fresh(TRUE,FALSE);FlushStatus=EFI_DEVICE_ERROR;
  assert(FatAbandonVolume(&Volume)==EFI_DEVICE_ERROR && Volume.Valid && Flushes==1 && !Uninstalls && Releases==1);
  Fresh(TRUE,FALSE);UninstallStatus=EFI_DEVICE_ERROR;
  assert(FatAbandonVolume(&Volume)==EFI_DEVICE_ERROR && Volume.Valid && Flushes==1 && Uninstalls==1 && Releases==1);
  Fresh(TRUE,FALSE);
  assert(FatAbandonVolume(&Volume)==EFI_SUCCESS && !Volume.Valid && Flushes==1 && Uninstalls==1 &&
    Cleanups==1 && Releases==1 && !Errors && !Locked);
  Fresh(FALSE,TRUE);Media.MediaPresent=FALSE;
  assert(FatAbandonVolume(&Volume)==EFI_SUCCESS && !Volume.Valid && !Flushes && Uninstalls==1 &&
    Cleanups==1 && Releases==1 && Errors==1 && !Locked);
  return 0;
}
