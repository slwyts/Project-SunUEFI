// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFastbootBoot.h"
#include <Library/BaseMemoryLib.h>
#define BOOT_MAX_SECTIONS 96U
STATIC UINT16 B16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC UINT32 B32(CONST UINT8 *P){return (UINT32)B16(P)|((UINT32)B16(P+2)<<16);}
STATIC UINT64 B64(CONST UINT8 *P){return (UINT64)B32(P)|((UINT64)B32(P+4)<<32);}
STATIC BOOLEAN Power(UINT32 V){return V && !(V&(V-1));}
STATIC BOOLEAN Range(UINT64 Offset,UINT64 Bytes,UINT64 Limit){return Offset<=Limit && Bytes<=Limit-Offset;}
STATIC EFI_STATUS Read(CONST PIANO_BOOT_SOURCE *S,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  if(!Range(Offset,Bytes,S->Bytes))return EFI_COMPROMISED_DATA;
  ZeroMem(Buffer,Bytes);EFI_STATUS Status=S->Read(S->Context,Offset,Bytes,Buffer);
  return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}
typedef struct {CONST PIANO_BOOT_SOURCE *Source;UINT64 Base;} VIEW;
STATIC EFI_STATUS ViewRead(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  VIEW *V=Context;if(Offset>MAX_UINT64-V->Base)return EFI_COMPROMISED_DATA;
  return Read(V->Source,V->Base+Offset,Bytes,Buffer);
}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 N,UINT64 B,UINT64 M){return N && M && A<B+M && B<A+N;}
STATIC EFI_STATUS ParsePe(CONST PIANO_BOOT_SOURCE *S,PIANO_BOOT_PE *Out) {
  UINT8 Dos[64],Nt[24],Opt[240],Section[40];EFI_STATUS Status=Read(S,0,sizeof(Dos),Dos);if(EFI_ERROR(Status))return Status;
  if(B16(Dos)!=0x5A4D)return EFI_UNSUPPORTED;
  UINT32 PeOffset=B32(Dos+60);if(PeOffset<64)return EFI_COMPROMISED_DATA;
  Status=Read(S,PeOffset,sizeof(Nt),Nt);if(EFI_ERROR(Status))return Status;
  if(B32(Nt)!=0x00004550)return EFI_COMPROMISED_DATA;
  UINT16 Sections=B16(Nt+6),OptBytes=B16(Nt+20);
  if(B16(Nt+4)!=0xAA64)return EFI_UNSUPPORTED;
  if(!Sections || Sections>BOOT_MAX_SECTIONS || OptBytes<112 || OptBytes>sizeof(Opt) || !(B16(Nt+22)&2))return EFI_COMPROMISED_DATA;
  UINT32 Symbols=B32(Nt+16),SymbolOffset=B32(Nt+12);
  if(Symbols && (!SymbolOffset || !Range(SymbolOffset,(UINT64)Symbols*18,S->Bytes)))return EFI_COMPROMISED_DATA;
  UINT64 OptOffset=(UINT64)PeOffset+sizeof(Nt),Table=OptOffset+OptBytes;
  Status=Read(S,OptOffset,OptBytes,Opt);if(EFI_ERROR(Status))return Status;
  if(B16(Opt)!=0x20B || B16(Opt+68)!=10)return EFI_UNSUPPORTED; // EFI application, PE32+.
  UINT32 Sa=B32(Opt+32),Fa=B32(Opt+36),Image=B32(Opt+56),Headers=B32(Opt+60),Entry=B32(Opt+16),Dirs=B32(Opt+108);
  if(!Power(Sa) || !Power(Fa) || Fa<512 || Fa>65536 || Sa<Fa || (Sa<4096 && Sa!=Fa) ||
     !Image || Image%Sa || !Headers || Headers%Fa || Headers>S->Bytes || Headers>Image ||
     !Entry || Entry>=Image || Dirs>16 || Dirs>(OptBytes-112U)/8U ||
     !Range(Table,(UINT64)Sections*40,Headers) || B64(Opt+24)>MAX_UINT64-Image)return EFI_COMPROMISED_DATA;
  typedef struct {UINT32 Rva,Span,Raw,Bytes;} PART;
  PART Parts[BOOT_MAX_SECTIONS];ZeroMem(Parts,sizeof(Parts));BOOLEAN EntryFound=FALSE;
  for(UINTN N=0;N<Sections;++N) {
    Status=Read(S,Table+N*40,sizeof(Section),Section);if(EFI_ERROR(Status))return Status;
    PART P={.Rva=B32(Section+12),.Span=MAX(B32(Section+8),B32(Section+16)),.Raw=B32(Section+20),.Bytes=B32(Section+16)};
    if((P.Span && (P.Rva%Sa || P.Rva<Headers || !Range(P.Rva,P.Span,Image))) ||
       (P.Bytes && (P.Raw%Fa || P.Bytes%Fa || P.Raw<Headers || !Range(P.Raw,P.Bytes,S->Bytes))))return EFI_COMPROMISED_DATA;
    if((B16(Section+32) && (!B32(Section+24) || !Range(B32(Section+24),(UINT64)B16(Section+32)*10,S->Bytes))) ||
       (B16(Section+34) && (!B32(Section+28) || !Range(B32(Section+28),(UINT64)B16(Section+34)*6,S->Bytes))))return EFI_COMPROMISED_DATA;
    for(UINTN I=0;I<N;++I)if(Overlap(P.Rva,P.Span,Parts[I].Rva,Parts[I].Span) || Overlap(P.Raw,P.Bytes,Parts[I].Raw,Parts[I].Bytes))return EFI_COMPROMISED_DATA;
    if(Entry>=P.Rva && Entry-P.Rva<P.Bytes && (B32(Section+36)&0x20000000U)) {
      if(EntryFound)return EFI_COMPROMISED_DATA;
      EntryFound=TRUE;Out->EntryFileOffset=P.Raw+(UINT64)(Entry-P.Rva);
    }
    Parts[N]=P;
  }
  if(!EntryFound)return EFI_COMPROMISED_DATA;
  for(UINTN N=0;N<Dirs;++N) {
    if(N==4)continue; // Certificate directory is a file offset, not an RVA.
    UINT32 Rva=B32(Opt+112+N*8),Bytes=B32(Opt+116+N*8);
    if((Rva==0)!=(Bytes==0) || !Range(Rva,Bytes,Image))return EFI_COMPROMISED_DATA;
    if(!Bytes)continue;
    BOOLEAN FileBacked=Range(Rva,Bytes,Headers);
    for(UINTN I=0;I<Sections;++I)if(Rva>=Parts[I].Rva && Range(Rva-Parts[I].Rva,Bytes,Parts[I].Bytes))FileBacked=TRUE;
    if(!FileBacked)return EFI_COMPROMISED_DATA;
  }
  if(Dirs>4) {
    UINT32 Offset=B32(Opt+112+4*8),Bytes=B32(Opt+116+4*8);
    if((Offset==0)!=(Bytes==0) || (Bytes && (Offset%8 || Offset<Headers || !Range(Offset,Bytes,S->Bytes))))return EFI_COMPROMISED_DATA;
    for(UINTN I=0;I<Sections;++I)if(Overlap(Offset,Bytes,Parts[I].Raw,Parts[I].Bytes))return EFI_COMPROMISED_DATA;
    Out->Certificate=(PIANO_BOOT_RANGE){Offset,Bytes}; // Bounds only, no signature validation.
  }
  Out->Machine=0xAA64;Out->Subsystem=10;Out->Sections=Sections;Out->EntryRva=Entry;Out->ImageBytes=Image;
  Out->HeaderBytes=Headers;Out->SectionAlignment=Sa;Out->FileAlignment=Fa;Out->PreferredBase=B64(Opt+24);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Component(CONST PIANO_BOOT_SOURCE *S,UINT64 *Cursor,UINT32 Bytes,UINT32 Page,PIANO_BOOT_RANGE *Out) {
  if(!Range(*Cursor,Bytes,S->Bytes))return EFI_COMPROMISED_DATA;
  Out->Offset=*Cursor;Out->Bytes=Bytes;
  UINT64 Rounded=((UINT64)Bytes+Page-1)&~((UINT64)Page-1);
  if(!Range(*Cursor,Rounded,S->Bytes))return EFI_COMPROMISED_DATA;
  *Cursor+=Rounded;return EFI_SUCCESS;
}
STATIC EFI_STATUS ParseAndroid(CONST PIANO_BOOT_SOURCE *S,PIANO_BOOT_IMAGE *Out) {
  UINT8 H[1660];EFI_STATUS Status=Read(S,0,44,H);if(EFI_ERROR(Status))return Status;
  UINT32 Version=B32(H+40),Header,Page,Kernel,Ramdisk,Second=0,Dtbo=0,Dtb=0,Signature=0;
  if(Version>4)return EFI_UNSUPPORTED;
  Header=Version<3?(Version==0?1632:Version==1?1648:1660):(Version==3?1580:1584);
  Status=Read(S,0,Header,H);if(EFI_ERROR(Status))return Status;
  if(Version<3) {
    Page=B32(H+36);Kernel=B32(H+8);Ramdisk=B32(H+16);Second=B32(H+24);
    Out->Cmdline=(PIANO_BOOT_RANGE){64,512};Out->ExtraCmdline=(PIANO_BOOT_RANGE){608,1024};
    if(!Power(Page) || Page<Header || Page>65536)return EFI_COMPROMISED_DATA;
    if(Version>=1){Dtbo=B32(H+1632);if(B32(H+1644)!=Header)return EFI_COMPROMISED_DATA;}
    if(Version==2)Dtb=B32(H+1648);
  } else {
    Page=4096;Kernel=B32(H+8);Ramdisk=B32(H+12);
    Out->Cmdline=(PIANO_BOOT_RANGE){44,1536};
    if(Version==4)Signature=B32(H+1580);
    // AOSP fastboot37's raw-input wrapper emits sizeof(v3) in a v4 header.
    // Recognize only the measured zero-signature form, flag it explicitly,
    // and still require/read the full v4 structure within the 4096-byte page.
    Out->KnownV4CliHeaderQuirk=Version==4 && B32(H+20)==1580 && Signature==0;
    if((B32(H+20)!=Header && !Out->KnownV4CliHeaderQuirk) || B32(H+24) || B32(H+28) || B32(H+32) || B32(H+36))return EFI_COMPROMISED_DATA;
  }
  if(!Kernel && !Ramdisk)return EFI_COMPROMISED_DATA;
  UINT64 Cursor=Page;
  Status=Component(S,&Cursor,Kernel,Page,&Out->Kernel);if(EFI_ERROR(Status))return Status;
  Status=Component(S,&Cursor,Ramdisk,Page,&Out->Ramdisk);if(EFI_ERROR(Status))return Status;
  if(Version<3) {
    Status=Component(S,&Cursor,Second,Page,&Out->Second);if(EFI_ERROR(Status))return Status;
    if(Version>=1 && Dtbo && B64(H+1636)!=Cursor)return EFI_COMPROMISED_DATA;
    Status=Component(S,&Cursor,Dtbo,Page,&Out->RecoveryDtbo);if(EFI_ERROR(Status))return Status;
    Status=Component(S,&Cursor,Dtb,Page,&Out->Dtb);if(EFI_ERROR(Status))return Status;
  } else if(Version==4) {
    if(!Range(Cursor,Signature,S->Bytes))return EFI_COMPROMISED_DATA;
    Out->Signature=(PIANO_BOOT_RANGE){Cursor,Signature};Cursor+=Signature;
  }
  Out->Kind=PianoBootAndroid;Out->Version=Version;Out->HeaderBytes=Header;Out->PageBytes=Page;
  Out->DeclaredHeaderBytes=Version<3?(Version?B32(H+1644):Header):B32(H+20);
  Out->Trailing=(PIANO_BOOT_RANGE){Cursor,S->Bytes-Cursor};Out->KernelPeStatus=EFI_UNSUPPORTED;
  if(Kernel>=2) {
    UINT8 Magic[2];Status=Read(S,Out->Kernel.Offset,2,Magic);if(EFI_ERROR(Status))return Status;
    if(B16(Magic)==0x5A4D) {
      VIEW View={S,Out->Kernel.Offset};PIANO_BOOT_SOURCE K={&View,ViewRead,Kernel};
      Out->KernelPeStatus=ParsePe(&K,&Out->Pe);Out->KernelIsArm64Pe=Out->KernelPeStatus==EFI_SUCCESS;
      if(Out->KernelPeStatus!=EFI_SUCCESS && Out->KernelPeStatus!=EFI_UNSUPPORTED && Out->KernelPeStatus!=EFI_COMPROMISED_DATA)return Out->KernelPeStatus;
      if(!Out->KernelIsArm64Pe)ZeroMem(&Out->Pe,sizeof(Out->Pe));
    }
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootBootParse(CONST PIANO_BOOT_SOURCE *S,PIANO_BOOT_IMAGE *Image) {
  if(Image==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Image,sizeof(*Image));
  if(S==NULL || S->Read==NULL || !S->Bytes)return EFI_INVALID_PARAMETER;
  UINT8 Magic[8];EFI_STATUS Status=Read(S,0,sizeof(Magic),Magic);if(EFI_ERROR(Status))return Status;
  PIANO_BOOT_IMAGE Out;ZeroMem(&Out,sizeof(Out));
  if(!CompareMem(Magic,"ANDROID!",8))Status=ParseAndroid(S,&Out);
  else if(B16(Magic)==0x5A4D){Status=ParsePe(S,&Out.Pe);if(Status==EFI_SUCCESS){Out.Kind=PianoBootArm64Pe;Out.Kernel=(PIANO_BOOT_RANGE){0,S->Bytes};}}
  else Status=EFI_UNSUPPORTED;
  if(Status==EFI_SUCCESS)*Image=Out;
  return Status;
}
