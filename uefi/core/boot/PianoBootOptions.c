// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <PiDxe.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include "Components/os-boot/PianoEspBootSource.h"
STATIC EFI_STATUS mFileStatus=EFI_NOT_STARTED,mOptionStatus=EFI_NOT_STARTED;
VOID PianoReportLinuxBootOption(VOID) {
  DEBUG((DEBUG_WARN,"PIANO_LINUX_BOOT_OPTION_SAVED file=%r register=%r\n",mFileStatus,mOptionStatus));
  CONST PIANO_ESP_DISCOVERY *D=PianoEspBootDiscovery();
  DEBUG((DEBUG_WARN,"PIANO_ESP_DISCOVERY locate=%r sfs=%u matches=%u open=%r\n",D->Locate,D->Filesystems,D->Matches,D->Open));
  for(UINTN I=0;I<D->Candidates&&I<ARRAY_SIZE(D->Candidate);++I)
    DEBUG((DEBUG_WARN,"PIANO_ESP_CANDIDATE index=%u stage=%u name=%s revision=%x type=%u block=%u last=%lx first_lba=%lx last_lba=%lx pun=%u lun=%u\n",
      (UINT32)I,D->Candidate[I].Stage,D->Candidate[I].Name,D->Candidate[I].Revision,D->Candidate[I].Type,
      D->Candidate[I].BlockSize,D->Candidate[I].LastBlock,D->Candidate[I].FirstLba,D->Candidate[I].LastLba,
      D->Candidate[I].Pun,D->Candidate[I].Lun));
}

EFI_STATUS PianoRegisterLinuxBootOption(VOID) {
  EFI_STATUS Status=PianoEspBootPresent();
  mFileStatus=Status;
  DEBUG((DEBUG_WARN,"PIANO_LINUX_BOOT_OPTION stage=esp-file status=%r\n",Status));
  if(Status!=EFI_SUCCESS)return Status;
  EFI_GUID File={0x9ddefc13,0x4746,0x441b,{0x87,0xe9,0x4c,0xd3,0xe1,0x21,0xb0,0x84}};
  EFI_HANDLE *Volumes=NULL;UINTN Count=0;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiFirmwareVolume2ProtocolGuid,NULL,&Count,&Volumes);
  if(Status!=EFI_SUCCESS)return Status;
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv=NULL;VOID *Image=NULL;UINTN Bytes=0;UINT32 Auth=0;
    if(gBS->HandleProtocol(Volumes[I],&gEfiFirmwareVolume2ProtocolGuid,(VOID **)&Fv)!=EFI_SUCCESS||!Fv)continue;
    EFI_STATUS Read=Fv->ReadSection(Fv,&File,EFI_SECTION_PE32,0,&Image,&Bytes,&Auth);
    if(Image)FreePool(Image);
    if(Read!=EFI_SUCCESS||!Bytes)continue;
    EFI_DEVICE_PATH_PROTOCOL *Base=DevicePathFromHandle(Volumes[I]);
    if(!Base)continue;
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Node;
    ZeroMem(&Node,sizeof(Node));Node.Header.Type=MEDIA_DEVICE_PATH;Node.Header.SubType=MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength(&Node.Header,sizeof(Node));Node.FvFileName=File;
    EFI_DEVICE_PATH_PROTOCOL *Path=AppendDevicePathNode(Base,&Node.Header);
    if(!Path){Status=EFI_OUT_OF_RESOURCES;break;}
    EFI_BOOT_MANAGER_LOAD_OPTION Option;
    Status=EfiBootManagerInitializeLoadOption(&Option,LoadOptionNumberUnassigned,LoadOptionTypeBoot,
      LOAD_OPTION_ACTIVE,L"Linux",Path,NULL,0);
    FreePool(Path);
    if(Status==EFI_SUCCESS){
      // Variables now survive reboot. Reuse the standard matching option
      // instead of appending another Boot#### on every product startup.
      UINTN ExistingCount=0;
      EFI_BOOT_MANAGER_LOAD_OPTION *Existing=EfiBootManagerGetLoadOptions(&ExistingCount,LoadOptionTypeBoot);
      if(EfiBootManagerFindLoadOption(&Option,Existing,ExistingCount)<0)
        Status=EfiBootManagerAddLoadOptionVariable(&Option,MAX_UINTN);
      EfiBootManagerFreeLoadOptions(Existing,ExistingCount);EfiBootManagerFreeLoadOption(&Option);
    }
    break;
  }
  FreePool(Volumes);
  mOptionStatus=Status;
  DEBUG((DEBUG_WARN,"PIANO_LINUX_BOOT_OPTION stage=register status=%r\n",Status));
  return Status;
}
