// SPDX-License-Identifier: BSD-2-Clause-Patent
// WN8030 byte formats of linux-piano drivers/hid/hid-nanosic-wn8030.c
// (wn8030_bin_header, wn8030_header_cmd, wn8030_send_cmd), written independently
#include "PianoPogoMcu.h"

STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC VOID PutLe16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
STATIC VOID PutLe32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC VOID PutBe32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(24-8*I));}
STATIC UINT8 Sum8(CONST UINT8 *P,UINTN N){UINT8 S=0;for(UINTN I=0;I<N;++I)S=(UINT8)(S+P[I]);return S;}

EFI_STATUS PianoPogoMcuHeaderCommand(CONST UINT8 *Image,CONST UINT8 *Code,UINT32 CodeBytes,UINT8 Wire[PIANO_POGO_HEADER_WIRE_BYTES]) {
  STATIC CONST UINT8 Magic[7]={'N','A','N','O',' ','I','C'};
  if(Image==NULL || Code==NULL || Wire==NULL)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<7;++I)if(Image[I]!=Magic[I])return EFI_COMPROMISED_DATA;
  // Image header: flag[7] chip16 ro32 code32 codesize32 entry32 pid16 vid16
  // major8 minor8 version[17] ram_code_size32 ram_code_sum32 ...
  UINT32 Size=Le32(Image+48);
  if(Size!=CodeBytes || !Size || Size>PIANO_POGO_MAX_CODE_BYTES || (Size&3))return EFI_COMPROMISED_DATA;
  UINT8 *H=Wire+1;
  for(UINTN I=0;I<PIANO_POGO_HEADER_WIRE_BYTES;++I)Wire[I]=0;
  Wire[0]=PIANO_POGO_BOOT_REGISTER;
  H[0]=0x06;
  for(UINTN I=0;I<7;++I)H[1+I]=Image[I];
  PutLe16(H+8,0x8140);            // chip id expected by the bootrom
  PutLe32(H+10,0x2345);           // RAM code source
  PutLe32(H+14,0x40);             // RAM code destination
  PutLe32(H+18,Size);
  for(UINTN I=0;I<4;++I)H[22+I]=Image[21+I]; // code entry
  for(UINTN I=0;I<4;++I)H[26+I]=Image[25+I]; // pid, vid
  H[30]=Image[29];H[31]=Image[30];           // major, minor
  UINT32 Sum=0;for(UINT32 I=0;I<Size;I+=4)Sum+=Le32(Code+I);
  PutLe32(H+53,Sum);
  H[64]=Sum8(H+1,64);
  return EFI_SUCCESS;
}

UINT32 PianoPogoMcuDataCommand(CONST UINT8 *Code,UINT32 CodeBytes,UINT32 Offset,UINT8 Wire[PIANO_POGO_DATA_WIRE_BYTES]) {
  if(Code==NULL || Wire==NULL || Offset>=CodeBytes)return 0;
  UINT32 Bytes=MIN(CodeBytes-Offset,PIANO_POGO_DATA_CHUNK);
  for(UINTN I=0;I<PIANO_POGO_DATA_WIRE_BYTES;++I)Wire[I]=0;
  Wire[0]=PIANO_POGO_BOOT_REGISTER;Wire[1]=0x07;
  PutBe32(Wire+2,Offset);PutBe32(Wire+6,Bytes);
  for(UINT32 I=0;I<Bytes;++I)Wire[10+I]=Code[Offset+I];
  return Bytes;
}

VOID PianoPogoMcuBootCommand(UINT8 Command,UINT8 Wire[2]){Wire[0]=PIANO_POGO_BOOT_REGISTER;Wire[1]=Command;}

VOID PianoPogoMcuRuntimeCommand(UINT8 Command,UINT8 Value,UINT8 Wire[PIANO_POGO_COMMAND_WIRE_BYTES]) {
  for(UINTN I=0;I<PIANO_POGO_COMMAND_WIRE_BYTES;++I)Wire[I]=0;
  Wire[0]=PIANO_POGO_RUNTIME_REGISTER;
  UINT8 *B=Wire+1;
  B[0]=0x32;B[1]=0x00;B[2]=0x4E;B[3]=0x31;B[4]=0x80;B[5]=0x38;B[6]=Command;B[7]=0x01;B[8]=Value;
  B[9]=Sum8(B+2,7);
}

BOOLEAN PianoPogoMcuFrameHasReports(CONST UINT8 Frame[PIANO_POGO_REPORT_BYTES]) {
  return Frame!=NULL && Frame[0]==0x57 && Frame[2]!=0;
}
