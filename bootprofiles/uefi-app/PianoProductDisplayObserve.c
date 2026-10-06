// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductDisplayObserve.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
STATIC PIANO_PRODUCT_DISPLAY_REPORT mReport={.Revision=PIANO_DISPLAY_OBSERVE_REVISION};
STATIC BOOLEAN mBusy;
/* A failed/warning free has unknown ownership. Keep the pointer opaque forever;
 * neither retries nor diagnostic reemission dereference/free it again. */
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN AliveNow(PIANO_PRODUCT_DISPLAY_ALIVE Alive,PIANO_PRODUCT_DISPLAY_SNAPSHOT *S){
  if(mReport.ServicesLost || Alive==NULL || !Alive()){
    mReport.ServicesLost=TRUE;mReport.Retained=TRUE;
    if(S!=NULL){S->ServicesLost=TRUE;S->Retained=TRUE;S->Status=EFI_ABORTED;}
    return FALSE;
  }
  return TRUE;
}
STATIC VOID Retain(PIANO_PRODUCT_DISPLAY_SNAPSHOT *S,EFI_HANDLE *Handles){
  mReport.Retained=TRUE;S->Retained=TRUE;mReport.RetainedHandleBuffer=(UINTN)Handles;
}
STATIC EFI_STATUS Application(PIANO_PRODUCT_DISPLAY_ALIVE Alive,PIANO_PRODUCT_DISPLAY_SNAPSHOT *S){
  if(!AliveNow(Alive,S))return EFI_ABORTED;
  if(gBS==NULL || gST==NULL || gST->BootServices!=gBS ||
     gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL)return EFI_NOT_READY;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if(!AliveNow(Alive,S))return EFI_ABORTED;
  gBS->RestoreTPL(Tpl);
  if(!AliveNow(Alive,S))return EFI_ABORTED;
  return Tpl==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC VOID Emit(CONST PIANO_PRODUCT_DISPLAY_SNAPSHOT *S){
  // SerialPort DebugLib has a 256-byte formatting buffer. Keep complete lines
  // comfortably below it, including a 31-byte phase and full-width metadata.
  DEBUG((DEBUG_WARN,"PIANO_GOP_OBSERVE phase=%a status=%r preferred=%lx conout=%lx retained=%u lost=%u observation_only=1\n",
    S->Phase,S->Status,(UINT64)S->PreferredInterface,(UINT64)S->ConOutInterface,S->Retained,S->ServicesLost));
  DEBUG((DEBUG_WARN,"PIANO_GOP_STATUS locate=%r conout=%r enum=%r free=%r count=%lu recorded=%lu\n",
    S->PreferredStatus,S->ConOutStatus,S->EnumerationStatus,S->FreeStatus,(UINT64)S->HandleCount,(UINT64)S->RecordedCount));
  for(UINTN I=0;I<S->RecordedCount;++I){CONST PIANO_PRODUCT_DISPLAY_GOP *G=&S->Gop[I];
    DEBUG((DEBUG_WARN,"PIANO_GOP_ID phase=%a i=%u handle=%lx iface=%lx blt=%lx mode_ptr=%lx info_ptr=%lx\n",
      S->Phase,(UINT32)I,(UINT64)G->Handle,(UINT64)G->Interface,(UINT64)G->BltPc,
      (UINT64)G->ModePointer,(UINT64)G->InfoPointer));
    DEBUG((DEBUG_WARN,"PIANO_GOP_MODE i=%u mode_s=%r iface_s=%r mode=%u/%u wh=%u/%u fmt=%u stride=%u\n",
      (UINT32)I,G->ModeStatus,G->InterfaceStatus,
      G->Mode,G->MaxMode,G->Width,G->Height,G->PixelFormat,G->PixelsPerScanLine));
    DEBUG((DEBUG_WARN,"PIANO_GOP_FB i=%u base=%lx bytes=%lx preferred=%u conout=%u observation_only=1\n",
      (UINT32)I,G->FrameBufferBase,G->FrameBufferSize,G->Preferred,G->ConOut));
  }
}
EFI_STATUS PianoProductDisplayObserve(CONST CHAR8 *Phase,PIANO_PRODUCT_DISPLAY_ALIVE Alive){
  if(Phase==NULL || Alive==NULL)return EFI_INVALID_PARAMETER;
  UINTN Length=0;while(Length<PIANO_DISPLAY_OBSERVE_PHASE_BYTES && Phase[Length])++Length;
  if(!Length || Length==PIANO_DISPLAY_OBSERVE_PHASE_BYTES)return EFI_INVALID_PARAMETER;
  if(mBusy)return EFI_ALREADY_STARTED;
  if(mReport.Retained || mReport.ServicesLost)return EFI_NOT_READY;
  if(mReport.Count==PIANO_DISPLAY_OBSERVE_PHASES)return EFI_OUT_OF_RESOURCES;
  mBusy=TRUE;
  PIANO_PRODUCT_DISPLAY_SNAPSHOT *S=&mReport.Snapshot[mReport.Count++];
  ZeroMem(S,sizeof(*S));CopyMem(S->Phase,Phase,Length);
  S->PreferredStatus=S->ConOutStatus=S->EnumerationStatus=S->FreeStatus=EFI_NOT_STARTED;
  S->Status=Application(Alive,S);
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  if(S->Status!=EFI_SUCCESS)goto Done;
  if(gBS->LocateProtocol==NULL || gBS->HandleProtocol==NULL ||
     gBS->LocateHandleBuffer==NULL || gBS->FreePool==NULL){S->Status=EFI_NOT_READY;goto Done;}
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Preferred=NULL,*ConOut=NULL;
  if(!AliveNow(Alive,S))goto Done;
  S->PreferredStatus=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Preferred);
  if(!AliveNow(Alive,S))goto Done;
  if(S->PreferredStatus==EFI_SUCCESS)S->PreferredInterface=(UINTN)Preferred;
  else if(!EFI_ERROR(S->PreferredStatus)){Retain(S,NULL);S->Status=EFI_DEVICE_ERROR;goto Done;}
  if(gST->ConsoleOutHandle!=NULL){
    if(!AliveNow(Alive,S))goto Done;
    S->ConOutStatus=gBS->HandleProtocol(gST->ConsoleOutHandle,&gEfiGraphicsOutputProtocolGuid,(VOID **)&ConOut);
    if(!AliveNow(Alive,S))goto Done;
    if(S->ConOutStatus==EFI_SUCCESS)S->ConOutInterface=(UINTN)ConOut;
    else if(!EFI_ERROR(S->ConOutStatus)){Retain(S,NULL);S->Status=EFI_DEVICE_ERROR;goto Done;}
  }else S->ConOutStatus=EFI_NOT_FOUND;
  if(!AliveNow(Alive,S))goto Done;
  S->EnumerationStatus=gBS->LocateHandleBuffer(ByProtocol,&gEfiGraphicsOutputProtocolGuid,NULL,&Count,&Handles);
  S->HandleCount=Count;
  if(!AliveNow(Alive,S)){mReport.RetainedHandleBuffer=(UINTN)Handles;goto Done;}
  if(S->EnumerationStatus!=EFI_SUCCESS){
    S->Status=Exact(S->EnumerationStatus);
    if(Handles!=NULL || !EFI_ERROR(S->EnumerationStatus))Retain(S,Handles);
    goto Done;
  }
  if(Count==0 || Handles==NULL || Count>PIANO_DISPLAY_OBSERVE_GOPS){
    S->Status=Count>PIANO_DISPLAY_OBSERVE_GOPS?EFI_BAD_BUFFER_SIZE:EFI_COMPROMISED_DATA;
    if(Handles==NULL){Retain(S,NULL);goto Done;}
    goto Free;
  }
  S->Status=EFI_SUCCESS;
  for(UINTN I=0;I<Count;++I){
    PIANO_PRODUCT_DISPLAY_GOP *G=&S->Gop[S->RecordedCount++];
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Interface=NULL;
    G->Handle=(UINTN)Handles[I];G->ModeStatus=EFI_NOT_STARTED;
    if(!AliveNow(Alive,S)){mReport.RetainedHandleBuffer=(UINTN)Handles;goto Done;}
    G->InterfaceStatus=gBS->HandleProtocol(Handles[I],&gEfiGraphicsOutputProtocolGuid,(VOID **)&Interface);
    if(!AliveNow(Alive,S)){mReport.RetainedHandleBuffer=(UINTN)Handles;goto Done;}
    if(G->InterfaceStatus!=EFI_SUCCESS){
      if(!EFI_ERROR(G->InterfaceStatus)){Retain(S,Handles);S->Status=EFI_DEVICE_ERROR;goto Done;}
      if(S->Status==EFI_SUCCESS)S->Status=G->InterfaceStatus;
      continue;
    }
    G->Interface=(UINTN)Interface;G->Preferred=G->Interface==S->PreferredInterface;
    G->ConOut=G->Interface==S->ConOutInterface;
    if(Interface==NULL || Interface->Mode==NULL){G->ModeStatus=EFI_COMPROMISED_DATA;goto BadMode;}
    G->BltPc=(UINTN)Interface->Blt;
    CONST EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode=Interface->Mode;
    G->ModePointer=(UINTN)Mode;G->InfoPointer=(UINTN)Mode->Info;
    G->Mode=Mode->Mode;G->MaxMode=Mode->MaxMode;
    G->FrameBufferBase=Mode->FrameBufferBase;G->FrameBufferSize=Mode->FrameBufferSize;
    if(Mode->Info==NULL || Mode->SizeOfInfo<sizeof(*Mode->Info)){
      G->ModeStatus=EFI_COMPROMISED_DATA;goto BadMode;}
    G->Width=Mode->Info->HorizontalResolution;G->Height=Mode->Info->VerticalResolution;
    G->PixelFormat=(UINT32)Mode->Info->PixelFormat;G->PixelsPerScanLine=Mode->Info->PixelsPerScanLine;
    /* PixelBltOnly/base0 is legitimate for a console splitter. This does not
     * grant scanout/DMA ownership or require a mapped framebuffer. */
    G->ModeStatus=G->PixelFormat<PixelFormatMax?EFI_SUCCESS:EFI_COMPROMISED_DATA;
BadMode:
    if(G->ModeStatus!=EFI_SUCCESS && S->Status==EFI_SUCCESS)S->Status=G->ModeStatus;
  }
Free:
  if(!AliveNow(Alive,S)){mReport.RetainedHandleBuffer=(UINTN)Handles;goto Done;}
  S->FreeStatus=gBS->FreePool(Handles);
  if(!AliveNow(Alive,S)){mReport.RetainedHandleBuffer=(UINTN)Handles;goto Done;}
  if(S->FreeStatus!=EFI_SUCCESS){Retain(S,Handles);S->Status=Exact(S->FreeStatus);}
Done:
  // Do not touch BS/provider data again. On a live failure this RAM record can
  // still be exported; after EBS even diagnostic callbacks must not run.
  if(!mReport.ServicesLost && AliveNow(Alive,S))Emit(S);
  mBusy=FALSE;return S->Status;
}
EFI_STATUS PianoProductDisplayReemit(PIANO_PRODUCT_DISPLAY_ALIVE Alive){
  if(Alive==NULL)return EFI_INVALID_PARAMETER;
  if(mBusy)return EFI_ALREADY_STARTED;
  mBusy=TRUE;
  if(!AliveNow(Alive,NULL)){mBusy=FALSE;return EFI_ABORTED;}
  for(UINTN I=0;I<mReport.Count;++I){
    if(!AliveNow(Alive,NULL)){mBusy=FALSE;return EFI_ABORTED;}
    Emit(&mReport.Snapshot[I]);
  }
  mBusy=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoProductDisplayRetained(VOID){return mReport.Retained;}
CONST PIANO_PRODUCT_DISPLAY_REPORT *PianoProductDisplayGetReport(VOID){return &mReport;}
