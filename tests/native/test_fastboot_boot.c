// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure actual parser, fake exact reader; no allocator, Boot Services or DMA.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoFastbootBoot.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static UINT8 file[65536];static UINTN length,reads,max_read;static EFI_STATUS read_error;
static VOID p16(UINTN O,UINT16 V){file[O]=(UINT8)V;file[O+1]=(UINT8)(V>>8);}
static VOID p32(UINTN O,UINT32 V){for(UINTN I=0;I<4;++I)file[O+I]=(UINT8)(V>>(8*I));}
static VOID p64(UINTN O,UINT64 V){for(UINTN I=0;I<8;++I)file[O+I]=(UINT8)(V>>(8*I));}
static EFI_STATUS reader(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  assert(Context==file && Range(Offset,Bytes,length) && Range(Offset,Bytes,sizeof(file)));++reads;max_read=MAX(max_read,Bytes);
  if(read_error)return read_error;
  memcpy(Buffer,file+Offset,Bytes);return EFI_SUCCESS;
}
static EFI_STATUS parse(PIANO_BOOT_IMAGE *Image) {
  PIANO_BOOT_SOURCE S={file,reader,length};reads=max_read=0;EFI_STATUS R=PianoFastbootBootParse(&S,Image);
  assert(max_read<=1660 && reads<=110);if(R!=EFI_SUCCESS){PIANO_BOOT_IMAGE Zero={0};assert(!memcmp(Image,&Zero,sizeof(Zero)));}return R;
}
static VOID pe(UINTN Base) {
  p16(Base,0x5a4d);p32(Base+60,128);p32(Base+128,0x4550);p16(Base+132,0xaa64);p16(Base+134,1);p16(Base+148,240);p16(Base+150,2);
  UINTN Opt=Base+152;p16(Opt,0x20b);p32(Opt+16,4096);p64(Opt+24,0x10000000);p32(Opt+32,4096);p32(Opt+36,512);
  p32(Opt+56,8192);p32(Opt+60,512);p16(Opt+68,10);p32(Opt+108,16);
  UINTN Sec=Base+392;memcpy(file+Sec,".text",5);p32(Sec+8,512);p32(Sec+12,4096);p32(Sec+16,512);p32(Sec+20,512);p32(Sec+36,0x60000020);
}
static VOID android(UINT32 V) {
  memset(file,0,sizeof(file));memcpy(file,"ANDROID!",8);p32(40,V);p32(8,1024);
  if(V<3){p32(16,100);p32(24,37);p32(36,4096);if(V>=1){p32(1632,20);p64(1636,16384);p32(1644,V==1?1648:1660);}if(V==2)p32(1648,21);}
  else {p32(12,100);p32(20,V==3?1580:1584);if(V==4)p32(1580,73);}
  length=V==0?16384:V==1?20480:V==2?24576:V==3?12288:12361;pe(4096);read_error=EFI_SUCCESS;
}
int main(void) {
  PIANO_BOOT_IMAGE I;PIANO_BOOT_SOURCE S={file,reader,0};
  assert(PianoFastbootBootParse(NULL,&I)==EFI_INVALID_PARAMETER && PianoFastbootBootParse(&S,NULL)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootBootParse(&S,&I)==EFI_INVALID_PARAMETER);
  for(UINT32 V=0;V<=4;++V) {
    android(V);assert(parse(&I)==EFI_SUCCESS && I.Kind==PianoBootAndroid && I.Version==V && I.PageBytes==4096);
    assert(I.Kernel.Offset==4096 && I.Kernel.Bytes==1024 && I.Ramdisk.Offset==8192 && I.Ramdisk.Bytes==100 && I.KernelIsArm64Pe && I.KernelPeStatus==EFI_SUCCESS);
    assert(I.Pe.EntryFileOffset==512 && I.Pe.Machine==0xaa64 && I.Pe.Subsystem==10);
    if(V<3)assert(I.Second.Offset==12288 && I.Second.Bytes==37);
    if(V>=1 && V<3)assert(I.RecoveryDtbo.Offset==16384 && I.RecoveryDtbo.Bytes==20);
    if(V==2)assert(I.Dtb.Offset==20480 && I.Dtb.Bytes==21);
    if(V==4)assert(I.Signature.Offset==12288 && I.Signature.Bytes==73);
    assert(!I.Trailing.Bytes);
    UINTN Full=length;for(length=0;length<MIN(Full,(UINTN)1700);++length)assert(parse(&I)!=EFI_SUCCESS);length=Full;
    --length;assert(parse(&I)==EFI_COMPROMISED_DATA);
  }
  for(UINTN Case=0;Case<14;++Case) {
    android(Case<7?2:4);
    switch(Case) {
      case 0:p32(40,5);break;case 1:p32(36,4000);break;case 2:p32(36,1024);break;case 3:p32(36,0x80000000);break;
      case 4:p32(8,0xffffffff);break;case 5:p64(1636,12288);break;case 6:p32(1644,1632);break;
      case 7:p32(20,1580);break;case 8:p32(24,1);break;case 9:p32(28,1);break;case 10:p32(32,1);break;
      case 11:p32(36,1);break;case 12:p32(1580,0xffffffff);break;case 13:p32(8,0);p32(12,0);break;
    }
    assert(parse(&I)!=EFI_SUCCESS);
  }
  android(4);p32(8,0);assert(parse(&I)==EFI_SUCCESS && !I.Kernel.Bytes && !I.KernelIsArm64Pe); // init_boot shape, not launchable.
  android(4);length=12288;p32(1580,0);p32(20,1580);
  assert(parse(&I)==EFI_SUCCESS && I.KnownV4CliHeaderQuirk && I.HeaderBytes==1584 && I.DeclaredHeaderBytes==1580);
  android(4);p16(4096+132,0x8664);assert(parse(&I)==EFI_SUCCESS && !I.KernelIsArm64Pe && I.KernelPeStatus==EFI_UNSUPPORTED);
  android(4);p32(8,0x3ffff000);p32(12,0);p32(1580,0);length=0x40000000;
  assert(parse(&I)==EFI_SUCCESS && I.Kernel.Bytes==0x3ffff000 && I.KernelIsArm64Pe && max_read<=1660);
  // One-GiB logical source tested by metadata reads, never allocated/copied.
  for(UINTN Case=0;Case<26;++Case) {
    memset(file,0,sizeof(file));length=1024;pe(0);
    switch(Case) {
      case 0:p32(60,0xffffffff);break;case 1:p32(60,1);break;case 2:p32(128,0);break;case 3:p16(132,0x8664);break;
      case 4:p16(134,0);break;case 5:p16(134,97);break;case 6:p16(148,111);break;case 7:p16(148,241);break;
      case 8:p16(150,0);break;case 9:p16(152,0x10b);break;case 10:p16(152+68,11);break;
      case 11:p32(152+32,3000);break;case 12:p32(152+36,128);break;case 13:p32(152+56,4096);break;
      case 14:p32(152+108,17);break;case 15:p32(392+20,0);break;case 16:p32(392+36,0x40000040);break;
      case 17:p64(152+24,MAX_UINT64);break;case 18:p32(152+112+32,900);p32(152+116+32,256);break;
      case 19:p32(152+112,0xffffffff);p32(152+116,8);break;
      case 20:p32(152+112,4096+500);p32(152+116,20);break;
      case 21:p32(152+112,0);p32(152+116,8);break;
      case 22:p32(144,1);break;case 23:p32(140,1020);p32(144,1);break;
      case 24:p16(392+32,1);p32(392+24,1020);break;case 25:p16(392+34,1);p32(392+28,1020);break;
    }
    assert(parse(&I)!=EFI_SUCCESS);
  }
  memset(file,0,sizeof(file));length=1024;pe(0);assert(parse(&I)==EFI_SUCCESS && I.Kind==PianoBootArm64Pe && I.Kernel.Offset==0 && I.Kernel.Bytes==1024);
  read_error=EFI_DEVICE_ERROR;assert(parse(&I)==EFI_DEVICE_ERROR);read_error=EFI_WARN_UNKNOWN_GLYPH;assert(parse(&I)==EFI_DEVICE_ERROR);read_error=EFI_SUCCESS;
  memset(file,0,sizeof(file));length=8;assert(parse(&I)==EFI_UNSUPPORTED);memcpy(file,"VNDRBOOT",8);assert(parse(&I)==EFI_UNSUPPORTED);
  puts("Pure boot parser actual source: Android v0-v4 ranges/headers/padding, init_boot distinction, raw versus wrapped ARM64 PE, machine/subsystem/section/certificate bounds, truncation/overflow/read failures, bounded reads and zero output passed.");
  return 0;
}
