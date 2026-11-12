// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small returning AA64 EFI app: inspect its own loaded image and publish one
// boot-service-only volatile record. No disk, device, DMA or reset operations.
#include "PianoRamBootProbe.h"
#include <Protocol/LoadedImage.h>
STATIC EFI_GUID mLoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
STATIC EFI_GUID mRecordGuid=PIANO_RAM_BOOT_PROBE_GUID;
// Keep one real relocation in the PE, so the fixture exercises relocation at
// a loader-selected address rather than depending solely on PC-relative code.
EFI_STATUS (EFIAPI *volatile PianoRamBootProbeRelocation)(EFI_HANDLE,EFI_SYSTEM_TABLE *)=PianoRamBootProbeEntry;
STATIC UINT32 Crc(CONST VOID *Buffer,UINTN Bytes) {
  CONST UINT8 *P=Buffer;UINT32 Value=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I){Value^=P[I];for(UINTN J=0;J<8;++J)Value=(Value>>1)^((Value&1)?0xedb88320U:0);}
  return ~Value;
}
STATIC EFI_STATUS Exact(EFI_STATUS Status) {return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;}
EFI_STATUS EFIAPI PianoRamBootProbeEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(Image==NULL || St==NULL || St->Hdr.Signature!=EFI_SYSTEM_TABLE_SIGNATURE || St->Hdr.HeaderSize<sizeof(*St) ||
     St->BootServices==NULL || St->BootServices->Hdr.Signature!=EFI_BOOT_SERVICES_SIGNATURE ||
     St->BootServices->Hdr.HeaderSize<sizeof(EFI_BOOT_SERVICES) || St->BootServices->HandleProtocol==NULL ||
     St->RuntimeServices==NULL || St->RuntimeServices->Hdr.Signature!=EFI_RUNTIME_SERVICES_SIGNATURE ||
     St->RuntimeServices->Hdr.HeaderSize<sizeof(EFI_RUNTIME_SERVICES) || St->RuntimeServices->GetVariable==NULL ||
     St->RuntimeServices->SetVariable==NULL)return EFI_INVALID_PARAMETER;
  if(PianoRamBootProbeRelocation!=PianoRamBootProbeEntry)return EFI_COMPROMISED_DATA;
  PIANO_RAM_BOOT_PROBE_RECORD Record;
  for(UINTN I=0;I<sizeof(Record);++I)((UINT8 *)&Record)[I]=0;
  Record.Signature=PIANO_RAM_BOOT_PROBE_SIGNATURE;Record.Revision=1;Record.Bytes=sizeof(Record);Record.Flags=BIT0;
  Record.SystemTableRevision=St->Hdr.Revision;Record.BootServicesRevision=St->BootServices->Hdr.Revision;
#ifdef __aarch64__
  __asm__ volatile("mrs %0, CurrentEL":"=r"(Record.CurrentEl));Record.Flags|=BIT1;
#endif
  EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  Record.LoadedStatus=St->BootServices->HandleProtocol(Image,&mLoadedGuid,(VOID **)&Loaded);
  if(Record.LoadedStatus!=EFI_SUCCESS)return Exact(Record.LoadedStatus);
  if(Loaded==NULL || Loaded->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION || Loaded->ImageBase==NULL ||
     !Loaded->ImageSize)return EFI_COMPROMISED_DATA;
  Record.ImageBase=(UINTN)Loaded->ImageBase;Record.ImageBytes=Loaded->ImageSize;
  Record.ImageHandle=(UINTN)Image;Record.ParentHandle=(UINTN)Loaded->ParentHandle;Record.Flags|=BIT2;
  Record.ConsoleStatus=EFI_NOT_FOUND;
  if(St->ConOut!=NULL && St->ConOut->OutputString!=NULL)
    Record.ConsoleStatus=St->ConOut->OutputString(St->ConOut,L"SunUEFI RAM EFI probe: entry reached, returning to firmware.\r\n");
  if(Record.ConsoleStatus==EFI_SUCCESS)Record.Flags|=BIT3;
  // Never overwrite an existing value, even a volatile one. A parent that
  // wants another invocation must first read and explicitly remove its record.
  UINTN ExistingBytes=0;UINT32 Attributes=0;
  EFI_STATUS Status=St->RuntimeServices->GetVariable(PIANO_RAM_BOOT_PROBE_NAME,&mRecordGuid,&Attributes,&ExistingBytes,NULL);
  if(Status==EFI_SUCCESS || Status==EFI_BUFFER_TOO_SMALL)return EFI_ALREADY_STARTED;
  if(Status!=EFI_NOT_FOUND)return Exact(Status);
  Record.Crc32=Crc(&Record,sizeof(Record));
  return Exact(St->RuntimeServices->SetVariable(PIANO_RAM_BOOT_PROBE_NAME,&mRecordGuid,
    EFI_VARIABLE_BOOTSERVICE_ACCESS,sizeof(Record),&Record));
}
