// SPDX-License-Identifier: BSD-2-Clause-Patent
// NT36532 image layout and chip tables from the product Linux 7.3 driver
// (nt36xxx_fw_update.c, nt36xxx_mem_map.h), written independently
#include "PianoTouchNvt.h"

CONST PIANO_NVT_HW_REGS gPianoNvtHwRegs[3]={{0x1FB104,0x1FB43E},{0x3F004,0x3F0FE},{0x1F64E,0x1F01A}};

STATIC UINT32 Word(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}

UINT32 PianoNvtCrc32(CONST UINT8 *Data,UINT32 Length) {
  UINT32 Sum=0xFFFFFFFFU,Words=(Length+4)/4;
  for(UINT32 I=0;I<Words;++I) {
    Sum^=Word(Data+I*4);
    for(UINT32 J=0;J<32;++J)Sum=(Sum&0x80000000U)?(Sum<<1)^0x04C11DB7U:(Sum<<1);
  }
  return Sum;
}

EFI_STATUS PianoNvtParseImage(CONST UINT8 *D,UINTN Bytes,PIANO_NVT_IMAGE *Image) {
  if(D==NULL || Image==NULL || Bytes<0x2000 || Bytes>0x100000)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<sizeof(*Image);++I)((UINT8 *)Image)[I]=0;
  // The last 4 KiB sector ending in "NVT" or "MOD" bounds the image
  for(UINTN S=Bytes/4096;S>0 && !Image->NeedWriteSize;--S) {
    CONST UINT8 *E=D+S*4096-3;
    if((E[0]=='N'&&E[1]=='V'&&E[2]=='T') || (E[0]=='M'&&E[1]=='O'&&E[2]=='D'))Image->NeedWriteSize=(UINT32)(S*4096);
  }
  if(!Image->NeedWriteSize)return EFI_COMPROMISED_DATA;
  UINT32 Ver=Image->NeedWriteSize-4096;
  if((UINT8)(D[Ver]+D[Ver+1])!=0xFF)return EFI_COMPROMISED_DATA;
  Image->FwVer=D[Ver];
  UINT32 End=Word(D),Pos=0x30,Info=0,Overlay=0;
  if(End<0x30 || End>Bytes)return EFI_COMPROMISED_DATA;
  Image->Cascade2ndHeader=(D[0x20]&0x02)!=0;
  if(Image->Cascade2ndHeader){while(Pos<End/2){++Info;Pos+=0x10;}++Info;}
  else while(Pos<End){++Info;Pos+=0x10;}
  if(D[0x28]&0x10)Overlay=D[0x28]&0x0F;
  UINT32 Count=2+Overlay+Info;
  if(Count>PIANO_NVT_MAX_PARTITIONS)return EFI_UNSUPPORTED;
  BOOLEAN FoundHeader=FALSE;Pos=0x30;
  for(UINT32 L=0;L<Count;++L) {
    PIANO_NVT_PARTITION *P=&Image->Part[L];
    if(L<2) {
      P->BinAddr=Word(D+L*12);P->SramAddr=Word(D+4+L*12);P->Size=Word(D+8+L*12);
      if((UINT64)P->BinAddr+(P->Size+4)/4*4>Bytes)return EFI_COMPROMISED_DATA;
      P->Crc=PianoNvtCrc32(D+P->BinAddr,P->Size);
    } else {
      // As in the driver, a record after the header keeps the previous
      // position unless a cascaded second header sits at End - 0x10
      if(L<2+Info){if(!FoundHeader)Pos=0x30+0x10*(L-2);else if(Image->Cascade2ndHeader)Pos=End-0x10;}
      else Pos=Image->Part[1].BinAddr+0x10*(L-2-Info);
      UINT32 At=Pos;
      if((UINT64)At+16>Bytes)return EFI_COMPROMISED_DATA;
      P->SramAddr=Word(D+At);P->Size=Word(D+At+4);P->BinAddr=Word(D+At+8);P->Crc=Word(D+At+12);
      if(L<2+Info && P->BinAddr<End && P->Size)FoundHeader=TRUE;
    }
    if((UINT64)P->BinAddr+P->Size>Bytes)return EFI_COMPROMISED_DATA;
  }
  Image->Count=Count;
  return EFI_SUCCESS;
}

INTN PianoNvtMatchTrim(CONST UINT8 Id[6]) {
  STATIC CONST UINT8 Ids[2][6]={{0x0E,0xFF,0xFF,0x32,0x65,0x03},{0xFF,0xFF,0xFF,0x32,0x65,0x03}};
  STATIC CONST UINT8 Masks[2][6]={{1,0,0,1,1,1},{0,0,0,1,1,1}};
  for(INTN R=0;R<2;++R) {
    UINTN I=0;
    for(;I<6;++I)if(Masks[R][I] && Id[I]!=Ids[R][I])break;
    if(I==6)return R;
  }
  return -1;
}

VOID PianoNvtMemoryMap(INTN Row,BOOLEAN Cascade,PIANO_NVT_MAP *M) {
  // Same in all three NT36532 maps
  M->TxAutoCopyEn=0x1FC925;M->BootRdy=0x1FB50D;
  M->IlmDes=0x1FB528;M->IlmLength=0x1FB518;M->GIlmChecksum=0x1FB500;
  M->DlmDes=0x1FB52C;M->DlmLength=0x1FB530;M->GDlmChecksum=0x1FB504;
  // The single-chip map has no poll info
  (VOID)Row;
  M->EventBuf=Cascade?0x11C400:0x125800;M->PollInfo=Cascade?0x1093D8:0;
}
