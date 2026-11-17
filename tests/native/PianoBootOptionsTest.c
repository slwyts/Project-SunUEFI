// SPDX-License-Identifier: BSD-2-Clause-Patent
#define MDEPKG_NDEBUG
#include "../../uefi/core/boot/PianoBootOptions.c"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiGlobalVariableGuid={.Data1=1};
EFI_GUID gEfiSimpleFileSystemProtocolGuid={.Data1=2};
EFI_GUID mBmAutoCreateBootOptionGuid={.Data1=3};
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,sizeof(*A));}
STATIC EFI_BOOT_SERVICES Bs;
STATIC EFI_RUNTIME_SERVICES Rt;
STATIC EFI_BOOT_MANAGER_LOAD_OPTION Options[3],NextOption;
STATIC EFI_DEVICE_PATH_PROTOCOL Paths[3];
STATIC UINTN Count,NextNumber,Loads,Order[4],ReadySignals,NextDeletes,CurrentSets,CurrentDeletes;
STATIC BOOLEAN HasNext,NextMissing;
STATIC UINT8 Optional[]={0,0xff,0,7,8};
STATIC EFI_HANDLE Volume=(VOID *)0x1234;
BOOLEAN EFIAPI IsDevicePathValid(CONST EFI_DEVICE_PATH_PROTOCOL *Path,UINTN Maximum){
  assert(!Maximum);return Path==&Paths[0]||Path==&Paths[1]||Path==&Paths[2];
}
STATIC BOOLEAN Same(CONST CHAR16 *A,CONST CHAR16 *B){
  while(*A&&*A==*B){++A;++B;}return *A==*B;
}
STATIC EFI_STATUS EFIAPI Get(CHAR16 *Name,EFI_GUID *Guid,UINT32 *Attributes,UINTN *Bytes,VOID *Data){
  assert(Same(Name,L"BootNext")&&Guid==&gEfiGlobalVariableGuid&&!Attributes&&*Bytes==2);
  if(!HasNext)return EFI_NOT_FOUND;
  *(UINT16 *)Data=(UINT16)NextNumber;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Set(CHAR16 *Name,EFI_GUID *Guid,UINT32 Attributes,UINTN Bytes,VOID *Data){
  assert(Guid==&gEfiGlobalVariableGuid);
  if(Same(Name,L"BootNext")){
    assert(!Attributes&&!Bytes&&!Data);HasNext=FALSE;++NextDeletes;return EFI_SUCCESS;
  }
  assert(Same(Name,L"BootCurrent"));
  if(Bytes){assert(Bytes==2&&Data);++CurrentSets;}
  else {assert(!Data&&!Attributes);++CurrentDeletes;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,EFI_DEVICE_PATH_PROTOCOL **Path,EFI_HANDLE *Device){
  assert(Guid==&gEfiSimpleFileSystemProtocolGuid&&Path&&*Path);
  *Device=Volume;return EFI_SUCCESS;
}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Number,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){
  assert(Type==LoadOptionTypeBoot);*Number=Count;return Options;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *Array,UINTN Number){
  assert(Array==Options&&Number==Count);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerVariableToLoadOption(CHAR16 *Name,EFI_BOOT_MANAGER_LOAD_OPTION *Option){
  (VOID)Name;assert(!HasNext&&NextDeletes);
  if(NextMissing)return EFI_NOT_FOUND;
  *Option=NextOption;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option){
  assert(Option->OptionNumber==NextNumber);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerConnectDevicePath(EFI_DEVICE_PATH_PROTOCOL *Path,EFI_HANDLE *Handle){
  assert(Path&&!Handle);
  return EFI_SUCCESS;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI EfiBootManagerGetNextLoadOptionDevicePath(
  EFI_DEVICE_PATH_PROTOCOL *Path,EFI_DEVICE_PATH_PROTOCOL *Previous){
  if(Previous)return NULL;
  EFI_DEVICE_PATH_PROTOCOL *Result=malloc(sizeof(*Result));*Result=*Path;return Result;
}
VOID EFIAPI FreePool(VOID *Memory){free(Memory);}
UINTN EFIAPI UnicodeSPrint(CHAR16 *Buffer,UINTN Bytes,CONST CHAR16 *Format,...){
  (VOID)Format;assert(Bytes==18);memcpy(Buffer,L"Boot0017",18);return 8;
}
VOID EFIAPI EfiSignalEventReadyToBoot(VOID){++ReadySignals;}
EFI_STATUS PianoApplicationRunPath(EFI_HANDLE Parent,EFI_HANDLE Device,EFI_DEVICE_PATH_PROTOCOL *Path,
  CONST VOID *Data,UINT32 Bytes,UINT64 Limit,PIANO_FV_APPLICATION *Application){
  assert(Parent==(VOID *)1&&Device==Volume&&Data==Optional&&Bytes==sizeof(Optional));
  assert(Limit==0x10000000&&CurrentSets==Loads+1);
  Order[Loads++]=Path->SubType;
  Application->StartCalled=Path->SubType!=1;
  return Path->SubType==1?EFI_NOT_FOUND:EFI_LOAD_ERROR;
}
STATIC VOID Reset(VOID){
  memset(&Bs,0,sizeof(Bs));memset(&Rt,0,sizeof(Rt));
  Bs.LocateDevicePath=Locate;Rt.GetVariable=Get;Rt.SetVariable=Set;gBS=&Bs;gRT=&Rt;
  Count=2;HasNext=NextMissing=FALSE;NextNumber=23;
  Loads=ReadySignals=NextDeletes=CurrentSets=CurrentDeletes=0;
  for(UINTN I=0;I<3;++I){
    Paths[I]=(EFI_DEVICE_PATH_PROTOCOL){MEDIA_DEVICE_PATH,(UINT8)(I+1),{4,0}};
    Options[I]=(EFI_BOOT_MANAGER_LOAD_OPTION){.OptionNumber=I+1,.OptionType=LoadOptionTypeBoot,
      .Attributes=LOAD_OPTION_ACTIVE,.FilePath=&Paths[I],.OptionalData=Optional,.OptionalDataSize=sizeof(Optional)};
  }
  NextOption=Options[2];NextOption.OptionNumber=NextNumber;
}
int main(void){
  PIANO_FV_APPLICATION App;EFI_HANDLE Device;
  Reset();memset(&App,0,sizeof(App));
  assert(PianoStartBootOption((VOID *)1,&Device,&App)==EFI_LOAD_ERROR);
  assert(Loads==2&&Order[0]==1&&Order[1]==2&&Device==Volume);
  assert(ReadySignals==2&&CurrentSets==2&&CurrentDeletes==2&&!NextDeletes);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==3&&NextDeletes==1&&!HasNext);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;NextOption.Attributes=0;
  assert(PianoStartBootOption((VOID *)1,&Device,&App)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==3&&NextDeletes==1);
  Reset();memset(&App,0,sizeof(App));HasNext=NextMissing=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App)==EFI_LOAD_ERROR);
  assert(Loads==2&&Order[0]==1&&NextDeletes==1);
  Reset();memset(&App,0,sizeof(App));Options[0].Attributes=0;
  assert(PianoStartBootOption((VOID *)1,&Device,&App)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==2);
  puts("Standard BootNext, ordered options, inactive entries and binary OptionalData passed");
  return 0;
}
