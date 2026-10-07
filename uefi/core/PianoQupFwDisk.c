// SPDX-License-Identifier: BSD-2-Clause-Patent
// One immutable firmware partition in RAM. No physical disk is accessed.
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC EFI_GUID mImageGuid={0x77997A49,0x2795,0x457B,{0x95,0xB3,0x40,0x9B,0xAC,0x12,0x4C,0xA3}};
STATIC EFI_GUID mTypeGuid={0x21D1219F,0x2ED1,0x4AB4,{0x93,0x0A,0x41,0xA1,0x6A,0xE7,0x5F,0x7F}};
STATIC UINT8 *mFirmware;
STATIC UINTN mFirmwareBytes;
STATIC EFI_HANDLE mDisk;
STATIC CONST UINT8 mDigest[32]={0xB6,0x48,0x51,0x6E,0x1F,0xDE,0x83,0xA1,0xB6,0xA0,0xD6,0xA3,0xC1,0xBC,0x30,0x84,0xAD,0xC6,0x3A,0xEC,0xC3,0x4F,0x77,0x56,0xC3,0x18,0x50,0xAB,0x4A,0xD4,0xB4,0xE9};
STATIC EFI_BLOCK_IO_MEDIA mMedia={
  .MediaId=1,.RemovableMedia=FALSE,.MediaPresent=TRUE,.LogicalPartition=TRUE,
  .ReadOnly=TRUE,.WriteCaching=FALSE,.BlockSize=512,.IoAlign=1,.LastBlock=255
};
STATIC EFI_BLOCK_IO_PROTOCOL mBlock;
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  HARDDRIVE_DEVICE_PATH Partition;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0xE0B655CB,0x8BAA,0x48CA,{0x88,0x11,0x4F,0xAC,0x10,0x71,0x00,0x16}}},
  {{MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,{sizeof(HARDDRIVE_DEVICE_PATH),0}},1,0,256,
   {0xC5,0x9A,0x3F,0x22,0xA0,0xA0,0xE5,0x47,0x96,0x22,0x14,0x19,0x3B,0x8D,0x11,0xE1},MBR_TYPE_EFI_PARTITION_TABLE_HEADER,SIGNATURE_TYPE_GUID},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}
};
STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *This,BOOLEAN Extended) {
  return This==&mBlock?EFI_SUCCESS:EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  if(This!=&mBlock)return EFI_INVALID_PARAMETER;
  if(MediaId!=mMedia.MediaId)return EFI_MEDIA_CHANGED;
  if(Bytes==0)return EFI_SUCCESS;
  if(Buffer==NULL || mFirmware==NULL)return EFI_INVALID_PARAMETER;
  if(Bytes%512)return EFI_BAD_BUFFER_SIZE;
  UINTN Blocks=Bytes/512;
  if(Lba>255 || Blocks>256-(UINTN)Lba)return EFI_INVALID_PARAMETER;
  CopyMem(Buffer,mFirmware+(UINTN)Lba*512,Bytes);
  DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_BLOCK_READ lba=%lu bytes=%lu\n",Lba,(UINT64)Bytes));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *This,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  // This transport never has a write path, regardless of advertised media.
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This) {return EFI_SUCCESS;}

EFI_STATUS PianoInstallQupFwDisk(VOID) {
  STATIC EFI_GUID Scm={0x77ED108D,0x8524,0x4B8B,{0x9D,0x2E,0x34,0x98,0x7A,0xEC,0xB9,0xC1}};
  VOID *Existing=NULL;
  // Read/parsing test only: reject this profile if a SCM provider could let
  // the original GPI loader authenticate or program the firmware.
  if(!EFI_ERROR(gBS->LocateProtocol(&Scm,NULL,&Existing)))return EFI_ACCESS_DENIED;
  EFI_STATUS Status=GetSectionFromAnyFv(&mImageGuid,EFI_SECTION_RAW,0,(VOID **)&mFirmware,&mFirmwareBytes);
  if(EFI_ERROR(Status))return Status;
  UINT8 Hash[32];
  if(mFirmwareBytes!=0x20000 || !Sha256HashAll(mFirmware,mFirmwareBytes,Hash) || CompareMem(Hash,mDigest,32)) {
    FreePool(mFirmware);mFirmware=NULL;return EFI_SECURITY_VIOLATION;
  }
  mBlock.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION;mBlock.Media=&mMedia;
  mBlock.Reset=Reset;mBlock.ReadBlocks=Read;mBlock.WriteBlocks=Write;mBlock.FlushBlocks=Flush;
  Status=gBS->InstallMultipleProtocolInterfaces(&mDisk,&gEfiBlockIoProtocolGuid,&mBlock,
    &gEfiDevicePathProtocolGuid,&mPath,&mTypeGuid,NULL,NULL);
  DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_DISK_INSTALL %r readonly=1 bytes=%lu\n",Status,(UINT64)mFirmwareBytes));
  return Status;
}
