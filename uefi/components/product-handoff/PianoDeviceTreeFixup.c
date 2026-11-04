// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoDeviceTreeFixup.h"
#include <PianoPanelSelection.h>
#include <PianoProductBootObjects.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define DTB_MAX 0x200000U
#define DTB_PADDING 0x10000U
STATIC EFI_GUID mFixupGuid=EFI_DT_FIXUP_PROTOCOL_GUID;
STATIC EFI_HANDLE mFixupHandle;
STATIC BOOLEAN (*mAlive)(VOID);
STATIC EFI_STATUS (*mValidateReserved)(CONST VOID *,UINTN);
STATIC EFI_DT_FIXUP_PROTOCOL mFixup;

STATIC EFI_STATUS EFIAPI Fixup(EFI_DT_FIXUP_PROTOCOL *This,VOID *Tree,UINTN *BufferSize,UINT32 Flags){
  if(This!=&mFixup||!Tree||!BufferSize||!Flags||
     (Flags&~(EFI_DT_APPLY_FIXUPS|EFI_DT_RESERVE_MEMORY|EFI_DT_INSTALL_TABLE))||
     ((UINTN)Tree&7)||*BufferSize<40||(UINTN)Tree>MAX_UINTN-*BufferSize)return EFI_INVALID_PARAMETER;
  if(!mAlive||!mAlive())return EFI_NOT_READY;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);
  if(!mAlive())return EFI_ABORTED;if(Tpl!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  if(FdtCheckHeader(Tree)||FdtTotalSize(Tree)>*BufferSize||FdtTotalSize(Tree)>DTB_MAX||
     FdtNodeOffsetByCompatible(Tree,-1,"xiaomi,piano")!=0)return EFI_COMPROMISED_DATA;
  UINTN Capacity=MIN(*BufferSize,(UINTN)DTB_MAX+DTB_PADDING);
  if(Flags&EFI_DT_APPLY_FIXUPS){
    UINTN Required=(UINTN)FdtTotalSize(Tree)+DTB_PADDING;
    if(Required>*BufferSize){*BufferSize=Required;return EFI_BUFFER_TOO_SMALL;}
    if(FdtOpenInto(Tree,Tree,(INT32)Capacity))return EFI_COMPROMISED_DATA;
    EFI_STATUS Status=PianoPanelSelectDtb(Tree,Capacity);
    if(Status==EFI_SUCCESS)Status=PianoProductBootObjectsApplyGunyah(Tree,Capacity);
    if(Status!=EFI_SUCCESS)return Status;
    INT32 Chosen=FdtPathOffset(Tree,"/chosen");if(Chosen<0)return EFI_COMPROMISED_DATA;
    STATIC CONST CHAR8 *Drop[]={"linux,initrd-start","linux,initrd-end","kaslr-seed","rng-seed",
      "linux,uefi-system-table","linux,uefi-mmap-start","linux,uefi-mmap-size","linux,uefi-mmap-desc-size","linux,uefi-mmap-desc-ver"};
    for(UINTN I=0;I<ARRAY_SIZE(Drop);++I){INT32 R=FdtDelProp(Tree,Chosen,Drop[I]);if(R&&R!=-FDT_ERR_NOTFOUND)return EFI_COMPROMISED_DATA;}
    if(FdtPack(Tree))return EFI_COMPROMISED_DATA;
  }
  if(!mAlive())return EFI_ABORTED;
  if(Flags&EFI_DT_RESERVE_MEMORY){
    EFI_STATUS Status=mValidateReserved(Tree,FdtTotalSize(Tree));
    if(!mAlive())return EFI_ABORTED;if(Status!=EFI_SUCCESS)return Status;
  }
  if(Flags&EFI_DT_INSTALL_TABLE)return gBS->InstallConfigurationTable(&gFdtTableGuid,Tree);
  return EFI_SUCCESS;
}

EFI_STATUS PianoDeviceTreeFixupInstall(BOOLEAN (*Alive)(VOID),EFI_STATUS (*ValidateReserved)(CONST VOID *,UINTN)){
  if(!Alive||!ValidateReserved)return EFI_INVALID_PARAMETER;
  if(mFixupHandle)return EFI_ALREADY_STARTED;
  if(!Alive()||!gBS||!gBS->RaiseTPL||!gBS->RestoreTPL||!gBS->LocateProtocol||!gBS->InstallProtocolInterface||!gBS->InstallConfigurationTable)return EFI_NOT_READY;
  VOID *Existing=NULL;EFI_STATUS Status=gBS->LocateProtocol(&mFixupGuid,NULL,&Existing);
  if(!Alive())return EFI_ABORTED;
  if(Status!=EFI_NOT_FOUND)return Status==EFI_SUCCESS?EFI_ALREADY_STARTED:Status;
  mAlive=Alive;mValidateReserved=ValidateReserved;
  mFixup=(EFI_DT_FIXUP_PROTOCOL){EFI_DT_FIXUP_PROTOCOL_REVISION,Fixup};
  return gBS->InstallProtocolInterface(&mFixupHandle,&mFixupGuid,EFI_NATIVE_INTERFACE,&mFixup);
}
