// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUfsDmaLayout.c"
#include <IndustryStandard/Ufs.h>
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
int main(void){
  UINT8 trd[32],ucd[PIANO_UFS_UCD_BYTES];
  assert(PianoUfsBuildNop(trd,sizeof(trd),ucd,sizeof(ucd),0x40001000,7)==EFI_SUCCESS);
  assert(ReadLe32(trd)==0x11000000 && ReadLe32(trd+8)==15 && ReadLe32(trd+16)==0x40001000);
  assert(ReadLe32(trd+24)==0x00100008 && ucd[0]==0 && ucd[3]==7);
  assert(PianoUfsCheckNop(trd,ucd,7)==EFI_DEVICE_ERROR);
  Le32(trd+8,0);ucd[64]=0x20;ucd[67]=7;
  assert(PianoUfsCheckNop(trd,ucd,7)==EFI_SUCCESS);
  ucd[70]=1;assert(PianoUfsCheckNop(trd,ucd,7)==EFI_DEVICE_ERROR);ucd[70]=0;
  assert(PianoUfsCheckNop(trd,ucd,8)==EFI_DEVICE_ERROR);
  assert(PianoUfsBuildReadDescriptor(trd,32,ucd,sizeof(ucd),0x40001000,8,0,0,255)==EFI_SUCCESS);
  assert(ucd[0]==0x16 && ucd[3]==8 && ucd[5]==1 && ucd[12]==1 && ucd[13]==0 && ucd[18]==0 && ucd[19]==255);
  assert(ReadLe32(trd+24)==0x00100048);
  // Compare against the pinned EDK2 packed wire ABI, rather than checking
  // only offsets repeated from our implementation.
  UTP_QUERY_REQ_UPIU reference={0};reference.TransCode=0x16;reference.TaskTag=8;
  reference.QueryFunc=QUERY_FUNC_STD_READ_REQ;reference.Tsf.Opcode=UtpQueryFuncOpcodeRdDesc;
  reference.Tsf.Length=0xff00;
  assert(sizeof(reference)==32 && memcmp(ucd,&reference,sizeof(reference))==0);
  assert(PianoUfsBuildReadPowerMode(trd,32,ucd,1024,0x40001000,6)==EFI_SUCCESS);
  assert(ucd[12]==3 && ucd[13]==2 && ucd[5]==1 && ucd[18]==0 && ucd[19]==0 && ReadLe32(trd+24)==0x00100008);
  assert(PianoUfsBuildReadWriteProtectFlag(trd,32,ucd,1024,0x40001000,30,3)==EFI_SUCCESS);
  assert(ucd[0]==0x16 && ucd[3]==30 && ucd[5]==1 && ucd[12]==5 && ucd[13]==3 && ReadLe32(trd+28)==0);
  assert(PianoUfsBuildReadWriteProtectFlag(trd,32,ucd,1024,0x40001000,30,1)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBuildResumeActive(trd,32,ucd,1024,0x40001000,7)==EFI_SUCCESS);
  assert(ReadLe32(trd)==0x11000000 && ReadLe32(trd+28)==0 && ucd[1]==0 && ucd[2]==0xd0 && ReadBe32(ucd+12)==0);
  assert(ucd[16]==0x1b && ucd[20]==0x10);
  assert(PianoUfsBuildReadDescriptor(trd,32,ucd,sizeof(ucd),0x40001000,8,1,0,255)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBuildReadDescriptor(trd,32,ucd,sizeof(ucd),0x40001000,8,0,1,255)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBuildNop(trd,32,ucd,sizeof(ucd),0x40001001,7)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBuildReadCommand(trd,32,ucd,1024,0x40001000,0x40002000,4096,9,0,PianoUfsReportLuns,0,0)==EFI_SUCCESS);
  assert(ReadLe32(trd)==0x15000000 && ReadLe32(trd+28)==0x00400001);
  assert(ucd[0]==1 && ucd[1]==0x40 && ucd[3]==9 && ucd[16]==0xa0 && ucd[23]==0 && ucd[24]==0x10);
  assert(ReadLe32(ucd+256)==0x40002000 && ReadLe32(ucd+268)==4095);
  assert(PianoUfsBuildReadCommand(trd,32,ucd,1024,0x40001000,0x40002000,32,10,0,PianoUfsReadCapacity16,0,0)==EFI_SUCCESS);
  assert(ucd[16]==0x9e && ucd[17]==0x10 && ucd[29]==32);
  assert(PianoUfsBuildReadCommand(trd,32,ucd,1024,0x40001000,0x40002000,4096,11,0,PianoUfsReadLba10,1,1)==EFI_SUCCESS);
  assert(ucd[16]==0x28 && ucd[21]==1 && ucd[24]==1);
  assert(PianoUfsBuildReadCommand(trd,32,ucd,1024,0x40001000,0x40002000,4096,11,8,PianoUfsReadLba10,0,1)==EFI_INVALID_PARAMETER);
  UINT32 transferred;UINT8 *r=ucd+64;Le32(trd+8,0);ZeroMem(r,64);r[0]=0x21;r[3]=11;
  assert(PianoUfsCheckReadResponse(trd,ucd,11,4096,&transferred)==EFI_SUCCESS && transferred==4096);
  r[1]=0x20;Be32(r+12,4000);
  assert(PianoUfsCheckReadResponse(trd,ucd,11,4096,&transferred)==EFI_SUCCESS && transferred==96);
  r[1]=0;assert(PianoUfsCheckReadResponse(trd,ucd,11,4096,&transferred)==EFI_COMPROMISED_DATA);
  r[7]=2;assert(PianoUfsCheckReadResponse(trd,ucd,11,4096,&transferred)==EFI_DEVICE_ERROR);
  UINT8 data[4096],luns[8];UINTN count;ZeroMem(data,sizeof(data));Be32(data,24);
  data[9]=0;data[17]=2;data[24]=0xc1;data[25]=0x81;
  assert(PianoUfsParseLuns(data,sizeof(data),luns,&count)==EFI_SUCCESS && count==2 && luns[1]==2);
  Be32(data,4096);assert(PianoUfsParseLuns(data,4096,luns,&count)==EFI_COMPROMISED_DATA);
  Be32(data,16);data[17]=0;assert(PianoUfsParseLuns(data,4096,luns,&count)==EFI_COMPROMISED_DATA);
  UINT64 last;UINT32 block;ZeroMem(data,32);Be32(data+4,0x3000000);Be32(data+8,4096);
  assert(PianoUfsParseCapacity(data,32,&last,&block)==EFI_SUCCESS && last==0x3000000 && block==4096);
  Be32(data+8,1024);assert(PianoUfsParseCapacity(data,32,&last,&block)==EFI_UNSUPPORTED);
  assert(PianoUfsBuildReadCommand(trd,32,ucd,1024,0x40001000,0x40002000,4096,12,4,PianoUfsModeSense10,0,0)==EFI_SUCCESS);
  assert(ucd[16]==0x5a && ucd[17]==8 && ucd[18]==8 && ucd[23]==0x10 && ucd[24]==0);
  BOOLEAN wp,fua,wce,rcd;ZeroMem(data,32);data[1]=26;data[3]=0x90;data[8]=8;data[9]=0x12;data[10]=5;
  assert(PianoUfsParseCacheMode(data,28,&wp,&fua,&wce,&rcd)==EFI_SUCCESS && wp && fua && wce && rcd);
  assert(PianoUfsParseCacheMode(data,27,&wp,&fua,&wce,&rcd)==EFI_COMPROMISED_DATA && !fua);
  data[7]=24;assert(PianoUfsParseCacheMode(data,28,&wp,&fua,&wce,&rcd)==EFI_COMPROMISED_DATA);data[7]=0;
  data[9]=0x13;assert(PianoUfsParseCacheMode(data,28,&wp,&fua,&wce,&rcd)==EFI_COMPROMISED_DATA);data[9]=0x12;
  data[8]=0x48;assert(PianoUfsParseCacheMode(data,28,&wp,&fua,&wce,&rcd)==EFI_UNSUPPORTED);
  assert(PianoUfsParseCacheMode(data,7,&wp,&fua,&wce,&rcd)==EFI_COMPROMISED_DATA);
  puts("UTRD/UCD/UPIU layouts: exact byte order, offsets, NOP IN tag/OCS checks and read-descriptor-only query construction passed.");return 0;
}
