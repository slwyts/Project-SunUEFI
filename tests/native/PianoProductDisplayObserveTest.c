// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual observer with bounded EFI service methods; no framebuffer/registers.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include "PianoProductDisplayObserve.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiGraphicsOutputProtocolGuid={0x12345678,0,0,{0}};
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_SYSTEM_TABLE St;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL Gop[2];
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode[2];
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info[2];
STATIC EFI_HANDLE Handles[17];
STATIC UINT32 Case,Calls,Raises,Restores,Locates,Enumerates,Interfaces,Frees,Logs;
STATIC BOOLEAN Live=TRUE,Reentered;STATIC EFI_TPL Tpl=TPL_APPLICATION;
STATIC CHAR8 Lines[1024][256];STATIC UINTN Longest;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Description){fprintf(stderr,"actual PrintLib assert %s:%lu %s\n",File,(unsigned long)Line,Description);abort();}
UINT64 EFIAPI DivU64x32Remainder(UINT64 Dividend,UINT32 Divisor,UINT32 *Remainder){if(Remainder)*Remainder=(UINT32)(Dividend%Divisor);return Dividend/Divisor;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINT16 EFIAPI ReadUnaligned16(CONST VOID *P){UINT16 V;memcpy(&V,P,sizeof(V));return V;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,sizeof(V));return V;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Fmt,...){
  assert(Live&&Level&&strstr(Fmt,"PIANO_GOP_")&&Logs<1024);
  VA_LIST Args;VA_START(Args,Fmt);UINTN N=AsciiVSPrint(Lines[Logs],sizeof(Lines[Logs]),Fmt,Args);VA_END(Args);
  assert(N&&N<=180&&Lines[Logs][N-1]=='\n'&&Lines[Logs][N]==0);
  if(N>Longest)Longest=N;Logs++;
}
STATIC VOID Lost(VOID){Live=FALSE;gBS=(VOID *)1;gST=(VOID *)1;}
STATIC BOOLEAN EFIAPI Alive(VOID){
  if(Case==29&&!Reentered){Reentered=TRUE;assert(PianoProductDisplayObserve("nested",Alive)==EFI_ALREADY_STARTED);}
  return Live;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){assert(Live&&New==TPL_HIGH_LEVEL);Calls++;Raises++;if(Case==19)Lost();return Tpl;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){assert(Live&&Old==Tpl);Calls++;Restores++;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  assert(Live&&Guid==&gEfiGraphicsOutputProtocolGuid&&!Registration);Calls++;Locates++;
  *Out=Case==31?(VOID *)3:&Gop[0];
  if(Case==18)Lost();if(Case==1)return EFI_NOT_FOUND;
  return Case==3?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Enumerate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Out){
  assert(Live&&Type==ByProtocol&&Guid==&gEfiGraphicsOutputProtocolGuid&&!Key);Calls++;Enumerates++;
  *Count=Case==2?17:Case==33?0:2;*Out=Handles;
  if(Case==21)Lost();
  if(Case==1||Case==7){*Out=NULL;*Count=0;return EFI_NOT_FOUND;}
  if(Case==4||Case==6){if(Case==6)*Out=NULL;return EFI_WARN_STALE_DATA;}
  if(Case==5)return EFI_DEVICE_ERROR;
  if(Case==34)*Out=NULL;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Interface(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out){
  assert(Live&&Guid==&gEfiGraphicsOutputProtocolGuid);Calls++;
  if(Handle==St.ConsoleOutHandle){*Out=&Gop[1];return Case==10?EFI_WARN_STALE_DATA:Case==11?EFI_NOT_FOUND:EFI_SUCCESS;}
  assert(Handle==(VOID *)10||Handle==(VOID *)11);Interfaces++;*Out=Handle==(VOID *)10?&Gop[0]:&Gop[1];
  if(Case==22)Lost();
  if(Case==8)return EFI_DEVICE_ERROR;
  if(Case==9)return EFI_WARN_STALE_DATA;
  if(Case==35)*Out=NULL;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Free(VOID *P){assert(Live&&P==Handles);Calls++;Frees++;if(Case==23)Lost();return Case==16?EFI_WARN_STALE_DATA:Case==17?EFI_DEVICE_ERROR:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI NoBlt(EFI_GRAPHICS_OUTPUT_PROTOCOL *P,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,EFI_GRAPHICS_OUTPUT_BLT_OPERATION O,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN D){
  (VOID)P;(VOID)B;(VOID)O;(VOID)Sx;(VOID)Sy;(VOID)Dx;(VOID)Dy;(VOID)W;(VOID)H;(VOID)D;assert(!"observer must never invoke GOP");return EFI_UNSUPPORTED;}
STATIC VOID Setup(UINT32 N){
  Case=N;memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));gBS=&Bs;gST=&St;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.LocateHandleBuffer=Enumerate;Bs.HandleProtocol=Interface;Bs.FreePool=Free;
  St.BootServices=&Bs;St.ConsoleOutHandle=(VOID *)5;
  for(UINT32 I=0;I<17;++I)Handles[I]=(VOID *)(UINTN)(10+I);
  for(UINT32 I=0;I<2;++I){
    Info[I]=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=3200,.VerticalResolution=2136,.PixelFormat=I?PixelBltOnly:PixelBlueGreenRedReserved8BitPerColor,.PixelsPerScanLine=3200};
    Mode[I]=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Mode=0,.Info=&Info[I],.SizeOfInfo=sizeof(Info[I]),.FrameBufferBase=I?0:0xfc800000,.FrameBufferSize=I?0:3200*2136*4};
    Gop[I]=(EFI_GRAPHICS_OUTPUT_PROTOCOL){.Blt=NoBlt,.Mode=&Mode[I]};
  }
  if(Case==12)Gop[0].Mode=NULL;
  if(Case==13)Mode[0].Info=NULL;
  if(Case==14)Mode[0].SizeOfInfo=1;
  if(Case==15)Info[0].PixelFormat=PixelFormatMax;
  if(Case==20)Tpl=TPL_CALLBACK;
  if(Case==30)gBS=NULL;
  if(Case==32)St.ConsoleOutHandle=NULL;
}
STATIC VOID Run(UINT32 N){
  Setup(N);EFI_STATUS S;CONST PIANO_PRODUCT_DISPLAY_REPORT *R;
  if(N==36){Lost();assert(PianoProductDisplayReemit(Alive)==EFI_ABORTED&&!Calls&&!Logs&&PianoProductDisplayRetained());return;}
  if(N==27){assert(PianoProductDisplayObserve("abcdefghijklmnopqrstuvwxyz123456789",Alive)==EFI_INVALID_PARAMETER&&!Calls);return;}
  if(N==28){assert(PianoProductDisplayObserve(NULL,Alive)==EFI_INVALID_PARAMETER&&PianoProductDisplayObserve("",Alive)==EFI_INVALID_PARAMETER&&PianoProductDisplayObserve("phase",NULL)==EFI_INVALID_PARAMETER&&!Calls);return;}
  if(N==26){for(UINT32 I=0;I<8;++I)assert(PianoProductDisplayObserve("phase",Alive)==EFI_SUCCESS);UINT32 C=Calls;assert(PianoProductDisplayObserve("overflow",Alive)==EFI_OUT_OF_RESOURCES&&Calls==C);assert(PianoProductDisplayGetReport()->Count==8&&Frees==8);return;}
  if(N==37){
    assert(PianoProductDisplayObserve("1234567890123456789012345678901",Alive)==EFI_SUCCESS);
    // Extend a host fixture's captured CPU report to full-width diagnostic
    // values. Reemit must format these snapshots without provider access.
    PIANO_PRODUCT_DISPLAY_REPORT *Worst=(VOID *)PianoProductDisplayGetReport();
    PIANO_PRODUCT_DISPLAY_SNAPSHOT *W=&Worst->Snapshot[0];
    W->Status=W->PreferredStatus=W->ConOutStatus=W->EnumerationStatus=W->FreeStatus=EFI_INCOMPATIBLE_VERSION;
    W->PreferredInterface=W->ConOutInterface=W->HandleCount=MAX_UINTN;
    for(UINT32 I=0;I<2;++I){PIANO_PRODUCT_DISPLAY_GOP *G=&W->Gop[I];
      G->Handle=G->Interface=G->BltPc=G->ModePointer=G->InfoPointer=MAX_UINTN;
      G->ModeStatus=G->InterfaceStatus=EFI_INCOMPATIBLE_VERSION;
      G->Mode=G->MaxMode=G->Width=G->Height=G->PixelFormat=G->PixelsPerScanLine=MAX_UINT32;
      G->FrameBufferBase=G->FrameBufferSize=MAX_UINT64;G->Preferred=G->ConOut=TRUE;
    }
    UINT32 C=Calls,L=Logs;gBS=(VOID *)1;gST=(VOID *)1;
    assert(PianoProductDisplayReemit(Alive)==EFI_SUCCESS&&Calls==C&&Logs==L+8);
    assert(strstr(Lines[L],"observation_only=1\r\n")&&strstr(Lines[L+1],"recorded=2\r\n"));
    assert(strstr(Lines[L+2],"info_ptr=FFFFFFFFFFFFFFFF\r\n"));
    assert(strstr(Lines[L+3],"stride=4294967295\r\n"));
    assert(strstr(Lines[L+4],"bytes=FFFFFFFFFFFFFFFF preferred=1 conout=1 observation_only=1\r\n"));
    CHAR8 Old[256];PIANO_PRODUCT_DISPLAY_GOP *G=&W->Gop[0];
    UINTN OldBytes=AsciiSPrint(Old,sizeof(Old),
      "PIANO_GOP_INSTANCE phase=%a index=%u handle=%lx interface=%lx blt_pc=%lx mode_ptr=%lx info_ptr=%lx mode_status=%r interface_status=%r mode=%u max=%u width=%u height=%u format=%u stride=%u base=%lx bytes=%lx preferred=%u conout=%u observation_only=1\n",
      W->Phase,0,(UINT64)G->Handle,(UINT64)G->Interface,(UINT64)G->BltPc,(UINT64)G->ModePointer,(UINT64)G->InfoPointer,
      G->ModeStatus,G->InterfaceStatus,G->Mode,G->MaxMode,G->Width,G->Height,G->PixelFormat,G->PixelsPerScanLine,
      G->FrameBufferBase,G->FrameBufferSize,G->Preferred,G->ConOut);
    assert(OldBytes==255&&Old[OldBytes-1]!='\n'&&!strstr(Old,"preferred=1 conout=1"));
    printf("Actual AsciiVSPrint(256): full-width observer max line=%lu bytes, complete CRLF\n",(unsigned long)Longest);return;
  }
  S=PianoProductDisplayObserve("after-usb",Alive);R=PianoProductDisplayGetReport();assert(R->Revision==1&&R->Count==1);
  CONST PIANO_PRODUCT_DISPLAY_SNAPSHOT *P=&R->Snapshot[0];assert(!strcmp(P->Phase,"after-usb"));assert(S==P->Status);
  if(N==0||N==24||N==25||N==29||N==31||N==32){
    assert(S==EFI_SUCCESS&&!R->Retained&&P->HandleCount==2&&P->RecordedCount==2&&Frees==1&&Interfaces==2);
    assert(P->Gop[0].FrameBufferBase==0xfc800000&&P->Gop[0].FrameBufferSize==3200*2136*4&&P->Gop[0].PixelsPerScanLine==3200);
    assert(P->Gop[0].Width==3200&&P->Gop[0].Height==2136&&P->Gop[0].BltPc==(UINTN)NoBlt);
    assert(P->Gop[0].Preferred==(N!=31)&&P->Gop[1].ConOut==(N!=32));
    assert(P->Gop[1].ModeStatus==EFI_SUCCESS&&P->Gop[1].PixelFormat==PixelBltOnly&&!P->Gop[1].FrameBufferBase);
  }
  if(N==1||N==7)assert(S==EFI_NOT_FOUND&&!R->Retained&&!Frees&&!Interfaces);
  if(N==2)assert(S==EFI_BAD_BUFFER_SIZE&&P->HandleCount==17&&!P->RecordedCount&&!Interfaces&&Frees==1&&!R->Retained);
  if(N==3||N==4||N==6||N==9||N==10||N==16)assert(S==EFI_DEVICE_ERROR&&R->Retained);
  if(N==5||N==17)assert(S==EFI_DEVICE_ERROR&&R->Retained);
  if(N==3)assert(!Enumerates&&!Interfaces&&!Frees);
  if(N==4||N==5||N==6)assert(!Interfaces&&!Frees);
  if(N==9)assert(Interfaces==1&&!Frees);
  if(N==10)assert(!Enumerates&&!Frees);
  if(N==8)assert(S==EFI_DEVICE_ERROR&&!R->Retained&&Interfaces==2&&Frees==1);
  if(N==11)assert(S==EFI_SUCCESS&&!R->Retained&&P->ConOutStatus==EFI_NOT_FOUND);
  if(N>=12&&N<=15)assert(S==EFI_COMPROMISED_DATA&&!R->Retained&&P->Gop[0].ModeStatus==EFI_COMPROMISED_DATA&&Frees==1);
  if(N==16||N==17)assert(Frees==1&&Interfaces==2);
  if(N==4||N==5||N==9||N==16||N==17||N==21||N==22||N==23)assert(R->RetainedHandleBuffer==(UINTN)Handles);
  if(N==18||N==19||N==21||N==22||N==23){assert(S==EFI_ABORTED&&R->ServicesLost&&R->Retained);UINT32 C=Calls;assert(PianoProductDisplayObserve("retry",Alive)==EFI_NOT_READY&&Calls==C);assert(PianoProductDisplayReemit(Alive)==EFI_ABORTED&&Calls==C);}
  if(N==18)assert(Locates==1&&!Enumerates&&!Frees);
  if(N==19)assert(Raises==1&&!Restores&&!Locates);
  if(N==21)assert(Enumerates==1&&!Frees&&!Interfaces);
  if(N==22)assert(Interfaces==1&&!Frees);
  if(N==23)assert(Frees==1);
  if(N==20)assert(S==EFI_UNSUPPORTED&&Raises==1&&Restores==1&&!Locates&&!R->Retained);
  if(N==24){UINT32 C=Calls,L=Logs;memset(Gop,0xa5,sizeof(Gop));gBS=(VOID *)1;gST=(VOID *)1;assert(PianoProductDisplayReemit(Alive)==EFI_SUCCESS&&Calls==C&&Logs==L+8);}
  if(N==25){UINT32 L=Logs;Lost();assert(PianoProductDisplayReemit(Alive)==EFI_ABORTED&&Logs==L);assert(PianoProductDisplayRetained());}
  if(N==30)assert(S==EFI_NOT_READY&&!Calls);
  if(N==33)assert(S==EFI_COMPROMISED_DATA&&!R->Retained&&Frees==1&&!Interfaces);
  if(N==34)assert(S==EFI_COMPROMISED_DATA&&R->Retained&&!Frees&&!Interfaces);
  if(N==35)assert(S==EFI_COMPROMISED_DATA&&!R->Retained&&Frees==1&&Interfaces==2);
  if(R->Retained){UINT32 C=Calls;assert(PianoProductDisplayObserve("retry",Alive)==EFI_NOT_READY&&Calls==C);}
}
int main(VOID){
  for(UINT32 I=0;I<38;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);fflush(stdout);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"display observer case %u failed\n",I);return 1;}}
  puts("Actual GOP observer: 38 fork cases, actual AsciiVSPrint capped at256 bytes, metadata/warnings/EBS/opaque replay; no framebuffer or MMIO");return 0;
}
