// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware-read interface used by the original GPI loader. Only the verified
// active-slot QUP image is exposed; no BlockIo, filesystem or write service.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC EFI_GUID mReadGuid={0x8D12919D,0xB55A,0x4324,{0xBA,0x9B,0x1D,0x4C,0xD7,0xEC,0xCD,0xFE}};
STATIC EFI_GUID mImageGuid={0x77997A49,0x2795,0x457B,{0x95,0xB3,0x40,0x9B,0xAC,0x12,0x4C,0xA3}};
STATIC UINT8 *mImage;
STATIC UINTN mImageBytes;
STATIC EFI_HANDLE mProvider;
STATIC UINT64 mToken;
STATIC CONST UINT8 mHash[32]={0xB6,0x48,0x51,0x6E,0x1F,0xDE,0x83,0xA1,0xB6,0xA0,0xD6,0xA3,0xC1,0xBC,0x30,0x84,0xAD,0xC6,0x3A,0xEC,0xC3,0x4F,0x77,0x56,0xC3,0x18,0x50,0xAB,0x4A,0xD4,0xB4,0xE9};

STATIC BOOLEAN CallerRam(VOID *Pointer,UINTN Bytes) {
  EFI_MEMORY_REGION_DESCRIPTOR *Map;UINT8 Count;
  GetMemoryMap(&Map,&Count);UINTN Address=(UINTN)Pointer;
  for(UINTN I=0;I<Count;++I) {
    if(AsciiStrCmp(Map[I].Name,"DXE_Heap")!=0 && AsciiStrCmp(Map[I].Name,"UEFI_Stack")!=0)continue;
    if(Address>=Map[I].Address && Address-Map[I].Address<Map[I].Length &&
      Bytes<=Map[I].Length-(Address-Map[I].Address))return TRUE;
  }
  return FALSE;
}

STATIC UINT32 EFIAPI Open(UINT32 Device,CONST CHAR8 *Name,VOID **Handle) {
  if(Device!=0 || !CallerRam((VOID *)(UINTN)Name,8) || !CallerRam(Handle,sizeof(*Handle)) || mImage==NULL)return 1;
  // Both names are literal queries observed in the loader. No other partition
  // can be resolved and no live device or partition handle is returned.
  if(AsciiStrCmp(Name,"qupfw")!=0 && AsciiStrCmp(Name,"qupfw_a")!=0)return 1;
  *Handle=&mToken;DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_RAM_OPEN %a\n",Name));return 0;
}
STATIC UINT32 EFIAPI Info(VOID *Handle,VOID *Output) {
  if(Handle!=&mToken || !CallerRam(Output,0x3C))return 1;
  // The v1 GPI consumer multiplies these fields and passes the first as its
  // block count. Its destination is its own initialized 96-byte info object.
  UINT16 Blocks=(UINT16)(mImageBytes/512),BlockBytes=512;
  CopyMem((UINT8 *)Output+0x38,&Blocks,2);CopyMem((UINT8 *)Output+0x3A,&BlockBytes,2);
  DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_RAM_INFO blocks=%u block_bytes=512\n",Blocks));return 0;
}
STATIC UINT32 EFIAPI Read(VOID *Handle,UINT32 Lba,UINT32 Reserved,UINT32 Blocks,VOID *Output,UINT32 Bytes) {
  if(Handle!=&mToken || Reserved!=0 || Blocks==0 || Blocks>256 || Bytes!=Blocks*512 ||
    Lba>256 || Blocks>256-Lba || !CallerRam(Output,Bytes))return 1;
  CopyMem(Output,mImage+(UINTN)Lba*512,Bytes);
  DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_RAM_READ lba=%u bytes=%u\n",Lba,Bytes));return 0;
}
STATIC UINT32 EFIAPI Unsupported(VOID) {return 1;}
STATIC struct {
  UINT64 Version;
  UINT32(EFIAPI *Open)(UINT32,CONST CHAR8 *,VOID **);
  UINT32(EFIAPI *Info)(VOID *,VOID *);
  VOID *Unused[4];
  UINT32(EFIAPI *Read)(VOID *,UINT32,UINT32,UINT32,VOID *,UINT32);
  VOID *Tail[4];
} mInterface={1,Open,Info,{(VOID *)Unsupported,(VOID *)Unsupported,(VOID *)Unsupported,(VOID *)Unsupported},Read,
  {(VOID *)Unsupported,(VOID *)Unsupported,(VOID *)Unsupported,(VOID *)Unsupported}};

EFI_STATUS PianoInstallQupFwRam(VOID) {
  UINT8 Hash[32];EFI_STATUS Status;
  // This profile verifies firmware retrieval/parsing only. Do not let the
  // original loader proceed into PAS authentication until its buffers and
  // SCM wrapper have been separately reviewed and implemented.
  STATIC EFI_GUID Scm={0x77ED108D,0x8524,0x4B8B,{0x9D,0x2E,0x34,0x98,0x7A,0xEC,0xB9,0xC1}};
  VOID *Existing=NULL;
  if(!EFI_ERROR(gBS->LocateProtocol(&Scm,NULL,&Existing)))return EFI_ACCESS_DENIED;
  Status=GetSectionFromAnyFv(&mImageGuid,EFI_SECTION_RAW,0,(VOID **)&mImage,&mImageBytes);
  if(EFI_ERROR(Status))return Status;
  if(mImageBytes!=0x20000 || CompareMem(mImage,"\177ELF\001\001\001",7) ||
    !Sha256HashAll(mImage,mImageBytes,Hash) || CompareMem(Hash,mHash,32)) {
    FreePool(mImage);mImage=NULL;mImageBytes=0;return EFI_SECURITY_VIOLATION;
  }
  Status=gBS->InstallMultipleProtocolInterfaces(&mProvider,&mReadGuid,&mInterface,NULL);
  DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_RAM_INSTALL %r bytes=%lu\n",Status,(UINT64)mImageBytes));
  return Status;
}
