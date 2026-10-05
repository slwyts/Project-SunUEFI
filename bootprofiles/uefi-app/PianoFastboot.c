// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fastboot 0.4 command layer. No block, variable, flash or OEM passthrough API.
#include "PianoFastboot.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC EFI_STATUS Reply(PIANO_FASTBOOT *S, CONST CHAR8 *Text) {
  return S->Send(S->Context, Text, AsciiStrLen(Text));
}
STATIC BOOLEAN Equal(CONST CHAR8 *A, CONST CHAR8 *B) {
  return AsciiStrCmp(A,B)==0;
}
STATIC VOID Hex32(UINT32 Value, CHAR8 *Out) {
  STATIC CONST CHAR8 Digits[]="0123456789abcdef";
  for(UINTN I=0;I<8;++I)Out[I]=Digits[(Value>>((7-I)*4))&15];
  Out[8]=0;
}
VOID PianoFastbootReset(PIANO_FASTBOOT *S) {
  if(S==NULL)return;
  if(S->Upload!=NULL && !S->UploadBorrowed){ZeroMem(S->Upload,S->UploadBytes);FreePool(S->Upload);}
  S->Upload=NULL;S->UploadBytes=0;S->UploadBorrowed=FALSE;
  if(S->Download!=NULL) {
    // Received data can include private diagnostic payloads; clear on release.
    ZeroMem(S->Download,S->Expected);FreePool(S->Download);
  }
  S->Download=NULL;S->Expected=0;S->Received=0;
  S->Receiving=FALSE;S->Complete=FALSE;
  S->RebootRequested=FALSE;S->ExitRequested=FALSE;
}
EFI_STATUS PianoFastbootStageCopy(PIANO_FASTBOOT *S,CONST VOID *Data,UINTN Bytes) {
  if(S==NULL || Data==NULL || Bytes==0 || Bytes>PIANO_FASTBOOT_MAX_DOWNLOAD || S->Receiving)return EFI_INVALID_PARAMETER;
  UINT8 *Copy=AllocateZeroPool(Bytes);if(Copy==NULL)return EFI_OUT_OF_RESOURCES;
  CopyMem(Copy,Data,Bytes);
  if(S->Upload!=NULL && !S->UploadBorrowed){ZeroMem(S->Upload,S->UploadBytes);FreePool(S->Upload);}
  S->Upload=Copy;S->UploadBytes=Bytes;S->UploadBorrowed=FALSE;return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootInit(PIANO_FASTBOOT *S, VOID *Context,
                            PIANO_FB_SEND Send, PIANO_FB_LOG Log) {
  if(S==NULL || Send==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(S,sizeof(*S));S->Context=Context;S->Send=Send;S->Log=Log;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS GetVar(PIANO_FASTBOOT *S, CONST CHAR8 *Name) {
  if(Equal(Name,"version"))return Reply(S,"OKAY0.4");
  if(Equal(Name,"product"))return Reply(S,"OKAYpiano-sunuefi");
  if(Equal(Name,"serialno"))return Reply(S,"OKAYSunUEFI-piano");
  if(Equal(Name,"version-bootloader"))return Reply(S,"OKAYSunUEFI-debug-v1");
  if(Equal(Name,"is-userspace"))return Reply(S,"OKAYno");
  if(Equal(Name,"storage-policy"))return Reply(S,"OKAYno-persistent-writes");
  if(Equal(Name,"max-download-size"))return Reply(S,"OKAY0x04000000");
  if(Equal(Name,"download-size")) {
    CHAR8 Result[13]="OKAY";Hex32(S->Complete?(UINT32)S->Received:0,Result+4);
    return Reply(S,Result);
  }
  if(Equal(Name,"all")) {
    STATIC CONST CHAR8 *Rows[]={"INFOproduct:piano-sunuefi","INFOversion:0.4",
      "INFOstorage-policy:no-persistent-writes","INFOmax-download-size:0x04000000"};
    for(UINTN I=0;I<ARRAY_SIZE(Rows);++I) {
      EFI_STATUS Status=Reply(S,Rows[I]);if(EFI_ERROR(Status))return Status;
    }
    return Reply(S,"OKAY");
  }
  if(S->Query!=NULL) {
    CHAR8 Result[65]="OKAY";ZeroMem(Result+4,61);
    EFI_STATUS Status=S->Query(S->Context,Name,Result+4);
    if(!EFI_ERROR(Status))return Reply(S,Result);
    if(Status!=EFI_UNSUPPORTED)return Status;
  }
  return Reply(S,"FAILunknown variable");
}
STATIC EFI_STATUS Download(PIANO_FASTBOOT *S, CONST CHAR8 *Arg) {
  UINT32 Size=0;
  if(AsciiStrLen(Arg)!=8)return Reply(S,"FAILdownload length must be eight hex digits");
  for(UINTN I=0;I<8;++I) {
    CHAR8 C=Arg[I];UINT32 N;
    if(C>='0' && C<='9')N=C-'0';
    else if(C>='a' && C<='f')N=C-'a'+10;
    else if(C>='A' && C<='F')N=C-'A'+10;
    else return Reply(S,"FAILinvalid download length");
    Size=(Size<<4)|N;
  }
  if(Size==0 || Size>PIANO_FASTBOOT_MAX_DOWNLOAD)return Reply(S,"FAILdownload exceeds RAM limit");
  PianoFastbootReset(S);
  S->Download=AllocateZeroPool(Size);
  if(S->Download==NULL)return Reply(S,"FAILnot enough RAM");
  S->Expected=Size;
  CHAR8 Result[13]="DATA";Hex32(Size,Result+4);
  EFI_STATUS Status=Reply(S,Result);
  if(EFI_ERROR(Status)){PianoFastbootReset(S);return Status;}
  S->Receiving=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS HashDownload(PIANO_FASTBOOT *S) {
  UINT8 Hash[32];CHAR8 Line[61]="INFO";
  STATIC CONST CHAR8 Digits[]="0123456789abcdef";
  if(!S->Complete)return Reply(S,"FAILno complete RAM payload");
  if(!Sha256HashAll(S->Download,S->Received,Hash))return Reply(S,"FAILSHA256 unavailable");
  // Fastboot response packets are at most 64 bytes, including the tag.
  for(UINTN Part=0;Part<2;++Part) {
    for(UINTN I=0;I<16;++I) {
      Line[4+I*2]=Digits[Hash[Part*16+I]>>4];
      Line[5+I*2]=Digits[Hash[Part*16+I]&15];
    }
    Line[36]=0;EFI_STATUS Status=Reply(S,Line);if(EFI_ERROR(Status))return Status;
  }
  return Reply(S,"OKAY");
}
EFI_STATUS PianoFastbootPacket(PIANO_FASTBOOT *S, CONST VOID *Data, UINTN Bytes) {
  if(S==NULL || S->Send==NULL || (Data==NULL && Bytes!=0))return EFI_INVALID_PARAMETER;
  if(S->Receiving) {
    if(Bytes==0)return EFI_SUCCESS; // Ignore USB zero length packets.
    if(Bytes>S->Expected-S->Received) {
      PianoFastbootReset(S);return Reply(S,"FAILdownload overflow");
    }
    CopyMem(S->Download+S->Received,Data,Bytes);S->Received+=Bytes;
    if(S->Received==S->Expected) {
      S->Receiving=FALSE;S->Complete=TRUE;
      S->Upload=S->Download;S->UploadBytes=S->Received;S->UploadBorrowed=TRUE;
      return Reply(S,"OKAY");
    }
    return EFI_SUCCESS;
  }
  if(Bytes==0)return EFI_SUCCESS;
  if(Bytes>64)return Reply(S,"FAILcommand too long");
  CONST UINT8 *Raw=Data;
  for(UINTN I=0;I<Bytes;++I)if(Raw[I]<0x20 || Raw[I]>0x7e)
    return Reply(S,"FAILinvalid command bytes");
  CHAR8 Cmd[65];CopyMem(Cmd,Data,Bytes);Cmd[Bytes]=0;
  if(!AsciiStrnCmp(Cmd,"getvar:",7))return GetVar(S,Cmd+7);
  if(!AsciiStrnCmp(Cmd,"download:",9))return Download(S,Cmd+9);
  if(Equal(Cmd,"upload")) {
    if(S->Upload==NULL || !S->UploadBytes)return Reply(S,"FAILno staged RAM payload");
    CHAR8 Result[13]="DATA";Hex32((UINT32)S->UploadBytes,Result+4);
    EFI_STATUS Status=Reply(S,Result);if(EFI_ERROR(Status))return Status;
    Status=S->Send(S->Context,S->Upload,S->UploadBytes);if(EFI_ERROR(Status))return Status;
    return Reply(S,"OKAY");
  }
  if(Equal(Cmd,"oem sha256"))return HashDownload(S);
  if(Equal(Cmd,"oem discard")){PianoFastbootReset(S);return Reply(S,"OKAY");}
  if(Equal(Cmd,"oem status") || Equal(Cmd,"oem ramlog")) {
    if(S->Diagnostic==NULL)return Reply(S,"FAILdiagnostic service unavailable");
    return S->Diagnostic(S->Context,S,Cmd);
  }
  if(Equal(Cmd,"oem log")) {
    if(S->Log==NULL)return Reply(S,"FAILlog service unavailable");
    EFI_STATUS Status=S->Log(S->Context,S->Send);
    if(EFI_ERROR(Status))return Status;
    return Reply(S,"OKAY");
  }
  if(Equal(Cmd,"reboot")) {
    EFI_STATUS Status=Reply(S,"OKAY");
    if(!EFI_ERROR(Status))S->RebootRequested=TRUE;
    return Status;
  }
  if(Equal(Cmd,"continue")) {
    EFI_STATUS Status=Reply(S,"OKAY");
    if(!EFI_ERROR(Status))S->ExitRequested=TRUE;
    return Status;
  }
  if(Equal(Cmd,"boot"))return Reply(S,"FAILRAM boot handoff not implemented in debug v1");
  // Default deny covers flash, erase, set_active, flashing, arbitrary OEM,
  // reboot-edl, reboot-bootloader, memory poke and unknown future commands.
  return Reply(S,"FAILcommand disabled by RAM-only policy");
}
