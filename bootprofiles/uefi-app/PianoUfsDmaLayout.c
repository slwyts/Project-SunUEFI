// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exact little-endian UTRD and big-endian UPIU wire layouts. Constructors expose
// NOP, read queries/SCSI and no-data resume; no block write or write queries.
#include "PianoUfsDmaLayout.h"
#include <Library/BaseMemoryLib.h>
STATIC VOID Le32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC UINT32 ReadLe32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC VOID Be16(UINT8 *P,UINT16 V){P[0]=(UINT8)(V>>8);P[1]=(UINT8)V;}
STATIC EFI_STATUS Init(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINTN ResponseBytes) {
  if(Trd==NULL || Ucd==NULL || TrdBytes<32 || UcdBytes<PIANO_UFS_UCD_BYTES ||
     (Iova&127) || Iova>MAX_UINT64-PIANO_UFS_UCD_BYTES || ResponseBytes>PIANO_UFS_UCD_BYTES-PIANO_UFS_RESPONSE_OFFSET)
    return EFI_INVALID_PARAMETER;
  ZeroMem(Trd,32);ZeroMem(Ucd,PIANO_UFS_UCD_BYTES);UINT8 *T=Trd;
  Le32(T,(1U<<28)|(1U<<24)); // CT=UFS storage, interrupt completion, no data PRDT.
  Le32(T+8,0xF);Le32(T+16,(UINT32)Iova);Le32(T+20,(UINT32)(Iova>>32));
  Le32(T+24,(UINT32)((ResponseBytes+3)/4)|((PIANO_UFS_RESPONSE_OFFSET/4)<<16));
  return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildNop(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINT8 Tag) {
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,Iova,32);if(EFI_ERROR(Status))return Status;
  UINT8 *Request=Ucd;Request[0]=0;Request[3]=Tag;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildReadDescriptor(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,
                                      UINT8 Tag,UINT8 Idn,UINT8 Index,UINT16 Bytes) {
  if(Bytes==0 || Bytes>512 || !(Idn==0 || Idn==2 || Idn==7) || (Idn!=2 && Index!=0) || Index>7)
    return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,Iova,32+Bytes);if(EFI_ERROR(Status))return Status;
  UINT8 *R=Ucd;R[0]=0x16;R[3]=Tag;R[5]=1; // Query request, standard read query function.
  R[12]=1;R[13]=Idn;R[14]=Index;Be16(R+18,Bytes);return EFI_SUCCESS;
}
EFI_STATUS PianoUfsCheckNop(CONST VOID *Trd,CONST VOID *Ucd,UINT8 Tag) {
  if(Trd==NULL || Ucd==NULL)return EFI_INVALID_PARAMETER;
  CONST UINT8 *T=Trd,*R=(CONST UINT8 *)Ucd+PIANO_UFS_RESPONSE_OFFSET;
  if((ReadLe32(T+8)&0xFF)!=0 || (R[0]&0x3F)!=0x20 || R[3]!=Tag || R[6]!=0 || R[10]!=0 || R[11]!=0)
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildReadPowerMode(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINT8 Tag) {
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,Iova,32);if(EFI_ERROR(Status))return Status;
  UINT8 *R=Ucd;R[0]=0x16;R[3]=Tag;R[5]=1;R[12]=3;R[13]=2;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildReadWriteProtectFlag(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINT8 Tag,UINT8 Idn) {
  if(Idn!=2 && Idn!=3)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,Iova,32);if(EFI_ERROR(Status))return Status;
  UINT8 *R=Ucd;R[0]=0x16;R[3]=Tag;R[5]=1;R[12]=5;R[13]=Idn;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildResumeActive(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINT8 Tag) {
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,Iova,64);if(EFI_ERROR(Status))return Status;
  UINT8 *R=Ucd;R[0]=1;R[2]=0xD0;R[3]=Tag;R[16]=0x1B;R[20]=0x10;return EFI_SUCCESS;
}
STATIC VOID Be32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[3-I]=(UINT8)(V>>(8*I));}
STATIC UINT32 ReadBe32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
EFI_STATUS PianoUfsCheckReadResponse(CONST VOID *Trd,CONST VOID *Ucd,UINT8 Tag,UINT32 Requested,UINT32 *Transferred) {
  if(Trd==NULL || Ucd==NULL || Transferred==NULL)return EFI_INVALID_PARAMETER;
  CONST UINT8 *T=Trd,*R=(CONST UINT8 *)Ucd+PIANO_UFS_RESPONSE_OFFSET;*Transferred=0;
  if((ReadLe32(T+8)&0xFF)!=0 || (R[0]&0x3F)!=0x21 || R[3]!=Tag || R[6]!=0 || R[7]!=0)return EFI_DEVICE_ERROR;
  UINT32 Residual=ReadBe32(R+12);
  // Overflow cannot fit our PRDT. Residual is valid only for underflow.
  if((R[1]&0x40) || Residual>Requested || (Residual && !(R[1]&0x20)))return EFI_COMPROMISED_DATA;
  *Transferred=Requested-Residual;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsParseLuns(CONST VOID *Data,UINTN Bytes,UINT8 Luns[8],UINTN *Count) {
  if(Data==NULL || Luns==NULL || Count==NULL || Bytes<8)return EFI_INVALID_PARAMETER;
  CONST UINT8 *P=Data;UINT32 Length=ReadBe32(P);*Count=0;
  if((Length&7) || Length>Bytes-8)return EFI_COMPROMISED_DATA;
  for(UINTN I=8;I<8+Length;I+=8) {
    // Only standard peripheral addressing on bus zero and normal UFS LUNs.
    // Well-known/extended logical unit formats are not used as block targets.
    if(P[I]!=0 || P[I+1]>7)continue;
    BOOLEAN Normal=TRUE;for(UINTN J=2;J<8;++J)if(P[I+J])Normal=FALSE;
    if(!Normal)continue;
    for(UINTN J=0;J<*Count;++J)if(Luns[J]==P[I+1])return EFI_COMPROMISED_DATA;
    if(*Count==8)return EFI_COMPROMISED_DATA;
    Luns[(*Count)++]=P[I+1];
  }
  return *Count?EFI_SUCCESS:EFI_NOT_FOUND;
}
EFI_STATUS PianoUfsParseCapacity(CONST VOID *Data,UINTN Bytes,UINT64 *LastLba,UINT32 *BlockBytes) {
  if(Data==NULL || LastLba==NULL || BlockBytes==NULL || Bytes<32)return EFI_INVALID_PARAMETER;
  CONST UINT8 *P=Data;*LastLba=((UINT64)ReadBe32(P)<<32)|ReadBe32(P+4);*BlockBytes=ReadBe32(P+8);
  if(*LastLba==MAX_UINT64 || *LastLba<1 || (*BlockBytes!=512 && *BlockBytes!=4096))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBuildReadCommand(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,
                                  UINT64 UcdIova,UINT64 DataIova,UINT32 DataBytes,
                                  UINT8 Tag,UINT8 Lun,PIANO_UFS_READ_COMMAND Cmd,UINT32 Lba,UINT16 Blocks) {
  if((DataIova&3) || DataBytes==0 || DataBytes>4096 || DataIova>MAX_UINT64-DataBytes || Lun>7 ||
     Cmd>PianoUfsModeSense10 || (Cmd==PianoUfsReadCapacity16 && DataBytes!=32) ||
     (Cmd==PianoUfsReadLba10 && (!Blocks || (DataBytes!=512U*Blocks && DataBytes!=4096U*Blocks))))
    return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Init(Trd,TrdBytes,Ucd,UcdBytes,UcdIova,64);if(EFI_ERROR(Status))return Status;
  UINT8 *T=Trd,*R=Ucd;Le32(T,0x15000000); // READ from device, CT=1, INT=1.
  Le32(T+28,1U|((256U/4)<<16)); // One PRDT entry at byte 256.
  R[0]=1;R[1]=0x40;R[2]=Lun;R[3]=Tag;Be32(R+12,DataBytes);
  UINT8 *Cdb=R+16;
  if(Cmd==PianoUfsReportLuns){Cdb[0]=0xA0;Be32(Cdb+6,DataBytes);}
  else if(Cmd==PianoUfsReadCapacity16){Cdb[0]=0x9E;Cdb[1]=0x10;Be32(Cdb+10,32);}
  else if(Cmd==PianoUfsModeSense10){Cdb[0]=0x5A;Cdb[1]=0x08;Cdb[2]=0x08;Be16(Cdb+7,(UINT16)DataBytes);}
  else {Cdb[0]=0x28;Be32(Cdb+2,Lba);Be16(Cdb+7,Blocks);}
  UINT8 *Prdt=R+256;Le32(Prdt,(UINT32)DataIova);Le32(Prdt+4,(UINT32)(DataIova>>32));
  Le32(Prdt+12,DataBytes-1);return EFI_SUCCESS;
}
EFI_STATUS PianoUfsParseCacheMode(CONST VOID *Data,UINTN Bytes,BOOLEAN *WriteProtected,BOOLEAN *Fua,BOOLEAN *WriteCache,BOOLEAN *ReadCacheDisabled) {
  if(Data==NULL || WriteProtected==NULL || Fua==NULL || WriteCache==NULL || ReadCacheDisabled==NULL)return EFI_INVALID_PARAMETER;
  *WriteProtected=*Fua=*WriteCache=*ReadCacheDisabled=FALSE;
  if(Bytes<8)return EFI_COMPROMISED_DATA;
  CONST UINT8 *P=Data;UINTN Total=(((UINTN)P[0]<<8)|P[1])+2;
  UINTN DescriptorBytes=((UINTN)P[6]<<8)|P[7],Page=8+DescriptorBytes;
  if(Total<8 || Total>Bytes || Page>Total || Total-Page<3)return EFI_COMPROMISED_DATA;
  // PC=0, page 08 (current caching), DBD=1. Do not turn an unsupported or
  // truncated page into a claim that writes/FUA are safe.
  if((P[Page]&0x3F)!=8 || (P[Page]&0x40))return EFI_UNSUPPORTED;
  if(P[Page+1]<0x12 || (UINTN)P[Page+1]+2>Total-Page)return EFI_COMPROMISED_DATA;
  *WriteProtected=(P[3]&0x80)!=0;*Fua=(P[3]&0x10)!=0;
  *WriteCache=(P[Page+2]&4)!=0;*ReadCacheDisabled=(P[Page+2]&1)!=0;
  return EFI_SUCCESS;
}
