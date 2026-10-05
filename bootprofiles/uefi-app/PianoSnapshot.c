// SPDX-License-Identifier: BSD-2-Clause-Patent
// Downsample only the piano GOP framebuffer and append a PNG to console RAM.
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Protocol/GraphicsOutput.h>
#include <lodepng.h>
#include <stdlib.h>

STATIC EFI_EVENT mScreenshot;
STATIC CONST CHAR8 mBase64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
STATIC VOID EFIAPI Capture (EFI_EVENT Event, VOID *Context) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  unsigned char *Pixels, *Png=NULL; size_t PngBytes=0;
  UINTN Width=800, Height=534; EFI_STATUS Status;
  gBS->CloseEvent (Event); mScreenshot=NULL;
  Status=gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if (EFI_ERROR (Status) || Gop->Mode==NULL || Gop->Mode->Info==NULL) { return; }
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=Gop->Mode->Info;
  if (Gop->Mode->FrameBufferBase!=0xFC800000ULL || Info->HorizontalResolution!=3200 ||
      Info->VerticalResolution!=2136 || Info->PixelsPerScanLine!=3200 ||
      Info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor ||
      Gop->Mode->FrameBufferSize<3200ULL*2136*4) { return; }
  Pixels=malloc(Width*Height*4); if(Pixels==NULL) { return; }
  CONST volatile UINT8 *Frame=(CONST volatile UINT8 *)(UINTN)Gop->Mode->FrameBufferBase;
  for (UINTN Y=0;Y<Height;++Y) for (UINTN X=0;X<Width;++X) {
    CONST volatile UINT8 *Source=Frame+((Y*4)*3200+X*4)*4;
    UINT8 *Target=Pixels+(Y*Width+X)*4;
    Target[0]=Source[2];Target[1]=Source[1];Target[2]=Source[0];Target[3]=255;
  }
  unsigned Error=lodepng_encode32(&Png,&PngBytes,Pixels,(unsigned)Width,(unsigned)Height);
  free(Pixels);
  if(Error || PngBytes>0x80000) { if(Png)free(Png); return; }
  DEBUG((DEBUG_WARN,"SUNUEFI_PNG_BEGIN bytes=%lu width=%lu height=%lu\n",(UINT64)PngBytes,(UINT64)Width,(UINT64)Height));
  CHAR8 Line[129];UINTN Used=0;
  for (UINTN I=0;I<PngBytes;I+=3) {
    UINT32 Value=(UINT32)Png[I]<<16;
    if(I+1<PngBytes)Value|=(UINT32)Png[I+1]<<8;
    if(I+2<PngBytes)Value|=Png[I+2];
    Line[Used++]=mBase64[(Value>>18)&63];Line[Used++]=mBase64[(Value>>12)&63];
    Line[Used++]=I+1<PngBytes?mBase64[(Value>>6)&63]:'=';
    Line[Used++]=I+2<PngBytes?mBase64[Value&63]:'=';
    if(Used==128 || I+3>=PngBytes) { Line[Used]=0;DEBUG((DEBUG_WARN,"SUNUEFI_PNG_DATA %a\n",Line));Used=0; }
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_PNG_END\n"));free(Png);
}
VOID PianoScheduleSnapshot (VOID) {
#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
  // Product screenshots are requested through the resident fastboot worker at
  // application TPL. Do not run the diagnostic PNG encoder in a timer callback.
  return;
#else
  EFI_STATUS Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Capture,NULL,&mScreenshot);
  if(!EFI_ERROR(Status))gBS->SetTimer(mScreenshot,TimerRelative,15ULL*10000000ULL);
#endif
}
VOID PianoCancelSnapshot (VOID) {
  if(mScreenshot!=NULL) {
    gBS->SetTimer(mScreenshot,TimerCancel,0);
    gBS->CloseEvent(mScreenshot);
    mScreenshot=NULL;
  }
}
