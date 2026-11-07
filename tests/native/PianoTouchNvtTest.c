// SPDX-License-Identifier: BSD-2-Clause-Patent
// Prints the partition table PianoNvtParseImage finds in each image given;
// test_touch_nvt.py compares it with its own port of the driver parser
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "PianoTouchNvt.h"

int main(int argc,char **argv){
  STATIC CONST UINT8 Boe[6]={0x0A,0x12,0x34,0x32,0x65,0x03},Casc[6]={0xFF,0xFF,0xFF,0x32,0x65,0x03},Other[6]={0x0E,0,0,0x23,0x65,0x03};
  assert(PianoNvtMatchTrim((CONST UINT8 *)"\x0E\x01\x02\x32\x65\x03")==0 && PianoNvtMatchTrim(Boe)==1 && PianoNvtMatchTrim(Casc)==1 && PianoNvtMatchTrim(Other)==-1);
  PIANO_NVT_MAP Map;
  PianoNvtMemoryMap(0,TRUE,&Map);assert(Map.EventBuf==0x11C400 && Map.PollInfo==0x1093D8 && Map.TxAutoCopyEn==0x1FC925 && Map.BootRdy==0x1FB50D);
  PianoNvtMemoryMap(1,FALSE,&Map);assert(Map.EventBuf==0x125800 && Map.PollInfo==0 && Map.IlmDes==0x1FB528 && Map.GDlmChecksum==0x1FB504);
  STATIC UINT8 Small[0x1000];PIANO_NVT_IMAGE Image;
  assert(PianoNvtParseImage(Small,sizeof(Small),&Image)==EFI_INVALID_PARAMETER);
  for(int A=1;A<argc;++A){
    FILE *F=fopen(argv[A],"rb");assert(F);
    STATIC UINT8 Data[0x100000];size_t Bytes=fread(Data,1,sizeof(Data),F);fclose(F);
    assert(PianoNvtParseImage(Data,Bytes,&Image)==EFI_SUCCESS);
    printf("image %s need=%x count=%u cascade=%u fw_ver=%02x\n",argv[A],Image.NeedWriteSize,Image.Count,Image.Cascade2ndHeader,Image.FwVer);
    for(UINT32 L=0;L<Image.Count;++L)
      printf("part %u bin=%x sram=%x size=%x crc=%x\n",L,Image.Part[L].BinAddr,Image.Part[L].SramAddr,Image.Part[L].Size,Image.Part[L].Crc);
    // A broken end flag or version byte is refused
    STATIC UINT8 Copy[0x100000];memcpy(Copy,Data,Bytes);Copy[Image.NeedWriteSize-4096]^=1;
    assert(PianoNvtParseImage(Copy,Bytes,&Image)==EFI_COMPROMISED_DATA);
  }
  return 0;
}
