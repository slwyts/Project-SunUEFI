// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFvApplication.h"
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>

#define SIGNATURE SIGNATURE_32('P','F','V','1')
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC VOID Alive(PIANO_FV_APPLICATION *C) {
  if(C->EbsObserved){C->Retained=TRUE;C->Result=EFI_ABORTED;
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();}
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;((PIANO_FV_APPLICATION *)Context)->EbsObserved=TRUE;}
STATIC EFI_STATUS Free(PIANO_FV_APPLICATION *C,VOID **Pointer) {
  if(*Pointer==NULL)return EFI_SUCCESS;
  Alive(C);
  EFI_STATUS S=Exact(gBS->FreePool(*Pointer));Alive(C);
  if(S==EFI_SUCCESS)*Pointer=NULL;else C->Retained=TRUE;
  return S;
}
STATIC EFI_STATUS Retire(PIANO_FV_APPLICATION *C) {
  if(C->Image==NULL)return EFI_SUCCESS;
  Alive(C);
  EFI_LOADED_IMAGE_PROTOCOL *L=NULL;
  EFI_STATUS S=gBS->HandleProtocol(C->Image,&gEfiLoadedImageProtocolGuid,(VOID **)&L);Alive(C);
  if(C->StartReturned && (S==EFI_NOT_FOUND || S==EFI_INVALID_PARAMETER || S==EFI_UNSUPPORTED)) {
    C->Image=NULL;C->OptionsInstalled=FALSE;C->AutoUnloaded=TRUE;return EFI_SUCCESS;
  }
  if(S!=EFI_SUCCESS || L==NULL)return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
  if(C->LoadedIdentity && (L!=C->LoadedIdentity || L->ImageBase!=C->BaseIdentity || L->ImageSize!=C->SizeIdentity))return EFI_COMPROMISED_DATA;
  if(C->OptionsInstalled) {
    if(L->LoadOptions!=C->OptionsCopy || L->LoadOptionsSize!=C->OptionsBytes)return EFI_COMPROMISED_DATA;
    L->LoadOptions=C->OriginalOptions;L->LoadOptionsSize=C->OriginalOptionsBytes;C->OptionsInstalled=FALSE;
  }
  S=Exact(gBS->UnloadImage(C->Image));Alive(C);if(S==EFI_SUCCESS)C->Image=NULL;return S;
}
STATIC EFI_STATUS Cleanup(PIANO_FV_APPLICATION *C) {
  EFI_STATUS S=Retire(C);C->Retire=S;if(S!=EFI_SUCCESS)return S;
  S=Free(C,&C->ExitData);if(S!=EFI_SUCCESS)return S;
  if(C->OptionsCopy){Alive(C);ZeroMem(C->OptionsCopy,C->OptionsBytes);S=Free(C,&C->OptionsCopy);if(S!=EFI_SUCCESS)return S;}
  S=Free(C,&C->SourceOwned);if(S!=EFI_SUCCESS)return S;
  S=Free(C,&C->PathOwned);if(S!=EFI_SUCCESS)return S;
  S=Free(C,&C->HandlesOwned);if(S!=EFI_SUCCESS)return S;
  if(C->ExitEvent) {
    Alive(C);S=Exact(gBS->CloseEvent(C->ExitEvent));Alive(C);if(S!=EFI_SUCCESS)return S;C->ExitEvent=NULL;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Begin(PIANO_FV_APPLICATION *C,EFI_HANDLE Parent,UINT64 Limit) {
  if(!C || !Parent || !Limit || Limit>0x10000000 || !gBS)return EFI_INVALID_PARAMETER;
  if(C->Signature==SIGNATURE && (C->Busy || C->Retained))return EFI_ALREADY_STARTED;
  if(!gBS->RaiseTPL || !gBS->RestoreTPL || !gBS->CreateEventEx || !gBS->CloseEvent || !gBS->LoadImage || !gBS->StartImage || !gBS->UnloadImage ||
     !gBS->HandleProtocol || !gBS->FreePool || !gBS->AllocatePool)return EFI_UNSUPPORTED;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  if(Old!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  ZeroMem(C,sizeof(*C));C->Signature=SIGNATURE;C->Busy=TRUE;C->Load=C->Start=C->Retire=C->Cleanup=EFI_NOT_STARTED;
  EFI_STATUS S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,C,&gEfiEventExitBootServicesGuid,&C->ExitEvent));Alive(C);
  if(S==EFI_SUCCESS && C->ExitEvent==NULL){C->Retained=TRUE;return EFI_COMPROMISED_DATA;}
  return S;
}
STATIC EFI_STATUS Start(PIANO_FV_APPLICATION *C,CONST CHAR16 *Options,UINT64 Limit) {
  Alive(C);EFI_LOADED_IMAGE_PROTOCOL *L=NULL;
  EFI_STATUS S=Exact(gBS->HandleProtocol(C->Image,&gEfiLoadedImageProtocolGuid,(VOID **)&L));Alive(C);
  if(S!=EFI_SUCCESS)return S;
  if(!L || L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION || L->ImageCodeType!=EfiLoaderCode || !L->ImageBase || !L->ImageSize || L->ImageSize>Limit)return EFI_UNSUPPORTED;
  C->LoadedIdentity=L;C->BaseIdentity=L->ImageBase;C->SizeIdentity=L->ImageSize;
  if(Options) {
    UINTN Chars=0;while(Chars<2048 && Options[Chars])++Chars;if(Chars==2048)return EFI_BAD_BUFFER_SIZE;
    C->OptionsBytes=(UINT32)((Chars+1)*sizeof(CHAR16));C->OriginalOptions=L->LoadOptions;C->OriginalOptionsBytes=L->LoadOptionsSize;
    S=Exact(gBS->AllocatePool(EfiLoaderData,C->OptionsBytes,&C->OptionsCopy));Alive(C);if(S!=EFI_SUCCESS)return S;
    if(!C->OptionsCopy){C->Retained=TRUE;return EFI_COMPROMISED_DATA;}
    CopyMem(C->OptionsCopy,Options,C->OptionsBytes);L->LoadOptions=C->OptionsCopy;L->LoadOptionsSize=C->OptionsBytes;C->OptionsInstalled=TRUE;
  }
  C->StartCalled=TRUE;S=gBS->StartImage(C->Image,&C->ExitBytes,(CHAR16 **)&C->ExitData);C->Start=S;C->StartReturned=TRUE;Alive(C);return Exact(S);
}
STATIC EFI_STATUS Finish(PIANO_FV_APPLICATION *C,EFI_STATUS S) {
  C->Result=S;
  if(C->Retained)return S;
  C->Cleanup=Cleanup(C);
  if(C->Cleanup!=EFI_SUCCESS){C->Retained=TRUE;return C->Result=S==EFI_SUCCESS?C->Cleanup:S;}
  C->Busy=FALSE;return S;
}
EFI_STATUS PianoApplicationRunBuffer(EFI_HANDLE Parent,CONST VOID *Image,UINTN Bytes,
  CONST CHAR16 *Options,UINT64 Limit,PIANO_FV_APPLICATION *C) {
  if(C && C->Signature==SIGNATURE && (C->Busy || C->Retained))return EFI_ALREADY_STARTED;
  if(!Image || Bytes<4096 || Bytes>0x4000000)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Begin(C,Parent,Limit);if(S!=EFI_SUCCESS)return C && C->Signature==SIGNATURE && C->Busy?Finish(C,S):S;
  S=Exact(gBS->LoadImage(FALSE,Parent,NULL,(VOID *)Image,Bytes,&C->Image));C->Load=S;Alive(C);
  if(S==EFI_SUCCESS && !C->Image){C->Retained=TRUE;S=EFI_COMPROMISED_DATA;}
  if(S==EFI_SUCCESS)S=Start(C,Options,Limit);
  return Finish(C,S);
}
EFI_STATUS PianoFvApplicationRun(EFI_HANDLE Parent,CONST EFI_GUID *File,CONST CHAR16 *Options,
  UINT64 Limit,PIANO_FV_APPLICATION *C) {
  if(C && C->Signature==SIGNATURE && (C->Busy || C->Retained))return EFI_ALREADY_STARTED;
  if(!File)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Begin(C,Parent,Limit);if(S!=EFI_SUCCESS)return C && C->Signature==SIGNATURE && C->Busy?Finish(C,S):S;
  if(!gBS->LocateHandleBuffer){return Finish(C,EFI_UNSUPPORTED);}
  UINTN Count=0;EFI_HANDLE *Handles=NULL;
  S=Exact(gBS->LocateHandleBuffer(ByProtocol,&gEfiFirmwareVolume2ProtocolGuid,NULL,&Count,&Handles));Alive(C);C->HandlesOwned=Handles;
  if(S!=EFI_SUCCESS)return Finish(C,S);
  // A successful enumeration must be bounded and internally consistent before
  // any handle dereference. The returned allocation remains ours to retire.
  if(!Count || !Handles || Count>256)return Finish(C,EFI_COMPROMISED_DATA);
  S=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;++I) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv=NULL;
    EFI_STATUS Get=gBS->HandleProtocol(Handles[I],&gEfiFirmwareVolume2ProtocolGuid,(VOID **)&Fv);Alive(C);
    if(Get!=EFI_SUCCESS || !Fv || !Fv->ReadSection)continue;
    UINTN Bytes=0;UINT32 Auth=0;
    Get=Fv->ReadSection(Fv,File,EFI_SECTION_PE32,0,&C->SourceOwned,&Bytes,&Auth);Alive(C);
    if(Get!=EFI_SUCCESS){EFI_STATUS F=Free(C,&C->SourceOwned);if(F!=EFI_SUCCESS)return Finish(C,F);continue;}
    if(!C->SourceOwned || !Bytes || Bytes>0x2000000 || (Auth&EFI_AUTH_STATUS_TEST_FAILED)){S=EFI_SECURITY_VIOLATION;break;}
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Node;ZeroMem(&Node,sizeof(Node));Node.Header.Type=MEDIA_DEVICE_PATH;
    Node.Header.SubType=MEDIA_PIWG_FW_FILE_DP;SetDevicePathNodeLength(&Node.Header,sizeof(Node));Node.FvFileName=*File;
    EFI_DEVICE_PATH_PROTOCOL *Base=DevicePathFromHandle(Handles[I]);
    C->PathOwned=Base?AppendDevicePathNode(Base,&Node.Header):NULL;
    Alive(C);
    if(!C->PathOwned){S=EFI_NOT_FOUND;break;}
    S=Exact(gBS->LoadImage(FALSE,Parent,C->PathOwned,C->SourceOwned,Bytes,&C->Image));C->Load=S;Alive(C);
    if(S==EFI_SUCCESS && !C->Image){C->Retained=TRUE;S=EFI_COMPROMISED_DATA;}
    break;
  }
  if(S==EFI_SUCCESS){S=Free(C,&C->SourceOwned);if(S==EFI_SUCCESS)S=Free(C,&C->PathOwned);if(S==EFI_SUCCESS)S=Free(C,&C->HandlesOwned);}
  if(S==EFI_SUCCESS)S=Start(C,Options,Limit);
  return Finish(C,S);
}
