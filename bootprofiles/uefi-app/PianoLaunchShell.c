// SPDX-License-Identifier: BSD-2-Clause-Patent
// Launch the pinned UEFI Shell in FV while RamApp still owns UFS/keys/DMA.
#include <PiDxe.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>

STATIC EFI_GUID mShellFile={0x7C04A583,0x9E3E,0x4F1C,{0xAD,0x65,0xE0,0x52,0x68,0xD0,0xB4,0xD1}};
typedef struct {
  CONST CHAR8 *Name;
  BOOLEAN Requested,StartCalled,StartReturned,UnloadCalled,Application;
  EFI_STATUS Load,ImageProtocol,Start,Unload;
} SHELL_REPORT;
STATIC SHELL_REPORT mShellReports[3],mInteractiveReport;
STATIC BOOLEAN mShellReportsInitialized;

VOID PianoReportShellDiagnostics(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_REPORT_BEGIN initialized=%u planned_commands=3\n",mShellReportsInitialized));
  if(!mShellReportsInitialized) {
    DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_REPORT_NOT_STARTED commands_requested=0\n"));return;
  }
  for(UINTN I=0;I<ARRAY_SIZE(mShellReports)+1;++I) {
    SHELL_REPORT *R=I<ARRAY_SIZE(mShellReports)?&mShellReports[I]:&mInteractiveReport;
    if(I==ARRAY_SIZE(mShellReports) && !R->Requested)continue;
    CONST CHAR8 *Execution=!R->Requested?"not-requested":!R->StartCalled?"not-started":!R->StartReturned?
      "in-progress":EFI_ERROR(R->Start)?"returned-error":"returned-success";
    DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_REPORT command=%a requested=%u execution=%a load=%r image_protocol=%r start_called=%u start_returned=%u start=%r unload_called=%u unload=%r auto_unload_expected=%u\n",
      R->Name,R->Requested,Execution,R->Load,R->ImageProtocol,R->StartCalled,R->StartReturned,R->Start,R->UnloadCalled,R->Unload,R->Application && R->StartReturned));
  }
}

STATIC VOID InitReport(SHELL_REPORT *Report,CONST CHAR8 *Name) {
  ZeroMem(Report,sizeof(*Report));Report->Name=Name;
  Report->Load=Report->ImageProtocol=Report->Start=Report->Unload=EFI_NOT_STARTED;
}
STATIC EFI_TEXT_STRING mOriginalOutput;
STATIC EFI_STATUS EFIAPI LogShellOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *String) {
  // SerialPortLib is RAM-only in this profile. Avoid allocation and retain the
  // real console output; this makes standard Shell map/dh output recoverable.
  CHAR8 Part[161];UINTN I=0;
  for(UINTN N=0;String[N]!=0;++N) {
    CHAR16 C=String[N];if(C=='\r')continue;
    Part[I++]=C<128?(CHAR8)C:'?';
    if(I==sizeof(Part)-1 || C=='\n'){Part[I]=0;DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_OUTPUT %a\n",Part));I=0;}
  }
  if(I){Part[I]=0;DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_OUTPUT %a\n",Part));}
  return mOriginalOutput(This,String);
}

STATIC EFI_STATUS LoadShell(EFI_HANDLE Parent,EFI_HANDLE *Image) {
  EFI_HANDLE *Volumes=NULL;UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiFirmwareVolume2ProtocolGuid,NULL,&Count,&Volumes);
  if(EFI_ERROR(Status))return Status;
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv=NULL;VOID *Source=NULL;UINTN Bytes=0;UINT32 Auth=0;
    if(EFI_ERROR(gBS->HandleProtocol(Volumes[I],&gEfiFirmwareVolume2ProtocolGuid,(VOID **)&Fv)))continue;
    EFI_STATUS Read=Fv->ReadSection(Fv,&mShellFile,EFI_SECTION_PE32,0,&Source,&Bytes,&Auth);
    if(EFI_ERROR(Read))continue;
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH File;
    ZeroMem(&File,sizeof(File));File.Header.Type=MEDIA_DEVICE_PATH;File.Header.SubType=MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength(&File.Header,sizeof(File));File.FvFileName=mShellFile;
    EFI_DEVICE_PATH_PROTOCOL *Base=DevicePathFromHandle(Volumes[I]);
    EFI_DEVICE_PATH_PROTOCOL *Path=Base==NULL?NULL:AppendDevicePathNode(Base,&File.Header);
    // A real FV device path is required for Shell's loaded-image path lookup.
    Status=Path==NULL?EFI_NOT_FOUND:gBS->LoadImage(FALSE,Parent,Path,Source,Bytes,Image);
    if(Path!=NULL)FreePool(Path);if(Source!=NULL)FreePool(Source);
    if(!EFI_ERROR(Status))break;
  }
  FreePool(Volumes);return Status;
}

