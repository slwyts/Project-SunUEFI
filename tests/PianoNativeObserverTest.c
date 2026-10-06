// SPDX-License-Identifier: BSD-2-Clause-Patent
// Calls the byte-identical NativeProbe.c compiled with a host-only FFS table.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include "NativeProbeTable.h"
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_HANDLE gImageHandle=(VOID *)99;
STATIC EFI_BOOT_SERVICES Bs;
STATIC UINT32 Case,FvCalls[32],LoadCalls[32],StartCalls[32],UnloadCalls[32],HookBefore[32],HookAfter[32];
STATIC BOOLEAN Dependency;
typedef struct {CHAR8 Kind;UINT32 Index;} TRACE;
STATIC TRACE Events[256];STATIC UINT32 EventCount;
VOID PianoProbeFoundation(VOID);
VOID PianoNativeSetObserver(VOID (*Observer)(CONST CHAR8 *,BOOLEAN));
STATIC VOID Event(CHAR8 Kind,UINT32 Index){assert(EventCount<256);Events[EventCount++]=(TRACE){Kind,Index};}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){assert(L&&Fmt);}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **Out){
  assert(!Registration);*Out=NULL;
  if((G->Data1==0xbeef&&Dependency)||(Case==5&&(G->Data1==0xb0760469||G->Data1==0x54b6d3b4||G->Data1==0x4684800a))){*Out=(VOID *)1;return EFI_SUCCESS;}
  return EFI_NOT_FOUND;
}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,EFI_SECTION_TYPE T,UINTN Instance,VOID **Source,UINTN *Bytes){
  assert(T==EFI_SECTION_PE32&&!Instance&&G->Data1>0&&G->Data1<=ARRAY_SIZE(mNativeImages));UINT32 I=G->Data1-1;FvCalls[I]++;Event('F',I);
#ifndef PIANO_NATIVE_FIXTURE_CAPACITY
  if(I==2)return EFI_NOT_FOUND;
#endif
  UINT32 *P=malloc(sizeof(*P));assert(P);*P=I;*Source=P;*Bytes=sizeof(*P);return EFI_SUCCESS;
}
VOID EFIAPI FreePool(VOID *Source){assert(Source);Event('R',*(UINT32 *)Source);free(Source);}
STATIC EFI_STATUS EFIAPI Load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Handle){
  assert(!Policy&&Parent==gImageHandle&&!Path&&Source&&Bytes==4);UINT32 I=*(UINT32 *)Source;LoadCalls[I]++;Event('L',I);
#ifndef PIANO_NATIVE_FIXTURE_CAPACITY
  if(I==3)return EFI_DEVICE_ERROR;
#endif
  *Handle=(VOID *)(UINTN)(I+100);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Start(EFI_HANDLE Handle,UINTN *Bytes,CHAR16 **Data){
  assert(!Bytes&&!Data);UINT32 I=(UINT32)(UINTN)Handle-100;assert(I<ARRAY_SIZE(mNativeImages));StartCalls[I]++;Event('S',I);
#ifndef PIANO_NATIVE_FIXTURE_CAPACITY
  if(I==1&&Case!=2)Dependency=TRUE;
  if(I==4)return EFI_DEVICE_ERROR;
  if(I==5&&Case==6)return EFI_WARN_STALE_DATA;
#endif
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unload(EFI_HANDLE Handle){UINT32 I=(UINT32)(UINTN)Handle-100;UnloadCalls[I]++;Event('U',I);return EFI_SUCCESS;}
STATIC VOID Observer(CONST CHAR8 *Name,BOOLEAN Before){
  UINT32 I=0;while(I<ARRAY_SIZE(mNativeImages)&&strcmp(Name,mNativeImages[I].Name))++I;assert(I<ARRAY_SIZE(mNativeImages));
  if(Before){assert(HookBefore[I]==HookAfter[I]);HookBefore[I]++;Event('B',I);}
  else{assert(HookBefore[I]==HookAfter[I]+1);HookAfter[I]++;Event('A',I);}
}
STATIC VOID Order(UINT32 I,BOOLEAN Enabled){
  CHAR8 Sequence[32]={0};UINT32 At=0;for(UINT32 E=0;E<EventCount;++E)if(Events[E].Index==I){assert(At<31);Sequence[At++]=Events[E].Kind;}
#ifndef PIANO_NATIVE_FIXTURE_CAPACITY
  if(I==2){assert(!strcmp(Sequence,"F")&&!HookBefore[I]&&!HookAfter[I]);return;}
  if(I==3){assert(!strcmp(Sequence,"FLR")&&!HookBefore[I]&&!HookAfter[I]);return;}
  if(I==4){assert(!strcmp(Sequence,Enabled?"FLRBSAU":"FLRSU"));return;}
#endif
  assert(!strcmp(Sequence,Enabled?"FLRBSA":"FLRS"));
}
STATIC VOID Run(UINT32 N){
  Case=N;gBS=&Bs;Bs.LocateProtocol=Locate;Bs.LoadImage=Load;Bs.StartImage=Start;Bs.UnloadImage=Unload;
  BOOLEAN Enabled=N!=1&&N!=3;
  if(N!=1)PianoNativeSetObserver(Observer);
  if(N==3)PianoNativeSetObserver(NULL);
  PianoProbeFoundation();
  for(UINT32 I=0;I<ARRAY_SIZE(mNativeImages);++I){assert(FvCalls[I]<=1&&LoadCalls[I]<=1&&StartCalls[I]<=1&&UnloadCalls[I]<=1);assert(HookBefore[I]==HookAfter[I]);if(StartCalls[I])assert(HookBefore[I]==(Enabled?1U:0U));}
#ifdef PIANO_NATIVE_FIXTURE_CAPACITY
  UINT32 Starts=0,Hooks=0;for(UINT32 I=0;I<ARRAY_SIZE(mNativeImages);++I){Order(I,Enabled);Starts+=StartCalls[I];Hooks+=HookBefore[I]+HookAfter[I];}
  assert(Starts==13&&Hooks==(Enabled?26U:0U)&&5+Hooks<=32);
#else
  for(UINT32 I=1;I<=5;++I)Order(I,Enabled);
  assert(!UnloadCalls[1]&&!UnloadCalls[5]&&UnloadCalls[4]==1);
  if(N==2)assert(!FvCalls[0]&&!StartCalls[0]&&!HookBefore[0]);else{Order(0,Enabled);assert(StartCalls[0]==1);}
  assert(!FvCalls[6]&&!FvCalls[7]&&!StartCalls[6]&&!StartCalls[7]);
  if(N==5){Order(8,Enabled);Order(9,Enabled);}else assert(!FvCalls[8]&&!FvCalls[9]&&!HookBefore[8]&&!HookBefore[9]);
#endif
  // Clearing after foundation leaves no observer calls on subsequent loading.
  PianoNativeSetObserver(NULL);UINT32 Before=0;for(UINT32 I=0;I<32;++I)Before+=HookBefore[I]+HookAfter[I];
  memset(FvCalls,0,sizeof(FvCalls));memset(LoadCalls,0,sizeof(LoadCalls));memset(StartCalls,0,sizeof(StartCalls));memset(UnloadCalls,0,sizeof(UnloadCalls));EventCount=0;
  PianoProbeFoundation();UINT32 After=0;for(UINT32 I=0;I<32;++I)After+=HookBefore[I]+HookAfter[I];assert(Before==After);
}
int main(VOID){for(UINT32 I=0;I<7;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"native callback case%u failed\n",I);return 1;}}
#ifdef PIANO_NATIVE_FIXTURE_CAPACITY
  puts("Actual NativeProbe:13 product names,26 callbacks+5 outer phases fit32; clear prevents stale callbacks");
#else
  puts("Actual NativeProbe:7 fork cases, before/Start/after/Unload order, FV/Load/DEPEX/runtime misses, second pass/warning/default/clear");
#endif
  return 0;}
