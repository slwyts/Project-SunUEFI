// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include <Uefi.h>
STATIC UINT32 PcdWidth=3200,PcdHeight=2136,PcdDepth=32;
#define _PCD_VALUE_PcdFrameBufferWidth PcdWidth
#define _PCD_VALUE_PcdFrameBufferHeight PcdHeight
#define _PCD_VALUE_PcdFrameBufferColorDepth PcdDepth
#include "../../uefi/components/product-support/Drivers/PianoGopDxe/SimpleFb.c"

EFI_BOOT_SERVICES *gBS;EFI_GUID gEfiGraphicsOutputProtocolGuid={0},gEfiDevicePathProtocolGuid={0};
STATIC UINT64 RegionBase=0xFC800000,RegionBytes=0x2B00000;
EFI_STATUS EFIAPI LocateMemoryRegionByName(CHAR8 *Name,EFI_MEMORY_REGION_DESCRIPTOR *D){assert(!strcmp(Name,"Display_Reserved"));memset(D,0,sizeof(*D));D->Address=RegionBase;D->Length=RegionBytes;return EFI_SUCCESS;}
STATIC EFI_TPL Tpl=TPL_APPLICATION;STATIC UINTN Cleans,Logs;
STATIC VOID *CleanAt[32];STATIC UINTN CleanBytes[32];
VOID *EFIAPI AllocatePool(UINTN N){return calloc(1,N);}VOID EFIAPI FreePool(VOID *P){free(P);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI SetMem32(VOID *D,UINTN N,UINT32 V){assert(!(N%4));for(UINTN I=0;I<N/4;++I)((UINT32 *)D)[I]=V;return D;}
VOID *EFIAPI SetMem64(VOID *D,UINTN N,UINT64 V){assert(!(N%8));for(UINTN I=0;I<N/8;++I)((UINT64 *)D)[I]=V;return D;}
INTN EFIAPI HighBitSet32(UINT32 V){if(!V)return -1;INTN N=0;while(V>>=1)++N;return N;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%lu %s\n",F,(unsigned long)L,D);abort();}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;if(strstr(Format,"SUNUEFI_GOP_BLT_ERROR"))++Logs;}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address,UINTN Bytes){
  assert(Tpl==TPL_NOTIFY);assert((UINTN)Address>=mFrameBase&&Bytes&&Bytes<=mFrameBytes&&
    (UINTN)Address-mFrameBase<=mFrameBytes-Bytes);assert(Cleans<32);
  // Simulate line rounding too: these tests use an aligned, whole-line allocation.
  assert(((UINTN)Address&~63ULL)>=mFrameBase&&(((UINTN)Address+Bytes+63)&~63ULL)<=mFrameBase+mFrameBytes);
  CleanAt[Cleans]=Address;CleanBytes[Cleans++]=Bytes;return Address;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){EFI_TPL Old=Tpl;assert(New>=Old);Tpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
STATIC VOID Seed(UINT8 *Allocation,UINT32 *Pixels){memset(Allocation,0xa7,384);for(UINT32 I=0;I<64;++I)Pixels[I]=0xff000000|(I+1);Cleans=0;}
STATIC VOID Canary(UINT8 *Allocation){for(UINTN I=0;I<64;++I)assert(Allocation[I]==0xa7&&Allocation[320+I]==0xa7);}
int main(void){
  EFI_PHYSICAL_ADDRESS Address;UINT32 W,H,Depth;
  PcdWidth=0;assert(GetFrameBufferInfos(&Address,&W,&H,&Depth)==EFI_UNSUPPORTED);
  PcdWidth=640;PcdHeight=0;assert(GetFrameBufferInfos(&Address,&W,&H,&Depth)==EFI_UNSUPPORTED);
  PcdHeight=475;RegionBytes=640*475*4-1;assert(GetFrameBufferInfos(&Address,&W,&H,&Depth)==EFI_UNSUPPORTED);
  RegionBytes=640*475*4;RegionBase=MAX_UINTN-RegionBytes+1;assert(GetFrameBufferInfos(&Address,&W,&H,&Depth)==EFI_UNSUPPORTED);
  RegionBase=MAX_UINTN-RegionBytes;assert(GetFrameBufferInfos(&Address,&W,&H,&Depth)==EFI_SUCCESS);
  EFI_BOOT_SERVICES Bs={.RaiseTPL=Raise,.RestoreTPL=Restore};gBS=&Bs;
  UINT8 *Allocation=aligned_alloc(64,384);assert(Allocation);UINT32 *Pixels=(UINT32 *)(Allocation+64);
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};Mode.Info=&Info;mFrameBuffer.Mode=&Mode;
  assert(SetFrameBufferModeDetails((UINTN)Pixels,8,8,32)==EFI_SUCCESS);
  assert(CreateFrameBufferConfig((UINTN)Pixels)==EFI_SUCCESS);
  Seed(Allocation,Pixels);
  // Concrete selected-library bug: valid downward rectangle writes row8.
  assert(FrameBufferBlt(FrameBufferConfiguration,NULL,EfiBltVideoToVideo,0,0,0,1,8,7,0)==EFI_SUCCESS);
  assert(Allocation[320]!=0xa7); // same allocated canary memory; no UB test required.
  Seed(Allocation,Pixels);UINT32 Original[64];memcpy(Original,Pixels,sizeof(Original));
  assert(FrameBufferGopBlt(&mFrameBuffer,NULL,EfiBltVideoToVideo,0,0,0,1,8,7,0)==EFI_SUCCESS);
  for(UINTN Y=1;Y<8;++Y)assert(!memcmp(Pixels+Y*8,Original+(Y-1)*8,32));
  assert(Cleans==7&&CleanAt[0]==Pixels+56&&CleanBytes[0]==32);Canary(Allocation);
  Seed(Allocation,Pixels);memcpy(Original,Pixels,sizeof(Original));
  assert(FrameBufferGopBlt(&mFrameBuffer,NULL,EfiBltVideoToVideo,0,1,0,0,8,7,0)==EFI_SUCCESS);
  for(UINTN Y=0;Y<7;++Y)assert(!memcmp(Pixels+Y*8,Original+(Y+1)*8,32));Canary(Allocation);
  Seed(Allocation,Pixels);memcpy(Original,Pixels,sizeof(Original));
  assert(FrameBufferGopBlt(&mFrameBuffer,NULL,EfiBltVideoToVideo,0,2,2,2,6,1,0)==EFI_SUCCESS);
  assert(!memcmp(Pixels+18,Original+16,24)&&Cleans==1);Canary(Allocation);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={0x33,0x22,0x11,0};Cleans=0;
  assert(FrameBufferGopBlt(&mFrameBuffer,&Color,EfiBltVideoFill,0,0,0,0,8,8,0)==EFI_SUCCESS);
  assert(Cleans==1&&CleanAt[0]==Pixels&&CleanBytes[0]==256);
  for(UINTN I=0;I<64;++I)assert(Pixels[I]==0x00112233);Canary(Allocation);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Input[6];for(UINTN I=0;I<6;++I)Input[I]=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){(UINT8)I,2,3,4};Cleans=0;
  assert(FrameBufferGopBlt(&mFrameBuffer,Input,EfiBltBufferToVideo,0,0,2,3,3,2,0)==EFI_SUCCESS);
  assert(Cleans==2&&CleanAt[0]==Pixels+26&&CleanAt[1]==Pixels+34&&CleanBytes[0]==12);Canary(Allocation);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Out[6];Cleans=0;
  assert(FrameBufferGopBlt(&mFrameBuffer,Out,EfiBltVideoToBltBuffer,2,3,0,0,3,2,0)==EFI_SUCCESS);
  assert(!Cleans&&!memcmp(Out,Input,sizeof(Input)));
  UINT32 Snapshot[64];memcpy(Snapshot,Pixels,sizeof(Snapshot));
  for(UINTN I=0;I<12;++I)assert(FrameBufferGopBlt(&mFrameBuffer,&Color,EfiBltVideoFill,0,0,MAX_UINTN,0,2,1,0)==EFI_INVALID_PARAMETER);
  assert(Logs==8&&mBltErrors==8&&!Cleans&&!memcmp(Snapshot,Pixels,sizeof(Snapshot)));Canary(Allocation);
  assert(FrameBufferGopBlt(&mFrameBuffer,&Color,EfiBltBufferToVideo,0,0,7,7,2,1,0)==EFI_INVALID_PARAMETER&&!Cleans);
  Mode.FrameBufferBase+=64;assert(FrameBufferGopBlt(&mFrameBuffer,&Color,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_COMPROMISED_DATA&&!Cleans);Mode.FrameBufferBase-=64;
  VOID *Configuration=FrameBufferConfiguration;FrameBufferConfiguration=NULL;
  assert(FrameBufferGopBlt(&mFrameBuffer,&Color,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);
  free(Configuration);free(mRowScratch);free(Allocation);puts("actual GOP and selected Blt library bounds/cache ranges/overlap/canaries passed");return 0;
}
