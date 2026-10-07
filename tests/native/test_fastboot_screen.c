// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tests the actual capture/encoding code using an explicit GOP mock.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include "../../uefi/core/PianoFastbootScreen.c"
EFI_GUID gEfiGraphicsOutputProtocolGuid;
EFI_BOOT_SERVICES Services,*gBS=&Services;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ModeInfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static UINT8 *Staged;static UINTN StagedBytes,Live;
static EFI_STATUS BltStatus,StageStatus;
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=calloc(1,N);if(P)++Live;return P;}
VOID EFIAPI FreePool(VOID *P){assert(P && Live);--Live;free(P);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
EFI_STATUS PianoFastbootStageCopy(PIANO_FASTBOOT *State,CONST VOID *P,UINTN N){
  assert(State);if(StageStatus)return StageStatus;free(Staged);Staged=malloc(N);assert(Staged);memcpy(Staged,P,N);StagedBytes=N;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **Out){assert(G==&gEfiGraphicsOutputProtocolGuid && !Registration);*Out=&Gop;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  assert(This==&Gop && Op==EfiBltVideoToBltBuffer && Sx==0 && Sy==0 && Dx==0 && Dy==0 && Delta==W*4);
  if(BltStatus)return BltStatus;
  for(UINTN Y=0;Y<H;++Y)for(UINTN X=0;X<W;++X)Pixels[Y*W+X]=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){.Blue=(UINT8)X,.Green=(UINT8)Y,.Red=0xA5,.Reserved=0xCC};
  return EFI_SUCCESS;
}
static UINT32 Le32(UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
int main(void){
  Services.LocateProtocol=Locate;ModeInfo.HorizontalResolution=3;ModeInfo.VerticalResolution=2;
  Mode.Info=&ModeInfo;Mode.SizeOfInfo=sizeof(ModeInfo);Gop.Mode=&Mode;Gop.Blt=Blt;
  PIANO_FASTBOOT State={0};UINT32 W,H,N,C;
  assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==EFI_SUCCESS);
  assert(W==3 && H==2 && N==78 && StagedBytes==78 && C==Crc(Staged,78) && Live==0);
  assert(Staged[0]=='B' && Staged[1]=='M' && Le32(Staged+2)==78 && Le32(Staged+10)==54 && Le32(Staged+18)==3 && Le32(Staged+22)==2);
  assert(Staged[54]==0 && Staged[55]==1 && Staged[56]==0xA5 && Staged[66]==0 && Staged[67]==0 && Staged[68]==0xA5);
  assert(Staged[63]==0 && Staged[64]==0 && Staged[65]==0 && Staged[75]==0 && Staged[76]==0 && Staged[77]==0);
  BltStatus=EFI_DEVICE_ERROR;assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==EFI_DEVICE_ERROR && W==0 && H==0 && N==0 && C==0 && Live==0);
  BltStatus=0;StageStatus=EFI_OUT_OF_RESOURCES;assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==StageStatus && !W && !N && Live==0);StageStatus=0;
  ModeInfo.HorizontalResolution=0;assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==EFI_BAD_BUFFER_SIZE);
  ModeInfo.HorizontalResolution=4096;ModeInfo.VerticalResolution=4096;assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==EFI_BAD_BUFFER_SIZE);
  Mode.SizeOfInfo=0;assert(PianoFastbootCaptureScreen(&State,&W,&H,&N,&C)==EFI_COMPROMISED_DATA);
  assert(PianoFastbootCaptureScreen(NULL,&W,&H,&N,&C)==EFI_INVALID_PARAMETER);
  free(Staged);puts("Actual GOP capture: BMP bottom-up BGR/padding/CRC, failed Blt/staging, bounds and cleanup passed.");return 0;
}
