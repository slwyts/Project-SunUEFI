// SPDX-License-Identifier: BSD-2-Clause-Patent
// Draw only into the inherited Piano framebuffer; no clocks, GPIOs or MMIO.
#include "../../components/product-support/Library/ProductBootManagerLib/ProductSplash.h"
#include "../../components/product-support/Library/ProductBootManagerLib/ProductSplashAssets.h"
#include "EarlyKeys.h"
#include "BootRequest.h"

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

STATIC VOID FlushFrame(VOID) {
#if defined(__aarch64__)
  UINT64 Sctlr,Ctr;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, ctr_el0":"=r"(Sctlr),"=r"(Ctr));
  if(Sctlr&(1ULL<<2)) {
    UINT64 Line=4ULL<<((Ctr>>16)&15),End=FRAME_BASE+(UINT64)FRAME_WIDTH*FRAME_HEIGHT*4;
    for(UINT64 Address=FRAME_BASE;Address<End;Address+=Line)
      __asm__ volatile("dc cvac, %0"::"r"(Address):"memory");
  }
  __asm__ volatile("dsb sy":::"memory");
#endif
}
STATIC int CanDraw(CONST VOID *Fdt) {
#if defined(__aarch64__)
  UINT64 El,Sctlr;__asm__ volatile("mrs %0, CurrentEL":"=r"(El));
  if(El!=4)return 0;
  __asm__ volatile("mrs %0, sctlr_el1":"=r"(Sctlr));
  if(Sctlr&1)return 0;
#endif
  return PianoEarlySplashAllowed(Fdt);
}
STATIC VOID DrawBrand(VOID) {
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};
  Info.HorizontalResolution=FRAME_WIDTH;Info.VerticalResolution=FRAME_HEIGHT;
  Info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;Info.PixelsPerScanLine=FRAME_WIDTH;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};Mode.Info=&Info;Mode.SizeOfInfo=sizeof(Info);
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={0};Gop.Mode=&Mode;Gop.Blt=Fill;
  PIANO_SPLASH_REPORT Report;
  if(PianoProductDrawSplash(&Gop,Alive,&Report)!=EFI_SUCCESS)return;
}
VOID PianoEarlyDrawSplash(CONST VOID *Fdt) {
  if(!CanDraw(Fdt))return;
  DrawBrand();FlushFrame();
}

STATIC VOID Rect(UINT32 X,UINT32 Y,UINT32 W,UINT32 H,UINT32 Rgb) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL P={(UINT8)Rgb,(UINT8)(Rgb>>8),(UINT8)(Rgb>>16),0};
  Fill(NULL,&P,EfiBltVideoFill,0,0,X,Y,W,H,0);
}
STATIC VOID Text(CONST CHAR8 *String,UINT32 X,UINT32 Y,UINT32 Scale,UINT32 Rgb) {
  for(UINT32 I=0;String[I];++I) {
    UINT8 Ch=(UINT8)String[I];CONST UINT8 *G=NULL;
    if(Ch>='a'&&Ch<='z')Ch=(UINT8)(Ch-'a'+'A');
    if(Ch>='A'&&Ch<='Z')G=mLetters[Ch-'A'];
    else if(Ch>='0'&&Ch<='9')G=mDigits[Ch-'0'];
    if(!G)continue;
    for(UINT32 Column=0;Column<5;++Column)
      for(UINT32 Row=0;Row<7;++Row)
        if(G[Column]&(1U<<Row))Rect(X+(I*6+Column)*Scale,Y+Row*Scale,Scale,Scale,Rgb);
  }
}

// Exported for a host-rendered preview of the exact framebuffer drawing code.
VOID PianoEarlyDrawChoice(unsigned Target,unsigned Seconds,int HaveKeys,int Initial) {
  if(Initial) {
    DrawBrand();
    Rect(0,1010,FRAME_WIDTH,FRAME_HEIGHT-1010,0xFFFFFF);
    Text("SELECT STARTUP",700,1050,9,0x565B63);
  }
  for(unsigned I=0;I<2;++I) {
    int Selected=(Target!=0)==(I!=0);
    UINT32 Y=1170+I*250;
    Rect(700,Y,1800,200,Selected?0x4273E2:0xE7EAF0);
    Rect(704,Y+4,1792,192,Selected?0xEEF4FF:0xF8F9FB);
    Text(I?"UEFI":"ANDROID",810,Y+55,13,Selected?0x2254B5:0x383D46);
    if(Selected)Text("SELECTED",1950,Y+68,8,0x4273E2);
  }
  Rect(690,1690,1900,130,0xFFFFFF);
  CHAR8 Count[]="AUTO START IN 3 SECONDS";Count[14]=(CHAR8)('0'+Seconds);
  Text(Count,700,1710,8,0x656B75);
  Rect(700,1805,1800,8,0xE9EDF3);
  if(Seconds)Rect(700,1805,600*Seconds,8,0x4273E2);
  Rect(0,1880,FRAME_WIDTH,256,0xFFFFFF);
  Text(HaveKeys?"VOLUME UP  ANDROID     VOLUME DOWN  UEFI":"CONTINUING SAVED STARTUP",192,1910,6,0x656B75);
  if(HaveKeys)Text("POWER  CONFIRM     THIS CHOICE IS FOR THIS BOOT ONLY",192,2010,5,0x848A94);
  FlushFrame();
}

