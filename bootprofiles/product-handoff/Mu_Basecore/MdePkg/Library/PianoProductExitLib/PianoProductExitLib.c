// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native Core client. No constructor/event/alloc; retry uses CPU reads only.
#include <Library/PianoProductExitLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
STATIC EFI_GUID mExitGuid=PIANO_PRODUCT_EXIT_PROTOCOL_GUID;
STATIC PIANO_PRODUCT_EXIT_PROTOCOL *mExit;
STATIC PIANO_PRODUCT_EXIT_PROTOCOL mFunctions;
STATIC EFI_HANDLE mImage;
STATIC CONST EFI_LOADED_IMAGE_PROTOCOL *mIdentity;
STATIC EFI_LOADED_IMAGE_PROTOCOL mLoaded;
STATIC UINT64 mEpoch;
STATIC BOOLEAN mBusy,mClean;
STATIC BOOLEAN SameImage(EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *L){
 return Image==mImage&&L==mIdentity&&L&&L->ImageBase==mLoaded.ImageBase&&L->ImageSize==mLoaded.ImageSize&&
   L->ParentHandle==mLoaded.ParentHandle&&L->ImageCodeType==mLoaded.ImageCodeType&&L->ImageDataType==mLoaded.ImageDataType&&
   L->SystemTable==mLoaded.SystemTable&&L->Revision==mLoaded.Revision;
}
STATIC VOID Stop(EFI_STATUS Status){
 if(mExit&&mFunctions.FailStop)mFunctions.FailStop(mExit,Status);
 CpuDeadLoop();while(TRUE){}
}
EFI_STATUS EFIAPI PianoProductBeforeExitBootServices(EFI_HANDLE Image,
 CONST EFI_LOADED_IMAGE_PROTOCOL *L,EFI_TPL Tpl,UINTN Key,UINTN Current,BOOLEAN Before){
 if(Tpl!=TPL_APPLICATION)return EFI_UNSUPPORTED;
 if(!Image||!L||!L->ImageBase||!L->ImageSize)return EFI_ACCESS_DENIED;
 // Reject an already stale key before any protocol call, Before notification,
 // timer change or device retirement. A standard caller can obtain a new map.
 if(Key!=Current)return EFI_INVALID_PARAMETER;
 if(mBusy)Stop(EFI_ALREADY_STARTED);
 if(mClean){
   if(!SameImage(Image,L))return EFI_ACCESS_DENIED;
   if(!mExit||CompareMem(mExit,&mFunctions,sizeof(mFunctions)))Stop(EFI_COMPROMISED_DATA);
   UINT64 Epoch=0;EFI_STATUS E=mFunctions.ObserveClean(mExit,Image,L,&Epoch);
   if(E!=EFI_SUCCESS||Epoch!=mEpoch)Stop(E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
   return EFI_SUCCESS;
 }
 if(Before)return EFI_NOT_READY; // Never discover/retire after Before attempt.
 if(!gBS||!gST||gST->BootServices!=gBS||gBS->Hdr.Signature!=EFI_BOOT_SERVICES_SIGNATURE||!gBS->LocateProtocol)return EFI_NOT_READY;
 PIANO_PRODUCT_EXIT_PROTOCOL *Found=NULL;
 EFI_STATUS E=gBS->LocateProtocol(&mExitGuid,NULL,(VOID **)&Found);
 if(E!=EFI_SUCCESS)return E==EFI_NOT_FOUND?EFI_NOT_READY:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
 if(!Found||Found->Revision!=PIANO_PRODUCT_EXIT_REVISION||!Found->BeforeExit||!Found->ObserveClean||!Found->FailStop)return EFI_COMPROMISED_DATA;
 mExit=Found;mFunctions=*Found;mBusy=TRUE;
 E=mFunctions.BeforeExit(mExit,Image,L,Key);
 // Known non-mutating refusal is permitted; a retiring provider itself must
 // fail-stop on warning/unknown/partial hardware outcomes, never resume an OS.
 if(E!=EFI_SUCCESS){if(!EFI_ERROR(E))Stop(EFI_DEVICE_ERROR);mBusy=FALSE;mExit=NULL;ZeroMem(&mFunctions,sizeof(mFunctions));return E;}
 if(CompareMem(mExit,&mFunctions,sizeof(mFunctions)))Stop(EFI_COMPROMISED_DATA);
 UINT64 Epoch=0;E=mFunctions.ObserveClean(mExit,Image,L,&Epoch);
 if(E!=EFI_SUCCESS||!Epoch)Stop(E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 mImage=Image;mIdentity=L;mLoaded=*L;mEpoch=Epoch;mClean=TRUE;mBusy=FALSE;
 return EFI_SUCCESS;
}
