// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoNvFvbBinding.h"
#include <Protocol/Variable.h>
#include <Protocol/LoadedImage.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
STATIC EFI_GUID mReady=PIANO_NV_READY_PROTOCOL_GUID;
STATIC VOID EFIAPI Exit(EFI_EVENT E,VOID*C){(VOID)E;PianoNvFvbFenceRuntime(&((PIANO_NV_FVB_BINDING*)C)->Fvb);}
STATIC VOID EFIAPI Virtual(EFI_EVENT E,VOID*C){(VOID)E;PIANO_NV_FVB_BINDING*B=C;EFI_STATUS S=PianoNvFvbConvertVirtual(&B->Fvb,gRT->ConvertPointer);if(S!=EFI_SUCCESS){B->Retained=TRUE;
#ifdef __aarch64__
 __asm__ volatile("msr daifset, #15":::"memory");
#endif
 CpuDeadLoop();}}
STATIC EFI_STATUS RuntimeRange(CONST VOID *Pointer,UINTN Bytes){
 if(!Pointer||!Bytes||(UINTN)Pointer>MAX_UINTN-Bytes)return EFI_INVALID_PARAMETER;
 UINT8 Map[32768];UINTN N=sizeof(Map),Key,Size;UINT32 Version;
 EFI_STATUS E=gBS->GetMemoryMap(&N,(VOID*)Map,&Key,&Size,&Version);if(E!=EFI_SUCCESS)return EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
 if(Size<sizeof(EFI_MEMORY_DESCRIPTOR)||N>sizeof(Map)||N%Size)return EFI_COMPROMISED_DATA;
 UINT64 At=(UINTN)Pointer,End=At+Bytes;
 while(At<End){BOOLEAN Found=FALSE;for(UINTN I=0;I<N;I+=Size){EFI_MEMORY_DESCRIPTOR*D=(VOID*)(Map+I);if(D->NumberOfPages>MAX_UINT64/4096||D->PhysicalStart>MAX_UINT64-D->NumberOfPages*4096)return EFI_COMPROMISED_DATA;
 UINT64 Limit=D->PhysicalStart+D->NumberOfPages*4096;if(At>=D->PhysicalStart&&At<Limit){if(D->Type!=EfiRuntimeServicesData||!(D->Attribute&EFI_MEMORY_RUNTIME))return EFI_ACCESS_DENIED;At=Limit<End?Limit:End;Found=TRUE;break;}}
 if(!Found)return EFI_NOT_FOUND;}return EFI_SUCCESS;
}
EFI_STATUS PianoNvFvbPublish(EFI_HANDLE Image,PIANO_NV_FVB_BINDING *B,PIANO_NV_JOURNAL *J){
 if(!Image||!B||!J||!J->Ready||J->Runtime||J->Dirty||J->Quarantined||!gBS||!gRT)return EFI_NOT_READY;
 if(B->Installed||B->Retained||B->Fvb.Signature)return EFI_ALREADY_STARTED;
 EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;EFI_STATUS S=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID**)&Loaded);
 if(S!=EFI_SUCCESS||!Loaded||Loaded->ImageCodeType!=EfiRuntimeServicesCode||Loaded->ImageDataType!=EfiRuntimeServicesData||!Loaded->ImageBase||!Loaded->ImageSize)return EFI_ACCESS_DENIED;
 UINTN Code=(UINTN)PianoNvFvbPublish,Start=(UINTN)Loaded->ImageBase;if(Start>MAX_UINTN-Loaded->ImageSize||Code<Start||Code>=Start+Loaded->ImageSize)return EFI_ACCESS_DENIED;
 S=RuntimeRange(B,sizeof(*B));if(S==EFI_SUCCESS)S=RuntimeRange(J,sizeof(*J));if(S==EFI_SUCCESS)S=RuntimeRange(J->Mirror,PIANO_NV_SNAPSHOT_BYTES);if(S!=EFI_SUCCESS)return S;
 VOID*Existing=NULL;S=gBS->LocateProtocol(&gEfiVariableArchProtocolGuid,NULL,&Existing);if(S!=EFI_NOT_FOUND)return S==EFI_SUCCESS?EFI_ALREADY_STARTED:S;
 S=PianoNvFvbInitialize(&B->Fvb,J);if(S!=EFI_SUCCESS)return S;
 B->Info=(PIANO_NV_READY_PROTOCOL){.Revision=1,.VariableBase=(UINTN)J->Mirror,.WorkingBase=(UINTN)J->Mirror+PIANO_NV_VARIABLE_BYTES,.SpareBase=(UINTN)J->Mirror+PIANO_NV_VARIABLE_BYTES+PIANO_NV_WORKING_BYTES,
 .VariableBytes=PIANO_NV_VARIABLE_BYTES,.WorkingBytes=PIANO_NV_WORKING_BYTES,.SpareBytes=PIANO_NV_SPARE_BYTES,.RecoveredSequence=J->Sequence};CopyMem(B->Info.VolumeUuid,J->Io.VolumeUuid,16);
 S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,B,&gEfiEventExitBootServicesGuid,&B->ExitEvent);
 if(S==EFI_SUCCESS&&B->ExitEvent)S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Virtual,B,&gEfiEventVirtualAddressChangeGuid,&B->VirtualEvent);else if(S==EFI_SUCCESS)S=EFI_COMPROMISED_DATA;
 if(S!=EFI_SUCCESS||!B->VirtualEvent){B->Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;}
 S=gBS->InstallMultipleProtocolInterfaces(&B->Handle,&gEfiFirmwareVolumeBlockProtocolGuid,&B->Fvb.Protocol,&mReady,&B->Info,NULL);
 if(S!=EFI_SUCCESS){B->Retained=TRUE;return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}B->Installed=TRUE;return EFI_SUCCESS;
}
