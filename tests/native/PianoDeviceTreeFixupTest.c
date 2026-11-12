// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real protocol, panel selection, Gunyah fixup and libfdt; BS/map are fixtures.
#include "../../uefi/core/product/PianoProductBootObjects.c"
#include "../../uefi/components/product-handoff/PianoDeviceTreeFixup.h"
#include <PianoFactoryPanel.h>
#include <Library/UefiBootServicesTableLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef FDT_TAGSIZE
#include <libfdt.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gFdtTableGuid={.Data1=0x1234};
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_DT_FIXUP_PROTOCOL *Protocol;
STATIC BOOLEAN Live=TRUE;
STATIC EFI_TPL Tpl=TPL_APPLICATION;
STATIC UINTN Reservations,Tables;
STATIC EFI_STATUS ReserveStatus=EFI_SUCCESS;
STATIC VOID *LastTable;
STATIC UINT8 Tree[131072] __attribute__((aligned(8))),Original[131072];
VOID *EFIAPI CopyMem(VOID *To,CONST VOID *From,UINTN Bytes){return memcpy(To,From,Bytes);}
VOID *EFIAPI ZeroMem(VOID *To,UINTN Bytes){return memset(To,0,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
STATIC BOOLEAN Alive(VOID){return Live;}
// Cold-HOB injection itself is exercised by PianoPlatformColdMemoryTest.
EFI_STATUS PianoProductMemoryApplyDtb(VOID *Fdt,UINTN Capacity){assert(Fdt==Tree&&Capacity<=sizeof(Tree));return EFI_SUCCESS;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){EFI_TPL Old=Tpl;Tpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
STATIC EFI_STATUS Reserved(CONST VOID *Fdt,UINTN Bytes){
  assert(Fdt==Tree&&Bytes==fdt_totalsize(Tree)&&!fdt_check_header(Tree));++Reservations;return ReserveStatus;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  EFI_GUID Expected=EFI_DT_FIXUP_PROTOCOL_GUID;(VOID)Registration;
  assert(!memcmp(Guid,&Expected,sizeof(Expected)));*Out=Protocol;return Protocol?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,EFI_GUID *Guid,EFI_INTERFACE_TYPE Type,VOID *Interface){
  EFI_GUID Expected=EFI_DT_FIXUP_PROTOCOL_GUID;assert(!memcmp(Guid,&Expected,sizeof(Expected))&&Type==EFI_NATIVE_INTERFACE);
  Protocol=Interface;*Handle=&Services;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Table(EFI_GUID *Guid,VOID *Fdt){
  assert(Guid==&gFdtTableGuid);++Tables;LastTable=Fdt;return EFI_SUCCESS;
}
STATIC UINTN MakeTree(VOID){
  assert(!fdt_create_empty_tree(Tree,sizeof(Tree)));
  assert(!fdt_setprop_string(Tree,0,"compatible","xiaomi,piano"));
  int Chosen=fdt_add_subnode(Tree,0,"chosen");assert(Chosen>=0);
  assert(!fdt_setprop_u64(Tree,Chosen,"linux,uefi-system-table",0x12345678));
  assert(!fdt_setprop_u64(Tree,Chosen,"linux,initrd-start",0x12345678));
  int Panel=fdt_add_subnode(Tree,0,"panel");assert(Panel>=0);
  assert(!fdt_setprop_string(Tree,Panel,"compatible","xiaomi,piano-csot-nt36532"));
  int Rm=fdt_add_subnode(Tree,0,"gunyah");assert(Rm>=0);
  assert(!fdt_setprop_string(Tree,Rm,"compatible","gunyah-resource-manager"));
  assert(!fdt_pack(Tree));return fdt_totalsize(Tree);
}
int main(void){
  gBS=&Services;Services.LocateProtocol=Locate;Services.InstallProtocolInterface=Install;
  Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;
  Services.InstallConfigurationTable=Table;
  assert(PianoDeviceTreeFixupInstall(Alive,Reserved)==EFI_SUCCESS&&Protocol&&Protocol->Revision==EFI_DT_FIXUP_PROTOCOL_REVISION);
  assert(PianoDeviceTreeFixupInstall(Alive,Reserved)==EFI_ALREADY_STARTED);
  PIANO_DEVICE_TREE_FIXUP_DIAGNOSTIC Diagnostic;
  PianoDeviceTreeFixupGetDiagnostic(&Diagnostic);
  assert(!strcmp(Diagnostic.Stage,"not-started")&&Diagnostic.Status==EFI_NOT_STARTED&&!Diagnostic.Calls);
  mObjectsStatus=mObjectsValidation=EFI_SUCCESS;mObjects.Coherent=TRUE;
  mObjects.FactoryPanel=PianoFactoryPanelBoe;mObjects.Gunyah.Valid=TRUE;mObjects.Gunyah.VmId=42;
  memset(mObjects.Gunyah.Resources,0x34,sizeof(mObjects.Gunyah.Resources));
  memset(mObjects.Gunyah.Interrupts,0x56,sizeof(mObjects.Gunyah.Interrupts));
  UINTN Bytes=MakeTree(),Size=Bytes;memcpy(Original,Tree,Bytes);
  assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_APPLY_FIXUPS|EFI_DT_RESERVE_MEMORY)==EFI_BUFFER_TOO_SMALL);
  assert(Size>Bytes&&Size<=sizeof(Tree)&&!memcmp(Tree,Original,Bytes)&&!Reservations&&!Tables);
  PianoDeviceTreeFixupGetDiagnostic(&Diagnostic);
  assert(!strcmp(Diagnostic.Stage,"padding")&&Diagnostic.Status==EFI_BUFFER_TOO_SMALL&&Diagnostic.Calls==1);
  assert(Diagnostic.Flags==(EFI_DT_APPLY_FIXUPS|EFI_DT_RESERVE_MEMORY)&&!Reservations&&!Tables);
  assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_APPLY_FIXUPS|EFI_DT_RESERVE_MEMORY)==EFI_SUCCESS&&Reservations==1&&!Tables);
  int Panel=fdt_path_offset(Tree,"/panel"),Rm=fdt_path_offset(Tree,"/gunyah"),Chosen=fdt_path_offset(Tree,"/chosen"),Length;
  assert(!strcmp(fdt_getprop(Tree,Panel,"compatible",NULL),"xiaomi,piano-boe-nt36532"));
  const VOID *Reg=fdt_getprop(Tree,Rm,"reg",&Length);
  assert(Reg&&Length==sizeof(mObjects.Gunyah.Resources)&&!memcmp(Reg,mObjects.Gunyah.Resources,Length));
  assert(!fdt_getprop(Tree,Chosen,"linux,uefi-system-table",NULL)&&!fdt_getprop(Tree,Chosen,"linux,initrd-start",NULL));
  Size=sizeof(Tree);ReserveStatus=EFI_NOT_READY;
  assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_RESERVE_MEMORY|EFI_DT_INSTALL_TABLE)==EFI_NOT_READY&&!Tables&&Reservations==2);
  PianoDeviceTreeFixupGetDiagnostic(&Diagnostic);
  assert(!strcmp(Diagnostic.Stage,"reservations")&&Diagnostic.Status==EFI_NOT_READY&&Reservations==2&&!Tables);
  ReserveStatus=EFI_SUCCESS;
  assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_RESERVE_MEMORY|EFI_DT_INSTALL_TABLE)==EFI_SUCCESS&&Tables==1&&LastTable==Tree);
  MakeTree();mObjects.Gunyah.Valid=FALSE;Size=sizeof(Tree);
  assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_APPLY_FIXUPS)==EFI_SUCCESS);
  Rm=fdt_path_offset(Tree,"/gunyah");assert(!strcmp(fdt_getprop(Tree,Rm,"status",NULL),"disabled"));
  assert(Protocol->Fixup(Protocol,Tree,&Size,0x8000)==EFI_INVALID_PARAMETER);
  Tpl=TPL_NOTIFY;assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_RESERVE_MEMORY)==EFI_UNSUPPORTED);Tpl=TPL_APPLICATION;
  Live=FALSE;assert(Protocol->Fixup(Protocol,Tree,&Size,EFI_DT_RESERVE_MEMORY)==EFI_NOT_READY);
  PianoDeviceTreeFixupGetDiagnostic(&Diagnostic);
  assert(!strcmp(Diagnostic.Stage,"alive")&&Diagnostic.Status==EFI_NOT_READY);
  puts("Actual EFI DT fixup: padding retry, runtime panel/Gunyah, stale EFI cleanup and reservation refusal passed");return 0;
}
