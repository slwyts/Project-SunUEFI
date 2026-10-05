// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/VariableFlashInfoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include "Protocol/PianoNvReady.h"
STATIC EFI_GUID mGuid=PIANO_NV_READY_PROTOCOL_GUID;
STATIC EFI_STATUS Info(UINT32 Region,EFI_PHYSICAL_ADDRESS *Base,UINT64 *Bytes){
 if(!Base||!Bytes)return EFI_INVALID_PARAMETER;*Base=0;*Bytes=0;
 // Standard initialization consumes this at boot. Runtime reads use the
 // standard driver's converted caches/mirror, never a Boot Services lookup.
 if(EfiAtRuntime())return EFI_UNSUPPORTED;
 PIANO_NV_READY_PROTOCOL *P=NULL;EFI_STATUS E=gBS->LocateProtocol(&mGuid,NULL,(VOID**)&P);
 if(E!=EFI_SUCCESS)return EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
 if(!P||P->Revision!=1||!P->RecoveredSequence||P->VariableBytes!=262144||P->WorkingBytes!=65536||P->SpareBytes!=262144||
    !P->VariableBase||P->WorkingBase!=P->VariableBase+P->VariableBytes||P->SpareBase!=P->WorkingBase+P->WorkingBytes)return EFI_COMPROMISED_DATA;
 if(Region==0){*Base=P->VariableBase;*Bytes=P->VariableBytes;}else if(Region==1){*Base=P->WorkingBase;*Bytes=P->WorkingBytes;}else{*Base=P->SpareBase;*Bytes=P->SpareBytes;}return EFI_SUCCESS;
}
EFI_STATUS EFIAPI GetVariableFlashNvStorageInfo(EFI_PHYSICAL_ADDRESS *B,UINT64 *N){return Info(0,B,N);}
EFI_STATUS EFIAPI GetVariableFlashFtwWorkingInfo(EFI_PHYSICAL_ADDRESS *B,UINT64 *N){return Info(1,B,N);}
EFI_STATUS EFIAPI GetVariableFlashFtwSpareInfo(EFI_PHYSICAL_ADDRESS *B,UINT64 *N){return Info(2,B,N);}
