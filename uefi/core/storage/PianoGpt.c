// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, read-only GPT evidence parser. No BlockIO or storage commands.
#include "PianoGpt.h"
STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC UINT32 ByteCrc(UINT32 C,UINT8 Byte) {
  C^=Byte;for(UINTN I=0;I<8;++I)C=(C>>1)^((C&1)?0xEDB88320U:0);return C;
}
UINT32 PianoGptCrc32(CONST VOID *Data,UINTN Bytes) {
  CONST UINT8 *P=Data;UINT32 C=MAX_UINT32;
  for(UINTN I=0;I<Bytes;++I)C=ByteCrc(C,P[I]);return ~C;
}
EFI_STATUS PianoGptParseHeader(CONST VOID *Data,UINTN Bytes,UINT64 LastLba,UINT32 BlockBytes,PIANO_GPT_HEADER *H) {
  if(Data==NULL || H==NULL || (BlockBytes!=512 && BlockBytes!=4096) || Bytes!=BlockBytes || LastLba<2)return EFI_INVALID_PARAMETER;
  CONST UINT8 *P=Data;STATIC CONST CHAR8 Magic[]="EFI PART";
  for(UINTN I=0;I<8;++I)if(P[I]!=Magic[I])return EFI_NOT_FOUND;
  UINT32 Size=Le32(P+12);
  if(Le32(P+8)!=0x10000 || Size<92 || Size>Bytes || Le32(P+20)!=0 || Le64(P+24)!=1 || Le64(P+32)!=LastLba)
    return EFI_COMPROMISED_DATA;
  UINT32 C=MAX_UINT32;
  for(UINTN I=0;I<Size;++I)C=ByteCrc(C,(I>=16 && I<20)?0:P[I]);
  if(~C!=Le32(P+16))return EFI_CRC_ERROR;
  H->FirstUsable=Le64(P+40);H->LastUsable=Le64(P+48);H->EntryLba=Le64(P+72);
  H->Entries=Le32(P+80);H->EntryBytes=Le32(P+84);H->HeaderCrc=Le32(P+16);H->ArrayCrc=Le32(P+88);
  if(H->FirstUsable<2 || H->FirstUsable>H->LastUsable || H->LastUsable>=LastLba ||
     H->EntryLba<2 || H->EntryLba>MAX_UINT32 || !H->Entries || H->EntryBytes<128 ||
     (H->EntryBytes&(H->EntryBytes-1)) || H->EntryBytes>4096 ||
     H->Entries>PIANO_GPT_MAX_ARRAY_BYTES/H->EntryBytes)return EFI_UNSUPPORTED;
  H->ArrayBytes=H->Entries*H->EntryBytes;
  UINT64 Blocks=((UINT64)H->ArrayBytes+BlockBytes-1)/BlockBytes;
  if(H->EntryLba>=H->FirstUsable || Blocks>H->FirstUsable-H->EntryLba ||
     Blocks-1>MAX_UINT32-H->EntryLba)return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
EFI_STATUS PianoGptCheckEntries(CONST VOID *Data,UINTN Bytes,CONST PIANO_GPT_HEADER *H,UINTN *Active) {
  if(Data==NULL || H==NULL || Active==NULL || H->EntryBytes<128 || !H->Entries ||
     H->EntryBytes>4096 || H->Entries>PIANO_GPT_MAX_ARRAY_BYTES/H->EntryBytes ||
     Bytes!=H->ArrayBytes || Bytes!=H->Entries*H->EntryBytes)return EFI_INVALID_PARAMETER;
  *Active=0;if(PianoGptCrc32(Data,Bytes)!=H->ArrayCrc)return EFI_CRC_ERROR;
  CONST UINT8 *P=Data;
  for(UINTN I=0;I<H->Entries;++I) {
    CONST UINT8 *E=P+I*H->EntryBytes;BOOLEAN Used=FALSE;
    for(UINTN J=0;J<16;++J)if(E[J])Used=TRUE;
    if(!Used)continue;
    UINT64 First=Le64(E+32),Last=Le64(E+40);
    if(First>Last || First<H->FirstUsable || Last>H->LastUsable)return EFI_COMPROMISED_DATA;
    ++*Active;
  }
  return EFI_SUCCESS;
}
