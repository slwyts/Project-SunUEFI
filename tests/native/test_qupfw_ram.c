// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the real narrow firmware-read interface; no device access.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoQupFwRam.c"
EFI_BOOT_SERVICES *gBS;
STATIC EFI_MEMORY_REGION_DESCRIPTOR Regions[3];
VOID EFIAPI GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **Map,UINT8 *Count) {*Map=Regions;*Count=3;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) {return strcmp(A,B);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN Bytes) {return memcpy(A,B,Bytes);}
int main(void) {
  CHAR8 Names[3][16]={"qupfw","qupfw_a","userdata"};VOID *Handle=NULL;
  UINT8 InfoBytes[96];memset(InfoBytes,0xA5,sizeof(InfoBytes));
  UINT8 *Out=malloc(0x20000);mImage=malloc(0x20000);assert(Out && mImage);mImageBytes=0x20000;
  for(UINTN I=0;I<mImageBytes;++I)mImage[I]=(UINT8)(I*7);
  strcpy(Regions[0].Name,"DXE_Heap");Regions[0].Address=(UINTN)Out;Regions[0].Length=0x20000;
  strcpy(Regions[1].Name,"UEFI_Stack");
  UINTN Low=(UINTN)&Handle;UINTN High=Low+sizeof(Handle);
  if((UINTN)InfoBytes<Low)Low=(UINTN)InfoBytes;
  if((UINTN)InfoBytes+sizeof(InfoBytes)>High)High=(UINTN)InfoBytes+sizeof(InfoBytes);
  Regions[1].Address=Low;Regions[1].Length=High-Low;
  strcpy(Regions[2].Name,"DXE_Heap");Regions[2].Address=(UINTN)Names;Regions[2].Length=sizeof(Names);
  assert(Open(0,Names[0],&Handle)==0 && Handle==&mToken);
  assert(Open(0,Names[1],&Handle)==0);
  assert(Open(0,Names[2],&Handle)!=0 && Open(1,Names[0],&Handle)!=0);
  assert(Open(0,(CONST CHAR8 *)1,&Handle)!=0 && Open(0,Names[0],(VOID **)1)!=0);
  assert(Info(Handle,InfoBytes)==0);UINT16 Blocks,BlockSize;
  memcpy(&Blocks,InfoBytes+0x38,2);memcpy(&BlockSize,InfoBytes+0x3A,2);
  assert(Blocks==256 && BlockSize==512);
  for(UINTN I=0;I<sizeof(InfoBytes);++I)if(I<0x38 || I>=0x3C)assert(InfoBytes[I]==0xA5);
  assert(Read(Handle,0,0,256,Out,0x20000)==0 && memcmp(Out,mImage,0x20000)==0);
  assert(Read(Handle,255,0,1,Out,512)==0 && memcmp(Out,mImage+255*512,512)==0);
  memset(Out,0x5A,0x20000);
  assert(Read(Handle,256,0,1,Out,512)!=0);
  assert(Read(Handle,0,1,1,Out,512)!=0);
  assert(Read(Handle,0,0,257,Out,257*512)!=0);
  assert(Read(Handle,0,0,1,Out,1)!=0);
  assert(Read((VOID *)1,0,0,1,Out,512)!=0);
  assert(Read(Handle,0,0,1,(VOID *)1,512)!=0);
  for(UINTN I=0;I<0x20000;++I)assert(Out[I]==0x5A);
  puts("Only QUP firmware names accepted; exact info fields, bounded RAM reads and rejection without mutation passed.");
  free(Out);free(mImage);return 0;
}
