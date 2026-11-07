// SPDX-License-Identifier: BSD-2-Clause-Patent
// Byte layouts of the WN8030 bootrom download and runtime commands, checked
// against an independent re-computation and, when given, the pinned image
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "PianoPogoMcu.h"

static UINT32 Le32(const UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT32 Be32(const UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}

static void CheckImage(const UINT8 *Image,const UINT8 *Code,UINT32 Size){
  UINT8 Wire[PIANO_POGO_HEADER_WIRE_BYTES];
  assert(PianoPogoMcuHeaderCommand(Image,Code,Size,Wire)==EFI_SUCCESS);
  const UINT8 *H=Wire+1;UINT32 Sum=0;UINT8 Sum8=0;
  for(UINT32 I=0;I<Size;I+=4)Sum+=Le32(Code+I);
  for(int I=1;I<64;++I)Sum8+=H[I];
  assert(Wire[0]==0x5C && H[0]==0x06 && !memcmp(H+1,"NANO IC",7));
  assert(H[8]==0x40 && H[9]==0x81 && Le32(H+10)==0x2345 && Le32(H+14)==0x40 && Le32(H+18)==Size);
  assert(!memcmp(H+22,Image+21,4) && !memcmp(H+26,Image+25,4) && H[30]==Image[29] && H[31]==Image[30]);
  assert(Le32(H+49)==0 && Le32(H+53)==Sum && Le32(H+57)==0 && H[64]==Sum8);
  UINT8 Data[PIANO_POGO_DATA_WIRE_BYTES];UINT32 Offset=0,Packets=0;
  for(;;){
    UINT32 Bytes=PianoPogoMcuDataCommand(Code,Size,Offset,Data);
    if(!Bytes)break;
    assert(Data[0]==0x5C && Data[1]==0x07 && Be32(Data+2)==Offset && Be32(Data+6)==Bytes && Bytes<=503);
    assert(!memcmp(Data+10,Code+Offset,Bytes));
    for(UINT32 I=10+Bytes;I<sizeof(Data);++I)assert(Data[I]==0);
    Offset+=Bytes;++Packets;
  }
  assert(Offset==Size && Packets==(Size+502)/503);
}

int main(int argc,char **argv){
  // Synthetic image: header fields at the struct wn8030_bin_header offsets
  static UINT8 Image[64],Code[1012];
  memcpy(Image,"NANO IC",7);Image[21]=0x44;Image[24]=0x1F;Image[25]=0x94;Image[26]=0x12;Image[27]=0x06;Image[28]=0x32;
  Image[29]=6;Image[30]=2;Image[48]=(UINT8)sizeof(Code);Image[49]=(UINT8)(sizeof(Code)>>8);
  for(UINT32 I=0;I<sizeof(Code);++I)Code[I]=(UINT8)(I*13+5);
  CheckImage(Image,Code,sizeof(Code));
  UINT8 Wire[PIANO_POGO_HEADER_WIRE_BYTES];
  assert(PianoPogoMcuHeaderCommand(Image,Code,sizeof(Code)-4,Wire)==EFI_COMPROMISED_DATA); // size mismatch
  Image[0]='X';assert(PianoPogoMcuHeaderCommand(Image,Code,sizeof(Code),Wire)==EFI_COMPROMISED_DATA);Image[0]='N';
  // Runtime command: 0x4C, 32 00 4E 31 80 38 cmd 01 value, sum of bytes 2..8
  UINT8 Command[PIANO_POGO_COMMAND_WIRE_BYTES];PianoPogoMcuRuntimeCommand(PIANO_POGO_CMD_TOUCHPAD,1,Command);
  static const UINT8 Touchpad[]={0x4C,0x32,0x00,0x4E,0x31,0x80,0x38,0x21,0x01,0x01,0x5A};
  assert(!memcmp(Command,Touchpad,sizeof(Touchpad)));for(UINT32 I=sizeof(Touchpad);I<sizeof(Command);++I)assert(!Command[I]);
  PianoPogoMcuRuntimeCommand(PIANO_POGO_CMD_INDICATOR,PIANO_POGO_LED_CAPS_ON,Command);
  assert(Command[7]==0x2E && Command[9]==0xFD && Command[10]==(UINT8)(0x4E + 0x31 + 0x80 + 0x38 + 0x2E + 0x01 + 0xFD));
  UINT8 Boot[2];PianoPogoMcuBootCommand(0x05,Boot);assert(Boot[0]==0x5C && Boot[1]==0x05);
  UINT8 Frame[PIANO_POGO_REPORT_BYTES]={0x57,0,1};assert(PianoPogoMcuFrameHasReports(Frame));
  Frame[2]=0;assert(!PianoPogoMcuFrameHasReports(Frame));Frame[0]=0;Frame[2]=1;assert(!PianoPogoMcuFrameHasReports(Frame));
  if(argc>1){
    // The pinned Piano MCU image: header at 0x7FC0, RAM code at 0x8000
    FILE *F=fopen(argv[1],"rb");assert(F);static UINT8 File[0x20000];size_t N=fread(File,1,sizeof(File),F);fclose(F);
    assert(N>0x8000);UINT32 Size=Le32(File+0x7FC0+48);assert(0x8000+Size<=N);
    CheckImage(File+0x7FC0,File+0x8000,Size);
    printf("pinned image: %u code bytes, %u packets\n",Size,(Size+502)/503);
  }
  puts("Piano pogo MCU formats: header/checksums/503-byte packets/runtime commands/frame check PASS");
  return 0;
}
