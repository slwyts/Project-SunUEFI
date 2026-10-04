// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original DEPEX gates are honored before manually starting passive FFS images.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include "NativeProbeTable.h"

STATIC BOOLEAN Ready(CONST UINT8 *Expression,UINTN Length) {
  BOOLEAN Stack[64];UINTN Count=0,Offset=0;
  if(Length==0)return TRUE;
  while(Offset<Length) {
    UINT8 Op=Expression[Offset++];
    if(Op==2) {
      EFI_GUID Guid;VOID *Interface=NULL;
      if(Offset+16>Length || Count==64)return FALSE;
      CopyMem(&Guid,Expression+Offset,16);Offset+=16;
      Stack[Count++]=!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,&Interface));
    } else if(Op==6 || Op==7) {
      if(Count==64)return FALSE;Stack[Count++]=Op==6;
    } else if(Op==3 || Op==4) {
      if(Count<2)return FALSE;
      BOOLEAN Right=Stack[--Count];
      Stack[Count-1]=Op==3?(Stack[Count-1]&&Right):(Stack[Count-1]||Right);
    } else if(Op==5) {
      if(Count<1)return FALSE;Stack[Count-1]=!Stack[Count-1];
    } else if(Op==8) { return Count==1 && Stack[0]; }
    else return FALSE;
  }
  return FALSE;
}

VOID PianoProbeFoundation(VOID) {
  BOOLEAN Attempted[ARRAY_SIZE(mNativeImages)]={FALSE};
  DEBUG((DEBUG_WARN,"SUNUEFI_FOUNDATION_BEGIN\n"));
  for(UINTN Pass=0;Pass<ARRAY_SIZE(mNativeImages);++Pass) {
    BOOLEAN Progress=FALSE;
    for(UINTN I=0;I<ARRAY_SIZE(mNativeImages);++I) {
      CONST NATIVE_IMAGE *Image=&mNativeImages[I];
      // The touch profile explicitly starts GPI later with QUP clocks held.
      // Its TRUE DEPEX is insufficient for this post-ABL environment.
      if(AsciiStrCmp(Image->Name,"GpiDxe")==0)continue;
      if(Attempted[I] || !Ready(Image->Depex,Image->DepexBytes))continue;
      if(AsciiStrCmp(Image->Name,"UsbConfigDxe")==0) {
        // Disassembly shows runtime consumers omitted by its original DEPEX.
        // In particular HAL IOMMU is dereferenced without checking Locate's
        // result. PMIC bring-up faulted in test 22; never bypass that provider.
        STATIC EFI_GUID RuntimeDeps[]={
          {0x54B6D3B4,0x5D33,0x4F91,{0x86,0x00,0x6C,0x41,0xD5,0xDE,0xB1,0x9A}},
          {0x4684800A,0x2755,0x4EDC,{0xB4,0x43,0x7F,0x8C,0xEB,0x32,0x39,0xD3}}
        };
        BOOLEAN Available=TRUE;
        for(UINTN D=0;D<ARRAY_SIZE(RuntimeDeps);++D) {
          VOID *Interface=NULL;
          EFI_STATUS Dependency=gBS->LocateProtocol(&RuntimeDeps[D],NULL,&Interface);
          if(EFI_ERROR(Dependency)) {
            Available=FALSE;
            DEBUG((DEBUG_WARN,"SUNUEFI_USB_RUNTIME_DEPENDENCY %g %r\n",&RuntimeDeps[D],Dependency));
          }
        }
        if(!Available)continue;
      }
      // This module's native DEPEX omits its ChipInfo consumer. Test 21
      // identified that missing runtime dependency; require it explicitly.
      if(AsciiStrCmp(Image->Name,"QcomScmiDxe")==0) {
        EFI_GUID Chip={0xB0760469,0x970C,0x487A,{0xA4,0xB5,0x28,0xDB,0x7B,0x45,0xCE,0xF1}};
        VOID *Interface=NULL;
        if(EFI_ERROR(gBS->LocateProtocol(&Chip,NULL,&Interface)))continue;
      }
      Attempted[I]=TRUE;Progress=TRUE;
      VOID *Source=NULL;UINTN Bytes=0;EFI_HANDLE Handle=NULL;
      EFI_STATUS Status=GetSectionFromAnyFv((EFI_GUID *)&Image->Guid,EFI_SECTION_PE32,0,&Source,&Bytes);
      if(!EFI_ERROR(Status)) {
        Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Source,Bytes,&Handle);FreePool(Source);
        if(!EFI_ERROR(Status)) {
          DEBUG((DEBUG_WARN,"SUNUEFI_NATIVE_START %a\n",Image->Name));
          Status=gBS->StartImage(Handle,NULL,NULL);
          // A successfully started foundation driver must remain loaded.
          if(EFI_ERROR(Status))gBS->UnloadImage(Handle);
        }
      }
      DEBUG((DEBUG_WARN,"SUNUEFI_NATIVE_RESULT %a %r\n",Image->Name,Status));
    }
    if(!Progress)break;
  }
  for(UINTN I=0;I<ARRAY_SIZE(mNativeImages);++I)if(!Attempted[I]) {
    DEBUG((DEBUG_WARN,"SUNUEFI_NATIVE_DEPEX_WAIT %a\n",mNativeImages[I].Name));
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_FOUNDATION_END\n"));
}
