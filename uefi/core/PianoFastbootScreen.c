// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFastbootScreen.h"
#include <Protocol/GraphicsOutput.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#define SCREEN_MAX_PIXELS (8U*1024U*1024U)
STATIC VOID Put32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
STATIC UINT32 Crc(CONST UINT8 *P,UINTN N){UINT32 C=MAX_UINT32;for(UINTN I=0;I<N;++I){C^=P[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xEDB88320U:0);}return ~C;}
EFI_STATUS PianoFastbootCaptureScreen(PIANO_FASTBOOT *State,UINT32 *Width,
                                     UINT32 *Height,UINT32 *Bytes,UINT32 *Crc32){
  if(State==NULL || Width==NULL || Height==NULL || Bytes==NULL || Crc32==NULL)return EFI_INVALID_PARAMETER;
  *Width=*Height=*Bytes=*Crc32=0;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  if(Gop==NULL || Gop->Blt==NULL || Gop->Mode==NULL || Gop->Mode->Info==NULL ||
     Gop->Mode->SizeOfInfo<sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION))return EFI_COMPROMISED_DATA;
  UINT32 W=Gop->Mode->Info->HorizontalResolution,H=Gop->Mode->Info->VerticalResolution;
  if(!W || !H || W>4096 || H>4096 || (UINT64)W*H>SCREEN_MAX_PIXELS)return EFI_BAD_BUFFER_SIZE;
  UINTN Pixels=(UINTN)W*H,PixelBytes=Pixels*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  UINTN Pitch=ALIGN_VALUE((UINTN)W*3,4),Total=54+Pitch*H;
  if(Total>PIANO_FASTBOOT_MAX_DOWNLOAD || Total>MAX_UINT32)return EFI_BAD_BUFFER_SIZE;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Capture=AllocateZeroPool(PixelBytes);
  if(Capture==NULL)return EFI_OUT_OF_RESOURCES;
  S=Gop->Blt(Gop,Capture,EfiBltVideoToBltBuffer,0,0,0,0,W,H,(UINTN)W*sizeof(*Capture));
  if(S!=EFI_SUCCESS){ZeroMem(Capture,PixelBytes);FreePool(Capture);return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
  UINT8 *Bmp=AllocateZeroPool(Total);
  if(Bmp==NULL){ZeroMem(Capture,PixelBytes);FreePool(Capture);return EFI_OUT_OF_RESOURCES;}
  Bmp[0]='B';Bmp[1]='M';Put32(Bmp+2,(UINT32)Total);Put32(Bmp+10,54);Put32(Bmp+14,40);
  Put32(Bmp+18,W);Put32(Bmp+22,H);Bmp[26]=1;Bmp[28]=24;Put32(Bmp+34,(UINT32)(Pitch*H));
  for(UINTN Y=0;Y<H;++Y){
    UINT8 *Row=Bmp+54+(H-1-Y)*Pitch;
    for(UINTN X=0;X<W;++X){EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P=Capture+Y*W+X;Row[X*3]=P->Blue;Row[X*3+1]=P->Green;Row[X*3+2]=P->Red;}
  }
  UINT32 Check=Crc(Bmp,Total);
  S=PianoFastbootStageCopy(State,Bmp,Total);
  ZeroMem(Bmp,Total);FreePool(Bmp);ZeroMem(Capture,PixelBytes);FreePool(Capture);
  if(S==EFI_SUCCESS){*Width=W;*Height=H;*Bytes=(UINT32)Total;*Crc32=Check;}
  return S;
}