STATIC EFI_STATUS RunShell(EFI_HANDLE Parent,CONST CHAR16 *Options,SHELL_REPORT *Report) {
  Report->Requested=TRUE;
  EFI_HANDLE Image=NULL;EFI_STATUS Status=LoadShell(Parent,&Image);
  Report->Load=Status;
  DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_LOAD %r options=%s source=firmware-volume variables=emu-ram\n",Status,Options));
  if(EFI_ERROR(Status))return Status;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(!EFI_ERROR(Status) && Loaded==NULL)Status=EFI_DEVICE_ERROR;
  Report->ImageProtocol=Status;
  if(!EFI_ERROR(Status)) {
    Report->Application=Loaded->ImageCodeType==EfiLoaderCode;
    Loaded->LoadOptions=(VOID *)Options;Loaded->LoadOptionsSize=(UINT32)StrSize(Options);
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Console=gST->ConOut;
    if(Console!=NULL && Console->OutputString!=NULL){mOriginalOutput=Console->OutputString;Console->OutputString=LogShellOutput;}
    Report->StartCalled=TRUE;
    Status=gBS->StartImage(Image,NULL,NULL);
    Report->Start=Status;Report->StartReturned=TRUE;
    if(Console!=NULL && mOriginalOutput!=NULL){Console->OutputString=mOriginalOutput;mOriginalOutput=NULL;}
    DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_RETURN %r options=%s\n",Status,Options));
  }
  // UEFI applications are normally unloaded by Exit/return from StartImage.
  // An explicit unload also handles the pre-StartImage failure path.
  Report->UnloadCalled=TRUE;Report->Unload=gBS->UnloadImage(Image);return Status;
}

EFI_STATUS PianoRunShellDiagnostics(EFI_HANDLE Parent,BOOLEAN Interactive) {
  STATIC CONST CHAR16 *Commands[]={
    L"Shell.efi -nostartup -nointerrupt -noconsolein -nomap -noversion -exit map -r",
    L"Shell.efi -nostartup -nointerrupt -noconsolein -nomap -noversion -exit dh -p SimpleFileSystem",
    L"Shell.efi -nostartup -nointerrupt -noconsolein -nomap -noversion -exit drivers"
  };
  STATIC CONST CHAR8 *Names[]={"map-r","dh-simplefilesystem","drivers"};
  for(UINTN I=0;I<ARRAY_SIZE(mShellReports);++I)InitReport(&mShellReports[I],Names[I]);
  InitReport(&mInteractiveReport,"interactive");mShellReportsInitialized=TRUE;
  EFI_STATUS Result=EFI_SUCCESS;
  for(UINTN I=0;I<ARRAY_SIZE(Commands);++I) {
    EFI_STATUS Status=RunShell(Parent,Commands[I],&mShellReports[I]);
    // No SFS is an honest enumeration outcome; log each command result and
    // continue so a missing FAT volume does not suppress driver evidence.
    if(EFI_ERROR(Status) && !EFI_ERROR(Result))Result=Status;
  }
  if(Interactive)Result=RunShell(Parent,L"Shell.efi -nostartup -nointerrupt",&mInteractiveReport);
  PianoReportShellDiagnostics();
  return Result;
}
