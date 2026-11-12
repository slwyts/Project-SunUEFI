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
#include "PianoNativeImages.h"
#if defined(PIANO_PRODUCT_DISPLAY_TEST) && PIANO_PRODUCT_DISPLAY_TEST >= 2
#include <Protocol/LoadedImage.h>
#include "PianoProductBootLog.h"
typedef UINTN (EFIAPI *CLOCK_TRACE_CALL)(UINTN,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN);
STATIC UINTN mClockTraceBase;
STATIC CONST struct {UINT32 Site,Callee;CONST CHAR8 *Before,*After;BOOLEAN DisplayOnly;} mClockTraceSites[]={
  {0x8c6c,0x86f0,"BEFORE RAIL","AFTER RAIL",FALSE},
  {0x8c94,0xbabc,"BEFORE NPA","AFTER NPA",FALSE},
  {0x90b4,0x4e70,"BEFORE DISP MAP","AFTER DISP MAP",TRUE},
  {0x90d0,0xa0f4,"BEFORE DISP SRC","AFTER DISP SRC",TRUE},
  {0xc784,0xc540,"BEFORE CESTA","AFTER CESTA",FALSE},
  {0x3ff0,0x4604,"BEFORE CPU","AFTER CPU",FALSE},
  {0x1dc4,0x4080,"BEFORE MODULES","AFTER MODULES",FALSE},
  {0x9274,0x8a20,"BEFORE CLOCK CORE","AFTER CLOCK CORE",FALSE}
};
STATIC UINTN ClockTraceInvoke(UINTN I,UINTN A,UINTN B,UINTN C,UINTN D,UINTN E,UINTN F,UINTN G,UINTN H){
  BOOLEAN Show=!mClockTraceSites[I].DisplayOnly||A==mClockTraceBase+0x28548;
  if(Show)(VOID)PianoProductDisplayCheckpoint(mClockTraceSites[I].Before,(UINT32)(10+I*2));
  CLOCK_TRACE_CALL Original=(CLOCK_TRACE_CALL)(mClockTraceBase+mClockTraceSites[I].Callee);
  UINTN Result=Original(A,B,C,D,E,F,G,H);
  if(Show){
    DEBUG((DEBUG_WARN,"PIANO_CLOCK_INIT_CALL site=%x callee=%x result=%lx\n",
      mClockTraceSites[I].Site,mClockTraceSites[I].Callee,Result));
    (VOID)PianoProductDisplayCheckpoint(mClockTraceSites[I].After,(UINT32)(11+I*2));
  }
  return Result;
}
#define CLOCK_TRACE_WRAPPER(N) STATIC UINTN EFIAPI ClockTrace##N(UINTN A,UINTN B,UINTN C,UINTN D,UINTN E,UINTN F,UINTN G,UINTN H){return ClockTraceInvoke(N,A,B,C,D,E,F,G,H);}
CLOCK_TRACE_WRAPPER(0) CLOCK_TRACE_WRAPPER(1) CLOCK_TRACE_WRAPPER(2)
CLOCK_TRACE_WRAPPER(3) CLOCK_TRACE_WRAPPER(4) CLOCK_TRACE_WRAPPER(5)
CLOCK_TRACE_WRAPPER(6) CLOCK_TRACE_WRAPPER(7)
STATIC CLOCK_TRACE_CALL mClockTraceCallbacks[]={ClockTrace0,ClockTrace1,ClockTrace2,ClockTrace3,ClockTrace4,ClockTrace5,ClockTrace6,ClockTrace7};
STATIC UINT32 ClockTraceBranch(UINT32 Site,UINT32 Target){
  return 0x94000000U|((UINT32)(((INT32)Target-(INT32)Site)/4)&0x03ffffffU);
}
STATIC EFI_STATUS ClockTracePrepare(VOID *Source,UINTN Bytes){
  // Diagnostic-only thunks occupy captured, zero-filled executable padding.
  // Patch the owned file buffer before LoadImage, never live driver text or
  // hardware. Every original call target and the entire padding are checked.
  if(!Source||Bytes!=0x44000)return EFI_COMPROMISED_DATA;
  UINT8 *Raw=Source;
  for(UINTN I=0;I<ARRAY_SIZE(mClockTraceSites);++I){
    UINT32 Word;CopyMem(&Word,Raw+mClockTraceSites[I].Site,4);
    if(I==4&&Word==0x52800000U)continue; // Product intentionally defers this pass.
    if(Word!=ClockTraceBranch(mClockTraceSites[I].Site,mClockTraceSites[I].Callee))return EFI_COMPROMISED_DATA;
  }
  UINTN StubEnd=0x27800+ARRAY_SIZE(mClockTraceSites)*16;
  if(StubEnd>0x28000)return EFI_COMPROMISED_DATA;
  for(UINTN I=0x27800;I<StubEnd;++I)if(Raw[I])return EFI_COMPROMISED_DATA;
  // No relocation may reinterpret our absolute callback literals.
  for(UINTN At=0x41000;At<Bytes;){
    UINT32 Page,Length;CopyMem(&Page,Raw+At,4);CopyMem(&Length,Raw+At+4,4);
    if(!Length)break;
    if(Length<8||(Length&3)||Length>Bytes-At)return EFI_COMPROMISED_DATA;
    for(UINTN P=At+8;P<At+Length;P+=2){
      UINT16 Relocation;CopyMem(&Relocation,Raw+P,2);
      UINT32 Target=Page+(Relocation&0xfff);
      if((Relocation>>12)&&Target>=0x27800&&Target<StubEnd)return EFI_COMPROMISED_DATA;
    }
    At+=Length;
  }
  for(UINTN I=0;I<ARRAY_SIZE(mClockTraceSites);++I){
    UINT32 Stub=0x27800+(UINT32)I*16;
    if(I==4){UINT32 Word;CopyMem(&Word,Raw+mClockTraceSites[I].Site,4);if(Word==0x52800000U)continue;}
    UINT32 Instructions[]={0x58000050,0xd61f0200}; // LDR x16,+8; BR x16
    UINT64 Callback=(UINT64)(UINTN)mClockTraceCallbacks[I];
    CopyMem(Raw+Stub,Instructions,8);CopyMem(Raw+Stub+8,&Callback,8);
    UINT32 Branch=ClockTraceBranch(mClockTraceSites[I].Site,Stub);
    CopyMem(Raw+mClockTraceSites[I].Site,&Branch,4);
  }
  return EFI_SUCCESS;
}
#endif
STATIC EFI_HANDLE mNativeLoaded[ARRAY_SIZE(mNativeImages)];
STATIC BOOLEAN mFoundationAttempted,mFoundationRunning;
STATIC VOID (*mNativeObserver)(CONST CHAR8 *,BOOLEAN);
VOID PianoNativeSetObserver(VOID (*Observer)(CONST CHAR8 *,BOOLEAN)) {
  mNativeObserver=Observer;
}
EFI_STATUS PianoNativeGetLoadedImage(CONST EFI_GUID *FileGuid,EFI_HANDLE *Handle) {
  if(FileGuid==NULL||Handle==NULL)return EFI_INVALID_PARAMETER;
  *Handle=NULL;
  if(!mFoundationAttempted||mFoundationRunning)return EFI_NOT_READY;
  for(UINTN I=0;I<ARRAY_SIZE(mNativeImages);++I) {
    if(CompareGuid(FileGuid,&mNativeImages[I].Guid)) {
      if(!mNativeLoaded[I])return EFI_NOT_FOUND;
      *Handle=mNativeLoaded[I];return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

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
  if(mFoundationAttempted)return;
  mFoundationAttempted=mFoundationRunning=TRUE;
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
#if defined(PIANO_PRODUCT_DISPLAY_TEST) && PIANO_PRODUCT_DISPLAY_TEST >= 2
        if(AsciiStrCmp(Image->Name,"ClockDxe")==0)Status=ClockTracePrepare(Source,Bytes);
        if(EFI_ERROR(Status)){FreePool(Source);DEBUG((DEBUG_ERROR,"PIANO_CLOCK_TRACE_PREPARE status=%r\n",Status));continue;}
#endif
        Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Source,Bytes,&Handle);FreePool(Source);
        if(!EFI_ERROR(Status)) {
#if defined(PIANO_PRODUCT_DISPLAY_TEST) && PIANO_PRODUCT_DISPLAY_TEST >= 2
          if(AsciiStrCmp(Image->Name,"ClockDxe")==0){
            EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
            Status=gBS->HandleProtocol(Handle,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
            if(EFI_ERROR(Status)||!Loaded||!Loaded->ImageBase||Loaded->ImageSize!=0x44000){gBS->UnloadImage(Handle);continue;}
            mClockTraceBase=(UINTN)Loaded->ImageBase;
          }
#endif
          DEBUG((DEBUG_WARN,"SUNUEFI_NATIVE_START %a\n",Image->Name));
          if(mNativeObserver)mNativeObserver(Image->Name,TRUE);
          Status=gBS->StartImage(Handle,NULL,NULL);
          if(mNativeObserver)mNativeObserver(Image->Name,FALSE);
          // A successfully started foundation driver must remain loaded.
          if(EFI_ERROR(Status))gBS->UnloadImage(Handle);
          else mNativeLoaded[I]=Handle;
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
  mFoundationRunning=FALSE;
}
