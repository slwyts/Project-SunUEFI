// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoUkiImage.h"
#include <Library/BaseMemoryLib.h>

STATIC UINT16 Uki16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC UINT32 Uki32(CONST UINT8 *P){return Uki16(P)|((UINT32)Uki16(P+2)<<16);}
STATIC BOOLEAN UkiRange(UINTN At,UINTN Bytes,UINTN Limit){return At<=Limit&&Bytes<=Limit-At;}
STATIC EFI_STATUS UkiRead(VOID *Context,UINT64 At,UINTN Bytes,VOID *Out){
  CopyMem(Out,(CONST UINT8 *)Context+(UINTN)At,Bytes);return EFI_SUCCESS;
}

EFI_STATUS PianoUkiImageKernel(CONST VOID *LoadedBase,UINTN LoadedBytes,PIANO_BOOT_SOURCE *Kernel){
  if(!Kernel)return EFI_INVALID_PARAMETER;
  PIANO_BOOT_SOURCE Empty={0};*Kernel=Empty;
  if(!LoadedBase||LoadedBytes<64||LoadedBytes>0x10000000||
     (UINTN)LoadedBase>MAX_UINTN-LoadedBytes)return EFI_INVALID_PARAMETER;
  CONST UINT8 *Base=LoadedBase;
  if(Uki16(Base)!=0x5a4d)return EFI_UNSUPPORTED;
  UINT32 Pe=Uki32(Base+60);
  if(Pe<64||!UkiRange(Pe,24,LoadedBytes)||Uki32(Base+Pe)!=0x4550)return EFI_COMPROMISED_DATA;
  UINT16 Count=Uki16(Base+Pe+6),Optional=Uki16(Base+Pe+20);
  if(Uki16(Base+Pe+4)!=0xaa64||!Count||Count>96||Optional<112||Optional>240||
     !UkiRange(Pe+24,Optional,LoadedBytes))return EFI_COMPROMISED_DATA;
  CONST UINT8 *Opt=Base+Pe+24;
  UINT32 ImageBytes=Uki32(Opt+56),Headers=Uki32(Opt+60);
  UINTN Table=(UINTN)Pe+24+Optional;
  if(Uki16(Opt)!=0x20b||Uki16(Opt+68)!=10||!ImageBytes||ImageBytes>LoadedBytes||
     Headers>ImageBytes||!UkiRange(Table,(UINTN)Count*40,Headers))return EFI_COMPROMISED_DATA;
  CONST UINT8 Name[8]={'.','l','i','n','u','x',0,0};
  PIANO_BOOT_SOURCE Found={0};
  for(UINTN I=0;I<Count;++I){
    CONST UINT8 *Section=Base+Table+I*40;
    UINT32 Bytes=Uki32(Section+8),Rva=Uki32(Section+12);
    if(Bytes&&(Rva<Headers||!UkiRange(Rva,Bytes,ImageBytes)))return EFI_COMPROMISED_DATA;
    if(CompareMem(Section,Name,sizeof(Name)))continue;
    if(Found.Context||Bytes<64)return EFI_COMPROMISED_DATA;
    Found=(PIANO_BOOT_SOURCE){(VOID *)(Base+Rva),UkiRead,Bytes};
  }
  if(!Found.Context)return EFI_NOT_FOUND;
  *Kernel=Found;return EFI_SUCCESS;
}

EFI_STATUS PianoUkiImageVerifyCode(CONST PIANO_BOOT_SOURCE *Kernel,CONST VOID *Base,UINTN Bytes){
  if(!Kernel||!Kernel->Context||!Base||!Bytes||Bytes>0x10000000||
     (UINTN)Base>MAX_UINTN-Bytes)return EFI_INVALID_PARAMETER;
  PIANO_BOOT_IMAGE Parsed;EFI_STATUS Status=PianoFastbootBootParse(Kernel,&Parsed);
  if(Status!=EFI_SUCCESS)return Status;
  if(Parsed.Kind!=PianoBootArm64Pe||Parsed.Pe.ImageBytes!=Bytes)return EFI_ACCESS_DENIED;
  CONST UINT8 *File=Kernel->Context;
  UINT32 Pe=Uki32(File+60);UINTN Table=Pe+24+Uki16(File+Pe+20);
  for(UINTN I=0;I<Parsed.Pe.Sections;++I){
    CONST UINT8 *Section=File+Table+40*I;
    if(!(Uki32(Section+36)&0x20000000U))continue;
    UINT32 Rva=Uki32(Section+12),Raw=Uki32(Section+20),RawBytes=Uki32(Section+16);
    if(!RawBytes||!UkiRange(Rva,RawBytes,Bytes)||!UkiRange(Raw,RawBytes,(UINTN)Kernel->Bytes))
      return EFI_COMPROMISED_DATA;
    if(CompareMem((CONST UINT8 *)Base+Rva,File+Raw,RawBytes))return EFI_SECURITY_VIOLATION;
  }
  return EFI_SUCCESS;
}