unsigned PianoEarlyChoose(CONST VOID *Fdt,unsigned DefaultTarget) {
#if defined(__aarch64__)
  if(!CanDraw(Fdt))return DefaultTarget;
  UINT64 Frequency=PianoEarlyFrequency();
  if(Frequency<1000000 || Frequency>2000000000ULL)return DefaultTarget;
  PIANO_EARLY_KEYS Keys;
  UINT8 Stable=0,Candidate=0,Now=0;
  int HaveKeys=PianoEarlyKeysStart(Fdt,&Keys);
  if(HaveKeys && !PianoEarlyKeysSample(&Keys,&Stable))HaveKeys=0;
  Candidate=Stable;
  unsigned Selected=DefaultTarget,Seconds=3,Shown=3,Samples=0;
  PianoEarlyDrawChoice(Selected,Seconds,HaveKeys,1);
  // Count the full three seconds after the first frame has reached memory.
  UINT64 Start=PianoEarlyCounter(),Deadline=Start+Frequency*3,LastPoll=Start,PowerPressed=0;
  // ABL's power-on key may still be held. Initial held keys never select or
  // confirm. Require fresh debounced edges after the initial snapshot.
  for(;;) {
    UINT64 NowTicks=PianoEarlyCounter();
    if(NowTicks>=Deadline)break;
    Seconds=(unsigned)((Deadline-NowTicks+Frequency-1)/Frequency);
    if(Seconds!=Shown) {Shown=Seconds;PianoEarlyDrawChoice(Selected,Seconds,HaveKeys,0);}
    if(HaveKeys && NowTicks-LastPoll>=Frequency/50) {
      LastPoll=NowTicks;
      if(!PianoEarlyKeysSample(&Keys,&Now)) {
        HaveKeys=0;PianoEarlyDrawChoice(Selected,Seconds,0,0);continue;
      }
      if(Now!=Candidate) {Candidate=Now;Samples=1;continue;}
      if(Samples<2)++Samples;
      if(Samples<2 || Stable==Now)continue;
      UINT8 Pressed=Now&~Stable,Released=Stable&~Now;
      // Chords remain reserved for the OEM paths. ABL already owns power-on
      // Recovery/Fastboot detection, and we never emit a direction for a chord.
      if(!(Now&PIANO_EARLY_KEY_POWER) &&
         (Now&(PIANO_EARLY_KEY_UP|PIANO_EARLY_KEY_DOWN))!=
             (PIANO_EARLY_KEY_UP|PIANO_EARLY_KEY_DOWN)) {
        if(Pressed&PIANO_EARLY_KEY_UP)Selected=PIANO_BOOT_REQUEST_NONE;
        if(Pressed&PIANO_EARLY_KEY_DOWN)Selected=PIANO_BOOT_REQUEST_LINUX;
        if(Pressed&(PIANO_EARLY_KEY_UP|PIANO_EARLY_KEY_DOWN)) {
          Shown=Seconds=3;
          PianoEarlyDrawChoice(Selected,3,HaveKeys,0);
          Deadline=PianoEarlyCounter()+Frequency*3;
        }
      }
      if(Pressed&PIANO_EARLY_KEY_POWER)PowerPressed=NowTicks;
      if((Released&PIANO_EARLY_KEY_POWER) && PowerPressed &&
         NowTicks-PowerPressed<Frequency && !(Now&(PIANO_EARLY_KEY_UP|PIANO_EARLY_KEY_DOWN)))break;
      Stable=Now;
    }
    __asm__ volatile("yield");
  }
  // Leave a visible handoff message. No later native display driver is used
  // on Android; the original GKI sees the exact ABL register/DTB/initrd tuple.
  Rect(690,1690,1900,130,0xFFFFFF);
  Text(Selected?"STARTING UEFI":"STARTING ANDROID",700,1710,8,0x656B75);
  FlushFrame();return Selected;
#else
  (VOID)Fdt;return DefaultTarget;
#endif
}
