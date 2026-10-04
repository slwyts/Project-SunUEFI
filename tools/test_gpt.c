// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoGpt.c"
static void store32(UINT8 *p,UINT32 v){for(unsigned i=0;i<4;++i)p[i]=(UINT8)(v>>(8*i));}
static void store64(UINT8 *p,UINT64 v){store32(p,(UINT32)v);store32(p+4,(UINT32)(v>>32));}
static void crc_header(UINT8 *p){store32(p+16,0);store32(p+16,PianoGptCrc32(p,92));}
int main(void){
  assert(PianoGptCrc32("123456789",9)==0xcbf43926);
  UINT8 header[4096]={0},entries[16384]={0};PIANO_GPT_HEADER h;UINTN active;
  memcpy(header,"EFI PART",8);store32(header+8,0x10000);store32(header+12,92);
  store64(header+24,1);store64(header+32,0xffff);store64(header+40,6);store64(header+48,0xfffa);
  store64(header+72,2);store32(header+80,128);store32(header+84,128);
  entries[0]=1;store64(entries+32,6);store64(entries+40,0x100);
  store32(header+88,PianoGptCrc32(entries,sizeof(entries)));crc_header(header);
  assert(PianoGptParseHeader(header,4096,0xffff,4096,&h)==EFI_SUCCESS && h.ArrayBytes==16384);
  assert(PianoGptCheckEntries(entries,16384,&h,&active)==EFI_SUCCESS && active==1);
  entries[100]^=1;assert(PianoGptCheckEntries(entries,16384,&h,&active)==EFI_CRC_ERROR);entries[100]^=1;
  store64(entries+40,0xffff);h.ArrayCrc=PianoGptCrc32(entries,16384);
  assert(PianoGptCheckEntries(entries,16384,&h,&active)==EFI_COMPROMISED_DATA);
  header[40]^=1;assert(PianoGptParseHeader(header,4096,0xffff,4096,&h)==EFI_CRC_ERROR);header[40]^=1;
  store32(header+80,MAX_UINT32);crc_header(header);
  assert(PianoGptParseHeader(header,4096,0xffff,4096,&h)==EFI_UNSUPPORTED);
  store32(header+80,128);store64(header+72,0xfffffff0);crc_header(header);
  assert(PianoGptParseHeader(header,4096,0xffff,4096,&h)==EFI_COMPROMISED_DATA);
  header[0]=0;assert(PianoGptParseHeader(header,4096,0xffff,4096,&h)==EFI_NOT_FOUND);
  puts("GPT: standard CRC vector, header and full array CRC, capacity/range bounds, malformed lengths and occupied partition checks passed.");
}
