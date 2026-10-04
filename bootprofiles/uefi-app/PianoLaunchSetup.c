// SPDX-License-Identifier: BSD-2-Clause-Patent
// Launch genuine pinned UiApp, while RamApp retains UFS/DMA/keyboard services.
#include <PiDxe.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/HiiDatabase.h>
#include <Protocol/HiiString.h>
#include <Protocol/HiiFont.h>
#include <Protocol/HiiConfigRouting.h>
#include <Protocol/FormBrowser2.h>
#include <Protocol/DisplayProtocol.h>
#include <Protocol/Variable.h>
#include <Protocol/VariableWrite.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/DebugLib.h>

STATIC EFI_GUID mUiAppFile={0x462CAA21,0x7614,0x4503,{0x83,0x6E,0x8A,0xB6,0xF4,0x66,0x23,0x31}};
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable);
STATIC BOOLEAN mSetupRequested,mSetupStartCalled,mSetupStartReturned;
STATIC EFI_STATUS mSetupServices=EFI_NOT_STARTED,mSetupLoad=EFI_NOT_STARTED,mSetupImage=EFI_NOT_STARTED,mSetupStart=EFI_NOT_STARTED;
VOID PianoReportSetupDiagnostics(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_REPORT requested=%u services=%r load=%r image=%r start_called=%u start_returned=%u start=%r backend=emu-ram persistent=0\n",
    mSetupRequested,mSetupServices,mSetupLoad,mSetupImage,mSetupStartCalled,mSetupStartReturned,mSetupStart));
}

STATIC EFI_STATUS CheckSetupServices(VOID) {
  // This stage cannot silently enable a hardware Flash/FVB variable backend.
  if(!PcdGetBool(PcdEmuVariableNvModeEnable)) {
    DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_REJECT variables_not_emu_ram=1\n"));return EFI_ACCESS_DENIED;
  }
  STATIC struct {EFI_GUID *Guid;CONST CHAR8 *Name;} Services[]={
    {&gEfiHiiDatabaseProtocolGuid,"HiiDatabase"},
    {&gEfiHiiStringProtocolGuid,"HiiString"},
    {&gEfiHiiFontProtocolGuid,"HiiFont"},
    {&gEfiHiiConfigRoutingProtocolGuid,"HiiConfigRouting"},
    {&gEfiFormBrowser2ProtocolGuid,"FormBrowser2"},
    {&gEdkiiFormDisplayEngineProtocolGuid,"FormDisplayEngine"},
    {&gEfiVariableArchProtocolGuid,"VariableArch"},
    {&gEfiVariableWriteArchProtocolGuid,"VariableWriteArch"}
  };
  EFI_STATUS Result=EFI_SUCCESS;
  for(UINTN I=0;I<ARRAY_SIZE(Services);++I) {
    VOID *Protocol=NULL;
    EFI_STATUS Status=gBS->LocateProtocol(Services[I].Guid,NULL,&Protocol);
    // Variable architectural protocols are marker protocols with NULL interface.
    if(I<6 && !EFI_ERROR(Status) && Protocol==NULL)Status=EFI_DEVICE_ERROR;
    DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_PROTOCOL %a %r\n",Services[I].Name,Status));
    if(EFI_ERROR(Status) && !EFI_ERROR(Result))Result=Status;
  }
  return Result;
}

STATIC EFI_STATUS LoadSetup(EFI_HANDLE Parent,EFI_HANDLE *Image) {
  EFI_HANDLE *Volumes=NULL;UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiFirmwareVolume2ProtocolGuid,NULL,&Count,&Volumes);
  if(EFI_ERROR(Status))return Status;
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv=NULL;VOID *Source=NULL;UINTN Bytes=0;UINT32 Authentication=0;
    if(EFI_ERROR(gBS->HandleProtocol(Volumes[I],&gEfiFirmwareVolume2ProtocolGuid,(VOID **)&Fv)))continue;
    EFI_STATUS Read=Fv->ReadSection(Fv,&mUiAppFile,EFI_SECTION_PE32,0,&Source,&Bytes,&Authentication);
    if(EFI_ERROR(Read)){if(Source!=NULL)FreePool(Source);continue;}
    if(Source==NULL || Bytes==0 || Bytes>0x2000000) {
      if(Source!=NULL)FreePool(Source);Status=EFI_COMPROMISED_DATA;continue;
    }
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH File;
    ZeroMem(&File,sizeof(File));File.Header.Type=MEDIA_DEVICE_PATH;File.Header.SubType=MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength(&File.Header,sizeof(File));File.FvFileName=mUiAppFile;
    EFI_DEVICE_PATH_PROTOCOL *Base=DevicePathFromHandle(Volumes[I]);
    EFI_DEVICE_PATH_PROTOCOL *Path=Base==NULL?NULL:AppendDevicePathNode(Base,&File.Header);
    Status=Path==NULL?EFI_NOT_FOUND:gBS->LoadImage(FALSE,Parent,Path,Source,Bytes,Image);
    if(Path!=NULL)FreePool(Path);FreePool(Source);
    DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_FV_LOAD volume=%p status=%r bytes=%lu real_fv_path=%u\n",
      Volumes[I],Status,(UINT64)Bytes,Base!=NULL));
    if(!EFI_ERROR(Status))break;
  }
  if(Volumes!=NULL)FreePool(Volumes);return Status;
}

EFI_STATUS PianoLaunchSetup(EFI_HANDLE Parent) {
  mSetupRequested=TRUE;mSetupStartCalled=mSetupStartReturned=FALSE;
  mSetupServices=mSetupLoad=mSetupImage=mSetupStart=EFI_NOT_STARTED;
  EFI_STATUS Status=CheckSetupServices();mSetupServices=Status;
  if(EFI_ERROR(Status))return Status;
  EFI_HANDLE Image=NULL;Status=LoadSetup(Parent,&Image);mSetupLoad=Status;
  if(EFI_ERROR(Status))return Status;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(!EFI_ERROR(Status) && (Loaded==NULL || Loaded->ImageCodeType!=EfiLoaderCode))Status=EFI_UNSUPPORTED;
  mSetupImage=Status;
  if(EFI_ERROR(Status)){gBS->UnloadImage(Image);return Status;}
  Loaded->LoadOptions=NULL;Loaded->LoadOptionsSize=0;
  Print(L"\r\nTianoCore Setup: settings are temporary and lost after restart.\r\n");
  DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_START app=UiApp backend=emu-ram persistent=0 hardware_verified=0\n"));
  BOOLEAN PreviousNavigation=PianoSetStandardKeyNavigation(TRUE);
  mSetupStartCalled=TRUE;
  Status=gBS->StartImage(Image,NULL,NULL);
  mSetupStart=Status;mSetupStartReturned=TRUE;
  PianoSetStandardKeyNavigation(PreviousNavigation);
  // UiApp is a UEFI application; normal return/Exit unloads it and runs its
  // library destructors. RamApp's provider lifetime continues until teardown.
  DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_RETURN %r backend=emu-ram persistent=0\n",Status));
  Print(L"Setup returned. Its settings will not survive a restart.\r\n");
  return Status;
}
