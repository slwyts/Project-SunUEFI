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
EFI_GUID gEfiFirmwareVolume2ProtocolGuid={.Data1=4};
EFI_GUID gEfiLoadFileProtocolGuid={.Data1=5};
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,sizeof(*A));}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
STATIC EFI_BOOT_SERVICES Bs;
STATIC EFI_RUNTIME_SERVICES Rt;
STATIC EFI_BOOT_MANAGER_LOAD_OPTION Options[3],NextOption;
STATIC EFI_DEVICE_PATH_PROTOCOL Paths[3];
STATIC EFI_DEVICE_PATH_PROTOCOL *AllocatedPath;
typedef struct {
  MEDIA_FW_VOL_DEVICE_PATH Volume;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH File;
  EFI_DEVICE_PATH_PROTOCOL End;
} FIRMWARE_PATH;
STATIC FIRMWARE_PATH MenuPath,RetiredPath;
STATIC UINTN Count,NextNumber,NextSize,Loads,Order[4],ReadySignals,NextDeletes,CurrentSets,CurrentDeletes,NextSaves;
STATIC UINTN Adds,Deletes,Position,Refreshes,PrepareCalls,FinishCalls,LoadFileLookups;
STATIC EFI_STATUS FilesystemStatus;
STATIC CONST EFI_DEVICE_PATH_PROTOCOL *BootPath;
STATIC UINTN RetryChecks,AlternatePaths;
STATIC BOOLEAN HasNext,NextMissing,OrphanMenu,AllowRetry,RetainApplication,ExitOnStart,ExitServices,HasAlternatePath;
STATIC BOOLEAN DeferredNext;
STATIC EFI_STATUS NextReadStatus,SaveStatus,PrepareStatus,SetStatus,ClearStatus,ImageStatus[4];
STATIC UINT16 Current;
STATIC UINT8 Optional[]={0,0xff,0,7,8};
STATIC EFI_HANDLE Volume=(VOID *)0x1234;
STATIC EFI_HANDLE Fv=(VOID *)0x5678;
STATIC EFI_HANDLE LoadFile=(VOID *)0x9000;
STATIC CONST CHAR16 OrphanName[]=L"Boot00f0";
BOOLEAN EFIAPI IsDevicePathValid(CONST EFI_DEVICE_PATH_PROTOCOL *Path,UINTN Maximum){
  assert(!Maximum);return Path!=NULL;
}
UINT8 EFIAPI DevicePathType(CONST VOID *Node){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->Type;}
UINT8 EFIAPI DevicePathSubType(CONST VOID *Node){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->SubType;}
UINTN EFIAPI DevicePathNodeLength(CONST VOID *Node){
  CONST EFI_DEVICE_PATH_PROTOCOL *Path=Node;
  return Path->Length[0]|((UINT16)Path->Length[1]<<8);
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(CONST VOID *Node){
  return (EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)Node+DevicePathNodeLength(Node));
}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *Node){
  return DevicePathType(Node)==END_DEVICE_PATH_TYPE && DevicePathSubType(Node)==END_ENTIRE_DEVICE_PATH_SUBTYPE;
}
UINTN EFIAPI GetDevicePathSize(CONST EFI_DEVICE_PATH_PROTOCOL *Path){
  return Path && Path->Type==MEDIA_DEVICE_PATH && Path->SubType==MEDIA_PIWG_FW_VOL_DP?
    sizeof(FIRMWARE_PATH):sizeof(EFI_DEVICE_PATH_PROTOCOL);
}
STATIC BOOLEAN Same(CONST CHAR16 *A,CONST CHAR16 *B){
  while(*A&&*A==*B){++A;++B;}return *A==*B;
}
STATIC EFI_STATUS EFIAPI Get(CHAR16 *Name,EFI_GUID *Guid,UINT32 *Attributes,UINTN *Bytes,VOID *Data){
  assert(Same(Name,L"BootNext")&&Guid==&gEfiGlobalVariableGuid&&!Attributes&&*Bytes==2);
  if(!HasNext)return EFI_NOT_FOUND;
  if(NextReadStatus!=EFI_SUCCESS)return NextReadStatus;
  BOOLEAN TooSmall=*Bytes<NextSize;
  *Bytes=NextSize;
  if(TooSmall)return EFI_BUFFER_TOO_SMALL;
  UINT16 Value=(UINT16)NextNumber;
  memcpy(Data,&Value,NextSize);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI NextVariable(UINTN *Bytes,CHAR16 *Name,EFI_GUID *Guid){
  if(!OrphanMenu || Name[0])return EFI_NOT_FOUND;
  assert(*Bytes>=sizeof(OrphanName));*Bytes=sizeof(OrphanName);
  memcpy(Name,OrphanName,*Bytes);*Guid=gEfiGlobalVariableGuid;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Set(CHAR16 *Name,EFI_GUID *Guid,UINT32 Attributes,UINTN Bytes,VOID *Data){
  assert(Guid==&gEfiGlobalVariableGuid&&!ExitServices);
  if(Same(Name,L"BootNext")){
    assert(!Attributes&&!Bytes&&!Data);HasNext=FALSE;++NextDeletes;return EFI_SUCCESS;
  }
  assert(Same(Name,L"BootCurrent"));
  if(Bytes){
    assert(Bytes==2&&Data&&Current==MAX_UINT16);
    if(SetStatus==EFI_SUCCESS){Current=*(UINT16 *)Data;++CurrentSets;}
    return SetStatus;
  }
  assert(!Data&&!Attributes&&Current!=MAX_UINT16);
  if(ClearStatus==EFI_SUCCESS || ClearStatus==EFI_NOT_FOUND)Current=MAX_UINT16;
  ++CurrentDeletes;
  return ClearStatus;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,EFI_DEVICE_PATH_PROTOCOL **Path,EFI_HANDLE *Device){
  assert(Path&&*Path);
  if(Guid==&gEfiFirmwareVolume2ProtocolGuid){
    assert(PianoBootOptionIsSimpleInit(*Path));
    *Device=Fv;*Path=&((FIRMWARE_PATH *)*Path)->File.Header;
  }else if(Guid==&gEfiSimpleFileSystemProtocolGuid){
    if(FilesystemStatus!=EFI_SUCCESS){
      *Path=NULL;
      return FilesystemStatus;
    }
    *Device=Volume;
  }else{
    assert(Guid==&gEfiLoadFileProtocolGuid && *Path==AllocatedPath);
    ++LoadFileLookups;*Device=LoadFile;
  }
  return EFI_SUCCESS;
}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Number,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){
  assert(Type==LoadOptionTypeBoot);*Number=Count;return Count?Options:NULL;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *Array,UINTN Number){
  assert(Array==(Count?Options:NULL)&&Number==Count&&!ExitServices);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerVariableToLoadOption(CHAR16 *Name,EFI_BOOT_MANAGER_LOAD_OPTION *Option){
  if(Same(Name,OrphanName)){
    assert(OrphanMenu);
    *Option=(EFI_BOOT_MANAGER_LOAD_OPTION){.OptionNumber=0xf0,.FilePath=&MenuPath.Volume.Header};return EFI_SUCCESS;
  }
  if(Same(Name,L"Boot0002")){*Option=Options[1];return EFI_SUCCESS;}
  assert(Same(Name,L"Boot0017"));
  assert((!HasNext&&NextDeletes&&NextSaves)||(HasNext&&DeferredNext&&!NextDeletes&&!NextSaves));
  if(NextMissing)return EFI_NOT_FOUND;
  *Option=NextOption;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option){
  assert(Option&&!ExitServices);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerInitializeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option,
  UINTN Number,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type,UINT32 Attr,CHAR16 *Description,
  EFI_DEVICE_PATH_PROTOCOL *Path,UINT8 *Data,UINT32 Size){
  assert(Number==LoadOptionNumberUnassigned&&Type==LoadOptionTypeBoot&&Attr==LOAD_OPTION_ACTIVE);
  assert(Same(Description,L"SimpleInit")&&PianoBootOptionIsSimpleInit(Path)&&!Data&&!Size);
  *Option=(EFI_BOOT_MANAGER_LOAD_OPTION){.OptionNumber=Number,.OptionType=Type,.FilePath=Path};
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerAddLoadOptionVariable(EFI_BOOT_MANAGER_LOAD_OPTION *Option,UINTN At){
  assert(PianoBootOptionIsSimpleInit(Option->FilePath)&&!OrphanMenu);
  Position=At;++Adds;OrphanMenu=TRUE;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerDeleteLoadOptionVariable(UINTN Number,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){
  assert(Number==Options[0].OptionNumber&&Type==LoadOptionTypeBoot&&Options[0].FilePath==&RetiredPath.Volume.Header);
  ++Deletes;return EFI_SUCCESS;
}
VOID EFIAPI EfiBootManagerRefreshAllBootOption(VOID){++Refreshes;}
EFI_STATUS EFIAPI GetFileDevicePathFromAnyFv(CONST EFI_GUID *Name,EFI_SECTION_TYPE Type,UINTN Instance,
  EFI_DEVICE_PATH_PROTOCOL **Path){
  assert(CompareGuid(Name,&mPianoSimpleInitBootFileGuid)&&Type==EFI_SECTION_PE32&&!Instance);
  *Path=malloc(sizeof(MenuPath));assert(*Path);memcpy(*Path,&MenuPath,sizeof(MenuPath));return EFI_SUCCESS;
}
EFI_STATUS PianoNvFileSave(BOOLEAN Freeze) {
  assert(!Freeze&&NextDeletes&&!Loads);
  ++NextSaves;return SaveStatus;
}
BOOLEAN PianoNvFileCanConsumeBootNext(UINT16 Number){
  assert(Number==NextNumber);
  return !DeferredNext;
}
STATIC EFI_STATUS Prepare(CONST EFI_DEVICE_PATH_PROTOCOL *Path){
  assert(Path&&!BootPath);++PrepareCalls;
  if(PrepareStatus==EFI_SUCCESS)BootPath=Path;
  return PrepareStatus;
}
STATIC VOID FinishSystem(VOID){assert(BootPath);BootPath=NULL;++FinishCalls;}
STATIC VOID EFIAPI Expired(UINT64 Time){(VOID)Time;assert(FALSE);}
STATIC BOOLEAN Retry(EFI_STATUS Status){
  assert(EFI_ERROR(Status)&&Current==MAX_UINT16&&CurrentDeletes==Loads);
  ++RetryChecks;
  return AllowRetry;
}
STATIC FIRMWARE_PATH FirmwarePath(EFI_GUID File){
  return (FIRMWARE_PATH){.Volume={{MEDIA_DEVICE_PATH,MEDIA_PIWG_FW_VOL_DP,{sizeof(MEDIA_FW_VOL_DEVICE_PATH),0}},{.Data1=10}},
    .File={{MEDIA_DEVICE_PATH,MEDIA_PIWG_FW_FILE_DP,{sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH),0}},File},
    .End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}};
}
EFI_STATUS EFIAPI EfiBootManagerConnectDevicePath(EFI_DEVICE_PATH_PROTOCOL *Path,EFI_HANDLE *Handle){
  assert(Path&&!Handle);
  return EFI_SUCCESS;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI EfiBootManagerGetNextLoadOptionDevicePath(
  EFI_DEVICE_PATH_PROTOCOL *Path,EFI_DEVICE_PATH_PROTOCOL *Previous){
  if(Previous){
    if(!HasAlternatePath || Path->SubType!=3 || Previous->SubType!=3)return NULL;
    Path=&Paths[1];
    ++AlternatePaths;
  }
  UINTN Bytes=GetDevicePathSize(Path);
  EFI_DEVICE_PATH_PROTOCOL *Result=malloc(Bytes);assert(Result);memcpy(Result,Path,Bytes);
  if(!Previous)AllocatedPath=Result;
  return Result;
}
VOID EFIAPI FreePool(VOID *Memory){
  assert(!ExitServices);
  if(Memory==AllocatedPath)AllocatedPath=NULL;
  free(Memory);
}
UINTN EFIAPI UnicodeSPrint(CHAR16 *Buffer,UINTN Bytes,CONST CHAR16 *Format,...){
  (VOID)Format;assert(Bytes==18);
  va_list Args;va_start(Args,Format);int Number=va_arg(Args,int);va_end(Args);
  assert(Number==2||Number==23);
  memcpy(Buffer,Number==2?L"Boot0002":L"Boot0017",18);return 8;
}
VOID EFIAPI EfiSignalEventReadyToBoot(VOID){++ReadySignals;}
EFI_STATUS PianoApplicationRunPath(EFI_HANDLE Parent,EFI_HANDLE Device,EFI_DEVICE_PATH_PROTOCOL *Path,
  CONST VOID *Data,UINT32 Bytes,UINT64 Limit,EFI_WATCHDOG_TIMER_NOTIFY Watchdog,PIANO_FV_APPLICATION *Application){
  assert(Parent==(VOID *)1);
  assert(Limit==0x10000000&&CurrentSets==Loads+1&&Current!=MAX_UINT16);
  assert(Loads<ARRAY_SIZE(Order)&&!Application->StartCalled);
  memset(Application,0,sizeof(*Application));
  Application->Cleanup=RetainApplication?EFI_DEVICE_ERROR:EFI_SUCCESS;
  Application->Retained=RetainApplication;
  Application->EbsObserved=ExitServices=ExitOnStart;
  if(Device==Fv){
    assert(PianoBootOptionIsSimpleInit(Path)&&!Data&&!Bytes&&!Watchdog&&!BootPath);
    Order[Loads++]=0;Application->StartCalled=Application->StartReturned=TRUE;
    return Application->Start=ImageStatus[0];
  }
  assert(Device==(FilesystemStatus==EFI_SUCCESS?Volume:LoadFile) &&
    Data==Optional && Bytes==sizeof(Optional) && Watchdog==Expired && BootPath==Path);
  Order[Loads++]=Path->SubType;
  EFI_STATUS Status=ImageStatus[Path->SubType];
  Application->StartCalled=Application->StartReturned=Path->SubType!=1;
  if(Application->StartCalled)Application->Start=Status;
  return Status;
}
STATIC VOID Reset(VOID){
  free(AllocatedPath);AllocatedPath=NULL;
  if(mSimpleInitPath){free(mSimpleInitPath);mSimpleInitPath=NULL;}
  memset(&Bs,0,sizeof(Bs));memset(&Rt,0,sizeof(Rt));
  Bs.LocateDevicePath=Locate;Rt.GetVariable=Get;Rt.SetVariable=Set;
  Rt.GetNextVariableName=NextVariable;gBS=&Bs;gRT=&Rt;
  Count=2;HasNext=NextMissing=OrphanMenu=FALSE;NextNumber=23;NextSize=sizeof(UINT16);
  DeferredNext=FALSE;
  Loads=ReadySignals=NextDeletes=CurrentSets=CurrentDeletes=NextSaves=0;
  Adds=Deletes=Refreshes=PrepareCalls=FinishCalls=LoadFileLookups=0;
  BootPath=NULL;Position=MAX_UINTN;SaveStatus=PrepareStatus=FilesystemStatus=EFI_SUCCESS;
  RetryChecks=AlternatePaths=0;AllowRetry=RetainApplication=ExitOnStart=ExitServices=HasAlternatePath=FALSE;
  NextReadStatus=SetStatus=ClearStatus=EFI_SUCCESS;Current=MAX_UINT16;
  ImageStatus[0]=EFI_SUCCESS;ImageStatus[1]=EFI_NOT_FOUND;ImageStatus[2]=ImageStatus[3]=EFI_LOAD_ERROR;
  MenuPath=FirmwarePath(mPianoSimpleInitBootFileGuid);
  RetiredPath=FirmwarePath((EFI_GUID){0x9ddefc13,0x4746,0x441b,{0x87,0xe9,0x4c,0xd3,0xe1,0x21,0xb0,0x84}});
  for(UINTN I=0;I<3;++I){
    Paths[I]=(EFI_DEVICE_PATH_PROTOCOL){MEDIA_DEVICE_PATH,(UINT8)(I+1),{4,0}};
    Options[I]=(EFI_BOOT_MANAGER_LOAD_OPTION){.OptionNumber=I+1,.OptionType=LoadOptionTypeBoot,
      .Attributes=LOAD_OPTION_ACTIVE,.FilePath=&Paths[I],.OptionalData=Optional,.OptionalDataSize=sizeof(Optional)};
  }
  NextOption=Options[2];NextOption.OptionNumber=NextNumber;
}
STATIC VOID TestFallback(VOID){
  PIANO_FV_APPLICATION App={0};EFI_HANDLE Device;
  PIANO_BOOT_DISPATCH Dispatch={.Selection=PianoBootAutomatic,.PrepareSystem=Prepare,
    .CanTryNextSystem=Retry,.FinishSystem=FinishSystem,.WatchdogExpired=Expired};

  Reset();HasNext=AllowRetry=HasAlternatePath=TRUE;Count=3;Options[2]=NextOption;
  ImageStatus[2]=EFI_SUCCESS;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(!HasNext&&NextDeletes==1&&NextSaves==1&&RetryChecks==1&&!AlternatePaths);
  assert(Loads==3&&Order[0]==3&&Order[1]==1&&Order[2]==2&&Device==Volume);
  assert(Dispatch.StartedSystem&&App.StartCalled&&Current==MAX_UINT16);

  Reset();AllowRetry=TRUE;Count=3;ImageStatus[3]=EFI_SUCCESS;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(RetryChecks==1&&Loads==3&&Order[0]==1&&Order[1]==2&&Order[2]==3);

  Reset();AllowRetry=TRUE;HasNext=TRUE;Count=3;Options[1].Attributes=0;Options[2]=NextOption;
  ImageStatus[3]=EFI_TIMEOUT;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_TIMEOUT);
  assert(RetryChecks==1&&Loads==2&&Dispatch.StartedSystem&&mOptionNumber==NextNumber);

  Reset();AllowRetry=HasNext=TRUE;Count=3;
  Dispatch.Selection=PianoBootSelected;Dispatch.Number=2;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==2&&HasNext&&!NextDeletes&&!RetryChecks&&App.StartCalled);

  Reset();AllowRetry=TRUE;
  Dispatch.Selection=PianoBootAutomatic;
  assert(PianoRefreshBootOptions()==EFI_SUCCESS);
  Options[0].FilePath=&MenuPath.Volume.Header;Options[0].OptionalData=NULL;Options[0].OptionalDataSize=0;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(Loads==1&&Dispatch.StartedMenu&&!Dispatch.StartedSystem&&!RetryChecks);

  Reset();Count=3;AllowRetry=TRUE;ClearStatus=EFI_WRITE_PROTECTED;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_WRITE_PROTECTED);
  assert(Loads==1&&Current==1&&!RetryChecks);

  Reset();Count=3;AllowRetry=HasNext=TRUE;ClearStatus=EFI_DEVICE_ERROR;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_DEVICE_ERROR);
  assert(Loads==1&&Current==NextNumber&&!RetryChecks&&Dispatch.StartedSystem);

  Reset();Count=3;AllowRetry=TRUE;SetStatus=EFI_WRITE_PROTECTED;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_WRITE_PROTECTED);
  assert(!Loads&&!CurrentDeletes&&!ReadySignals&&!RetryChecks&&FinishCalls==1&&!BootPath);

  Reset();HasNext=AllowRetry=RetainApplication=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&App.Retained&&!RetryChecks&&!CurrentDeletes&&!FinishCalls&&BootPath);
  memset(&App,0,sizeof(App));

  Reset();HasNext=AllowRetry=ExitOnStart=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&App.EbsObserved&&!RetryChecks&&!CurrentDeletes&&!FinishCalls&&BootPath);
  memset(&App,0,sizeof(App));

  Reset();HasNext=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&RetryChecks==1&&Dispatch.StartedSystem&&App.StartCalled);
}
STATIC VOID TestBootNextSize(VOID){
  PIANO_FV_APPLICATION App;
  EFI_HANDLE Device;
  PIANO_BOOT_DISPATCH Dispatch={.Selection=PianoBootAutomatic,.PrepareSystem=Prepare,
    .FinishSystem=FinishSystem,.WatchdogExpired=Expired};
  CONST UINTN InvalidSizes[]={1,3};
  for(UINTN I=0;I<ARRAY_SIZE(InvalidSizes);++I){
    Reset();memset(&App,0,sizeof(App));HasNext=TRUE;NextSize=InvalidSizes[I];ImageStatus[2]=EFI_SUCCESS;
    assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
    assert(Loads==2&&Order[0]==1&&Order[1]==2&&Device==Volume);
    assert(HasNext&&!NextDeletes&&!NextSaves&&Dispatch.StartedSystem);
  }

  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;NextReadStatus=EFI_DEVICE_ERROR;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_DEVICE_ERROR);
  assert(!Loads&&!PrepareCalls&&!NextDeletes&&!NextSaves);
}
STATIC VOID TestBootNextPersistence(VOID){
  PIANO_FV_APPLICATION App={0};
  EFI_HANDLE Device;
  PIANO_BOOT_DISPATCH Dispatch={.Selection=PianoBootAutomatic,.PrepareSystem=Prepare,
    .FinishSystem=FinishSystem,.WatchdogExpired=Expired};

  Reset();HasNext=DeferredNext=TRUE;ImageStatus[2]=EFI_SUCCESS;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(HasNext&&!NextDeletes&&!NextSaves&&Loads==2&&Order[0]==1&&Order[1]==2);
  memset(&App,0,sizeof(App));Loads=CurrentSets=0;
  Dispatch.Selection=PianoBootSelected;Dispatch.Number=23;ImageStatus[3]=EFI_SUCCESS;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(HasNext&&!NextDeletes&&!NextSaves&&Loads==1&&Order[0]==3);

  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;SaveStatus=EFI_NOT_STARTED;
  Dispatch.Selection=PianoBootAutomatic;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(!HasNext&&NextDeletes==1&&NextSaves==1&&Loads==1&&Order[0]==3);

  CONST EFI_STATUS OptionalSaveErrors[]={EFI_VOLUME_FULL,EFI_OUT_OF_RESOURCES,EFI_WRITE_PROTECTED};
  for(UINTN I=0;I<ARRAY_SIZE(OptionalSaveErrors);++I){
    Reset();memset(&App,0,sizeof(App));HasNext=TRUE;SaveStatus=OptionalSaveErrors[I];ImageStatus[2]=EFI_SUCCESS;
    assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
    assert(!HasNext&&NextDeletes==1&&NextSaves==1&&Loads==2&&Order[0]==1&&Order[1]==2);
  }
}
int main(void){
  PIANO_FV_APPLICATION App;EFI_HANDLE Device;
  PIANO_BOOT_DISPATCH Dispatch={.Selection=PianoBootAutomatic,.PrepareSystem=Prepare,
    .FinishSystem=FinishSystem,.WatchdogExpired=Expired};
  Reset();memset(&App,0,sizeof(App));
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==2&&PrepareCalls==2&&FinishCalls==2&&Order[0]==1&&Order[1]==2&&Device==Volume);
  assert(ReadySignals==2&&CurrentSets==2&&CurrentDeletes==2&&!NextDeletes);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==3&&NextDeletes==1&&NextSaves==1&&!HasNext);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;NextOption.Attributes=0;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==3&&NextDeletes==1);
  Reset();memset(&App,0,sizeof(App));HasNext=NextMissing=TRUE;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==2&&Order[0]==1&&NextDeletes==1);
  Reset();memset(&App,0,sizeof(App));Options[0].Attributes=0;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(Loads==1&&Order[0]==2);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;SaveStatus=EFI_DEVICE_ERROR;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_DEVICE_ERROR);
  assert(NextDeletes==1&&NextSaves==1&&!Loads&&!PrepareCalls);
  Reset();Count=0;
  assert(PianoRefreshBootOptions()==EFI_SUCCESS&&Adds==1&&Position==0&&Refreshes==1);
  assert(PianoRefreshBootOptions()==EFI_SUCCESS&&Adds==1&&Refreshes==2);
  Reset();Options[0].FilePath=&RetiredPath.Volume.Header;
  assert(PianoRefreshBootOptions()==EFI_SUCCESS&&Deletes==1&&Adds==1&&Position==MAX_UINTN);
  Options[0].FilePath=&MenuPath.Volume.Header;Options[0].OptionalData=NULL;Options[0].OptionalDataSize=0;
  memset(&App,0,sizeof(App));
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(Loads==1&&!PrepareCalls&&Dispatch.StartedMenu&&!Device&&CurrentDeletes==1);
  memset(&App,0,sizeof(App));Loads=CurrentSets=0;HasNext=TRUE;Dispatch.Selection=PianoBootSystems;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(HasNext&&!NextDeletes&&!NextSaves&&Loads==1&&Order[0]==2&&!Dispatch.StartedMenu);
  Reset();memset(&App,0,sizeof(App));HasNext=TRUE;
  Dispatch.Selection=PianoBootSelected;Dispatch.Number=2;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_LOAD_ERROR);
  assert(HasNext&&!NextDeletes&&Loads==1&&Order[0]==2);
  Reset();memset(&App,0,sizeof(App));PrepareStatus=EFI_NOT_READY;Dispatch.Selection=PianoBootAutomatic;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_NOT_READY);
  assert(Dispatch.SystemBlocked&&PrepareCalls==1&&!Loads&&!CurrentSets);
  Reset();memset(&App,0,sizeof(App));FilesystemStatus=EFI_NOT_FOUND;
  Dispatch.Selection=PianoBootSelected;Dispatch.Number=2;ImageStatus[2]=EFI_SUCCESS;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_SUCCESS);
  assert(LoadFileLookups==1&&Loads==1&&Order[0]==2&&Device==LoadFile&&FinishCalls==1);
  Reset();memset(&App,0,sizeof(App));FilesystemStatus=EFI_DEVICE_ERROR;
  assert(PianoStartBootOption((VOID *)1,&Device,&App,&Dispatch)==EFI_DEVICE_ERROR);
  assert(!LoadFileLookups&&!Loads&&!PrepareCalls&&!FinishCalls);
  TestFallback();
  TestBootNextSize();
  TestBootNextPersistence();
  Reset();
  puts("BootOrder, one-time choices, NV save, firmware entry and binary load options passed");
  return 0;
}
