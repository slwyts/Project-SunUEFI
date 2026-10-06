// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fastboot 0.4 command layer. No block, variable, flash or OEM passthrough API.
#include "PianoFastboot.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/MemoryAllocationLib.h>
#if PIANO_USB_RAM_BOOT
#include "PianoFastbootBoot.h"
#endif

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
  if(S->BootTransferFrozen || S->BootPreparing)return; // Retained/validated source must not be destroyed by reset.
  if(S->Upload!=NULL && !S->UploadBorrowed){ZeroMem(S->Upload,S->UploadBytes);FreePool(S->Upload);}
  S->Upload=NULL;S->UploadBytes=0;S->UploadBorrowed=FALSE;
  if(S->Download!=NULL) {
    // Received data can include private diagnostic payloads; clear on release.
    ZeroMem(S->Download,S->Expected);FreePool(S->Download);
  }
  S->Download=NULL;S->Expected=0;S->Received=0;
  S->Receiving=FALSE;S->Complete=FALSE;
  S->RebootRequested=FALSE;S->ExitRequested=FALSE;
  S->BootPending=FALSE;S->BootValidatedDownload=NULL;S->BootValidatedBytes=0;
  ZeroMem(&S->BootView,sizeof(S->BootView));ZeroMem(&S->BootProof,sizeof(S->BootProof));
}
EFI_STATUS PianoFastbootStageCopy(PIANO_FASTBOOT *S,CONST VOID *Data,UINTN Bytes) {
  if(S==NULL || Data==NULL || Bytes==0 || Bytes>PIANO_FASTBOOT_MAX_DOWNLOAD || S->Receiving)return EFI_INVALID_PARAMETER;
  if(S->BootPreparing || S->BootPending || S->BootTransferFrozen)return EFI_NOT_READY;
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
EFI_STATUS PianoFastbootSetStorage(PIANO_FASTBOOT *S,CONST PIANO_FB_STORAGE *Storage) {
  if(S==NULL)return EFI_INVALID_PARAMETER;
  if(Storage==NULL){ZeroMem(&S->Storage,sizeof(S->Storage));return EFI_SUCCESS;}
  if(Storage->Ready==NULL || Storage->Info==NULL || Storage->ReadBlocks==NULL)return EFI_INVALID_PARAMETER;
  S->Storage=*Storage;return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootSetBoot(PIANO_FASTBOOT *S,CONST PIANO_FB_BOOT *Boot) {
  if(S==NULL)return EFI_INVALID_PARAMETER;
  if(S->BootPreparing || S->BootPending || S->BootTransferFrozen)return EFI_NOT_READY;
  if(Boot!=NULL && (!Boot->MaxImageBytes || Boot->Ready==NULL || Boot->Validate==NULL || Boot->TakeAfterAck==NULL))return EFI_INVALID_PARAMETER;
#if !PIANO_USB_RAM_BOOT
  if(Boot!=NULL)return EFI_UNSUPPORTED;
#endif
  if(Boot!=NULL)S->Boot=*Boot;else ZeroMem(&S->Boot,sizeof(S->Boot));return EFI_SUCCESS;
}
#if PIANO_USB_RAM_BOOT
STATIC BOOLEAN BootDownloadReady(CONST PIANO_FASTBOOT *S) {
  return S->Download!=NULL && S->Complete && !S->Receiving && !S->RebootRequested && !S->ExitRequested && S->Expected &&
    S->Expected<=PIANO_FASTBOOT_MAX_DOWNLOAD && S->Received==S->Expected && S->UploadBorrowed && S->Upload==S->Download && S->UploadBytes==S->Received;
}
STATIC EFI_STATUS BootRead(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  PIANO_FASTBOOT *S=Context;
  if(!BootDownloadReady(S) || Offset>S->Received || Bytes>S->Received-Offset)return EFI_COMPROMISED_DATA;
  CopyMem(Buffer,S->Download+(UINTN)Offset,Bytes);return EFI_SUCCESS;
}
STATIC EFI_STATUS BootCommand(PIANO_FASTBOOT *S) {
  if(S->Boot.Ready==NULL || S->Boot.Validate==NULL || S->Boot.TakeAfterAck==NULL || !S->Boot.MaxImageBytes)return Reply(S,"FAILRAM boot backend unavailable");
  if(!BootDownloadReady(S))return Reply(S,"FAILno exclusive complete RAM payload");
  S->BootPreparing=TRUE;
  UINT8 *Original=S->Download;UINTN Bytes=S->Received;
  EFI_STATUS Status=S->Boot.Ready(S->Boot.Context);CONST CHAR8 *Failure="FAILRAM boot backend not ready";
  PIANO_BOOT_IMAGE Image;
  if(Status!=EFI_SUCCESS)goto Rejected;
  PIANO_BOOT_SOURCE Source={S,BootRead,Bytes};Status=PianoFastbootBootParse(&Source,&Image);
  Failure="FAILunsupported RAM boot image";if(Status!=EFI_SUCCESS)goto Rejected;
  BOOLEAN Raw=S->Boot.AllowRawLinux && Image.Kind==PianoBootAndroid && Image.Version==2 &&
    Image.Kernel.Bytes>=64 && Image.Ramdisk.Bytes && Image.Dtb.Bytes>=40 &&
    !Image.Second.Bytes && !Image.RecoveryDtbo.Bytes && !Image.Signature.Bytes && !Image.Trailing.Bytes;
  if(!Raw && Image.Kind==PianoBootAndroid && (!Image.KernelIsArm64Pe || Image.Ramdisk.Bytes || Image.Second.Bytes || Image.RecoveryDtbo.Bytes || Image.Dtb.Bytes ||
    (Image.KnownV4CliHeaderQuirk && !S->Boot.AllowKnownV4CliHeaderQuirk)))goto Rejected;
  if(Image.Kind!=PianoBootAndroid && Image.Kind!=PianoBootArm64Pe)goto Rejected;
  Failure="FAILRAM boot exceeds image budget";
  if(!Image.Kernel.Bytes || Image.Pe.ImageBytes>S->Boot.MaxImageBytes || Image.Kernel.Bytes>PIANO_FASTBOOT_MAX_DOWNLOAD)goto Rejected;
  S->BootView=(PIANO_FB_BOOT_VIEW){Image.Kernel.Offset,Image.Kernel.Bytes,Image.Pe.ImageBytes,Image.Kind==PianoBootAndroid,Image.KnownV4CliHeaderQuirk};
  S->BootValidatedDownload=Original;S->BootValidatedBytes=Bytes;
  Status=S->Boot.Validate(S->Boot.Context,S,&S->BootView);Failure="FAILRAM boot policy rejected";
  if(Status!=EFI_SUCCESS)goto Rejected;
  if(!BootDownloadReady(S) || S->Download!=Original || S->Received!=Bytes){S->BootTransferFrozen=TRUE;S->BootPreparing=FALSE;return EFI_COMPROMISED_DATA;}
  S->BootPreparing=FALSE;Status=Reply(S,"OKAY");
  if(Status==EFI_SUCCESS)S->BootPending=TRUE;
  else {S->BootValidatedDownload=NULL;S->BootValidatedBytes=0;ZeroMem(&S->BootView,sizeof(S->BootView));}
  return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
Rejected:
  if(!BootDownloadReady(S) || S->Download!=Original || S->Received!=Bytes){S->BootTransferFrozen=TRUE;S->BootPreparing=FALSE;return EFI_COMPROMISED_DATA;}
  S->BootPreparing=FALSE;S->BootValidatedDownload=NULL;S->BootValidatedBytes=0;ZeroMem(&S->BootView,sizeof(S->BootView));return Reply(S,Failure);
}
#endif
STATIC BOOLEAN StorageReady(PIANO_FASTBOOT *S) {
  return S->Storage.Ready!=NULL && S->Storage.Info!=NULL && S->Storage.ReadBlocks!=NULL && S->Storage.Ready(S->Storage.Context)==EFI_SUCCESS;
}
STATIC BOOLEAN PartitionName(CONST CHAR8 *Name) {
  UINTN N=0;
  for(;Name[N];++N) {
    CHAR8 C=Name[N];
    if(N>=PIANO_FASTBOOT_PARTITION_NAME || !((C>='a' && C<='z') || (C>='A' && C<='Z') ||
      (C>='0' && C<='9') || C=='_' || C=='-' || C=='.'))return FALSE;
  }
  return N!=0;
}
STATIC EFI_STATUS PartitionInfo(PIANO_FASTBOOT *S,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info) {
  if(!PartitionName(Name))return EFI_INVALID_PARAMETER;
  ZeroMem(Info,sizeof(*Info));EFI_STATUS Status=S->Storage.Info(S->Storage.Context,Name,Info);
  if(Status!=EFI_SUCCESS)return Status;
  // Never infer an alias, range, permission or live partition from a name.
  if(Info->Name[PIANO_FASTBOOT_PARTITION_NAME]!=0 || AsciiStrCmp(Name,Info->Name) || Info->Token==NULL || Info->ReadOnly!=TRUE ||
     !Info->Bytes || Info->BlockSize<512 || Info->BlockSize>4096 || (Info->BlockSize&(Info->BlockSize-1)) || Info->Bytes%Info->BlockSize)
    return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
STATIC VOID Hex64(UINT64 Value,CHAR8 *Out) {
  STATIC CONST CHAR8 Digits[]="0123456789abcdef";
  for(UINTN I=0;I<16;++I)Out[I]=Digits[(Value>>((15-I)*4))&15];
  Out[16]=0;
}
STATIC EFI_STATUS GetVar(PIANO_FASTBOOT *S, CONST CHAR8 *Name) {
  if(Equal(Name,"max-fetch-size"))return Reply(S,StorageReady(S)?"OKAY0x00010000":"FAILstorage backend not ready");
  if(!AsciiStrnCmp(Name,"partition-size:",15)) {
    if(!StorageReady(S))return Reply(S,"FAILstorage backend not ready");
    PIANO_FB_PARTITION_INFO Info;EFI_STATUS Status=PartitionInfo(S,Name+15,&Info);
    if(Status!=EFI_SUCCESS)return Reply(S,Status==EFI_NOT_FOUND?"FAILunknown partition":"FAILinvalid partition information");
    CHAR8 Result[23]="OKAY0x";Hex64(Info.Bytes,Result+6);return Reply(S,Result);
  }
  if(Equal(Name,"version"))return Reply(S,"OKAY0.4");
  if(Equal(Name,"product"))return Reply(S,"OKAYpiano-sunuefi");
  if(Equal(Name,"serialno"))return Reply(S,"OKAYSunUEFI-piano");
  if(Equal(Name,"version-bootloader"))return Reply(S,"OKAYSunUEFI-debug-v1");
  if(Equal(Name,"is-userspace"))return Reply(S,"OKAYno");
  if(Equal(Name,"storage-policy"))return Reply(S,"OKAYno-persistent-writes");
  if(Equal(Name,"max-download-size"))return Reply(S,"OKAY0x04000000");
  if(Equal(Name,"SunUEFI:ram-boot")) {
#if PIANO_USB_RAM_BOOT
    return Reply(S,S->Boot.Ready!=NULL && S->Boot.Validate!=NULL && S->Boot.TakeAfterAck!=NULL &&
      S->Boot.MaxImageBytes!=0 && S->Boot.Ready(S->Boot.Context)==EFI_SUCCESS?"OKAYenabled":"OKAYdisabled");
#else
    return Reply(S,"OKAYdisabled");
#endif
  }
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
STATIC BOOLEAN ParseFetchHex(CONST CHAR8 *Text,UINT64 *Value) {
  if(Text[0]!='0' || Text[1]!='x')return FALSE;
  UINTN N=0;UINT64 V=0;
  for(Text+=2;*Text;++Text) {
    UINT8 Digit;CHAR8 C=*Text;
    if(C>='0' && C<='9')Digit=(UINT8)(C-'0');
    else if(C>='a' && C<='f')Digit=(UINT8)(C-'a'+10);
    else if(C>='A' && C<='F')Digit=(UINT8)(C-'A'+10);
    else return FALSE;
    if(++N>16)return FALSE;
    V=(V<<4)|Digit;
  }
  if(!N)return FALSE;
  *Value=V;return TRUE;
}
STATIC EFI_STATUS Fetch(PIANO_FASTBOOT *S,CHAR8 *Arg) {
  if(!StorageReady(S))return Reply(S,"FAILstorage backend not ready");
  CHAR8 *OffsetArg=NULL,*SizeArg=NULL;
  for(CHAR8 *P=Arg;*P;++P)if(*P==':') {
    if(SizeArg!=NULL)return Reply(S,"FAILinvalid fetch arguments");
    *P=0;if(OffsetArg==NULL)OffsetArg=P+1;else SizeArg=P+1;
  }
  UINT64 Offset,Size;
  if(OffsetArg==NULL || SizeArg==NULL || !PartitionName(Arg) || !ParseFetchHex(OffsetArg,&Offset) ||
     !ParseFetchHex(SizeArg,&Size) || !Size || Size>PIANO_FASTBOOT_MAX_FETCH)return Reply(S,"FAILinvalid fetch range");
  PIANO_FB_PARTITION_INFO Info;EFI_STATUS Status=PartitionInfo(S,Arg,&Info);
  if(Status!=EFI_SUCCESS)return Reply(S,Status==EFI_NOT_FOUND?"FAILunknown partition":"FAILinvalid partition information");
  if(Offset>Info.Bytes || Size>Info.Bytes-Offset)return Reply(S,"FAILfetch outside partition");
  UINT8 *Data=AllocateZeroPool((UINTN)Size),*Scratch=NULL;
  if(Data==NULL)return Reply(S,"FAILnot enough RAM");
  UINTN Done=0,Block=Info.BlockSize;UINT64 Lba=Offset/Block;UINTN Within=(UINTN)(Offset%Block);
  // Fetch accepts byte ranges; the backend receives whole blocks only.
  // Stage the entire bounded chunk before DATA so a read failure cannot emit
  // a partial partition payload that the host could mistake for success.
  if(Within || Size%Block) {
    Scratch=AllocateZeroPool(Block);if(Scratch==NULL){Status=EFI_OUT_OF_RESOURCES;goto Finished;}
  }
  if(Within) {
    Status=S->Storage.ReadBlocks(S->Storage.Context,&Info,Lba,Block,Scratch);if(Status!=EFI_SUCCESS)goto Finished;
    UINTN Bytes=MIN((UINTN)Size,Block-Within);CopyMem(Data,Scratch+Within,Bytes);Done+=Bytes;++Lba;
  }
  UINTN Full=((UINTN)Size-Done)/Block*Block;
  if(Full) {
    Status=S->Storage.ReadBlocks(S->Storage.Context,&Info,Lba,Full,Data+Done);if(Status!=EFI_SUCCESS)goto Finished;
    Done+=Full;Lba+=Full/Block;
  }
  if(Done<(UINTN)Size) {
    // A misaligned head can leave a tail even when Size itself is aligned.
    if(Scratch==NULL){Scratch=AllocateZeroPool(Block);if(Scratch==NULL){Status=EFI_OUT_OF_RESOURCES;goto Finished;}}
    Status=S->Storage.ReadBlocks(S->Storage.Context,&Info,Lba,Block,Scratch);if(Status!=EFI_SUCCESS)goto Finished;
    CopyMem(Data+Done,Scratch,(UINTN)Size-Done);
  }
  {CHAR8 Result[13]="DATA";Hex32((UINT32)Size,Result+4);
    Status=Reply(S,Result);if(Status!=EFI_SUCCESS)goto Released;
    Status=S->Send(S->Context,Data,(UINTN)Size);if(Status!=EFI_SUCCESS)goto Released;
    Status=Reply(S,"OKAY");goto Released;}
Finished:
  Status=Reply(S,Status==EFI_OUT_OF_RESOURCES?"FAILnot enough RAM":"FAILpartition read failed");
Released:
  ZeroMem(Data,(UINTN)Size);FreePool(Data);
  if(Scratch!=NULL){ZeroMem(Scratch,Block);FreePool(Scratch);}
  return Status;
}
EFI_STATUS PianoFastbootPacket(PIANO_FASTBOOT *S, CONST VOID *Data, UINTN Bytes) {
  if(S==NULL || S->Send==NULL || (Data==NULL && Bytes!=0))return EFI_INVALID_PARAMETER;
  if(S->BootPreparing || S->BootPending || S->BootTransferFrozen)return EFI_NOT_READY;
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
  if(!AsciiStrnCmp(Cmd,"fetch:",6))return Fetch(S,Cmd+6);
  if(Equal(Cmd,"upload")) {
    if(S->Upload==NULL || !S->UploadBytes)return Reply(S,"FAILno staged RAM payload");
    CHAR8 Result[13]="DATA";Hex32((UINT32)S->UploadBytes,Result+4);
    EFI_STATUS Status=Reply(S,Result);if(EFI_ERROR(Status))return Status;
    Status=S->Send(S->Context,S->Upload,S->UploadBytes);if(EFI_ERROR(Status))return Status;
    return Reply(S,"OKAY");
  }
  if(Equal(Cmd,"oem sha256"))return HashDownload(S);
  if(Equal(Cmd,"oem discard")){PianoFastbootReset(S);return Reply(S,"OKAY");}
  if(Equal(Cmd,"oem setup") || Equal(Cmd,"oem shell") || Equal(Cmd,"oem simpleinit")) {
#if PIANO_USB_SERVICE
    if(S->Diagnostic==NULL)return Reply(S,"FAILUI navigation backend unavailable");
    EFI_STATUS Status=S->Diagnostic(S->Context,S,Cmd);
    return Status==EFI_UNSUPPORTED?Reply(S,"FAILUI navigation backend unavailable"):Status;
#else
    return Reply(S,"FAILcommand disabled by RAM-only policy");
#endif
  }
  if(Equal(Cmd,"oem status") || Equal(Cmd,"oem ramlog") || Equal(Cmd,"oem screenshot")) {
    if(S->Diagnostic==NULL)return Reply(S,"FAILdiagnostic service unavailable");
    EFI_STATUS Status=S->Diagnostic(S->Context,S,Cmd);
    return Status==EFI_UNSUPPORTED?Reply(S,"FAILdiagnostic command unavailable"):Status;
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
  if(Equal(Cmd,"boot")) {
#if PIANO_USB_RAM_BOOT
    return BootCommand(S);
#else
    return Reply(S,"FAILRAM boot handoff not implemented in debug v1");
#endif
  }
  // Default deny covers flash, erase, set_active, flashing, arbitrary OEM,
  // reboot-edl, reboot-bootloader, memory poke and unknown future commands.
  return Reply(S,"FAILcommand disabled by RAM-only policy");
}
