// SPDX-License-Identifier: BSD-2-Clause-Patent
// Draw only into the inherited Piano framebuffer; no clocks, GPIOs or MMIO.
#include "../../components/product-support/Library/ProductBootManagerLib/ProductSplash.h"

int PianoEarlySplashAllowed(const void *Fdt);

// The freestanding renderer may lower aggregate initialization/copies to these
// routines. Volatile byte loops prevent recursive compiler substitutions.
void *memset(void *Target,int Value,unsigned long Bytes) {
  volatile unsigned char *P=Target;
  for(unsigned long I=0;I<Bytes;++I)P[I]=(unsigned char)Value;
  return Target;
}
void *memcpy(void *Target,const void *Source,unsigned long Bytes) {
  volatile unsigned char *To=Target;const unsigned char *From=Source;
  for(unsigned long I=0;I<Bytes;++I)To[I]=From[I];
  return Target;
}

#define FRAME_BASE 0xFC800000ULL
#define FRAME_WIDTH 3200U
#define FRAME_HEIGHT 2136U

STATIC BOOLEAN EFIAPI Alive(VOID) {return TRUE;}
STATIC EFI_STATUS EFIAPI Fill(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,UINTN SourceX,UINTN SourceY,
  UINTN X,UINTN Y,UINTN Width,UINTN Height,UINTN Delta) {
  (VOID)Gop;
  if(!Pixel || Operation!=EfiBltVideoFill || SourceX || SourceY || Delta ||
     !Width || !Height || X>=FRAME_WIDTH || Y>=FRAME_HEIGHT ||
     Width>FRAME_WIDTH-X || Height>FRAME_HEIGHT-Y)return EFI_INVALID_PARAMETER;
  UINT32 Color=(UINT32)Pixel->Blue|((UINT32)Pixel->Green<<8)|((UINT32)Pixel->Red<<16);
  volatile UINT32 *Frame=(volatile UINT32 *)(UINTN)FRAME_BASE;
  for(UINTN Row=Y;Row<Y+Height;++Row)
    for(UINTN Column=X;Column<X+Width;++Column)Frame[Row*FRAME_WIDTH+Column]=Color;
  return EFI_SUCCESS;
}

VOID PianoEarlyDrawSplash(CONST VOID *Fdt) {
  UINT64 El;__asm__ volatile("mrs %0, CurrentEL":"=r"(El));
  if(El!=4)return;
  if(!PianoEarlySplashAllowed(Fdt))return;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};
  Info.HorizontalResolution=FRAME_WIDTH;Info.VerticalResolution=FRAME_HEIGHT;
  Info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;Info.PixelsPerScanLine=FRAME_WIDTH;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};Mode.Info=&Info;Mode.SizeOfInfo=sizeof(Info);
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={0};Gop.Mode=&Mode;Gop.Blt=Fill;
  PIANO_SPLASH_REPORT Report;
  if(PianoProductDrawSplash(&Gop,Alive,&Report)!=EFI_SUCCESS)return;
  // ABL should enter with D-cache disabled. Also cover a cache-enabled handoff
  // without changing cache/MMU policy or invalidating any other memory.
  UINT64 Sctlr,Ctr;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, ctr_el0":"=r"(Sctlr),"=r"(Ctr));
  if(Sctlr&(1ULL<<2)) {
    UINT64 Line=4ULL<<((Ctr>>16)&15),End=FRAME_BASE+(UINT64)FRAME_WIDTH*FRAME_HEIGHT*4;
    for(UINT64 Address=FRAME_BASE;Address<End;Address+=Line)
      __asm__ volatile("dc cvac, %0"::"r"(Address):"memory");
  }
  __asm__ volatile("dsb sy":::"memory");
}
