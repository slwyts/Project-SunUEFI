// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoFastboot.c"
static char replies[300][65];static unsigned calls;static int fail_send,fail_alloc;
static void *allocation;static size_t allocation_bytes;
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *A,UINTN N){return memset(A,0,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){
  if(fail_alloc)return NULL;
  assert(!allocation);allocation=calloc(1,N);allocation_bytes=N;return allocation;
}
VOID EFIAPI FreePool(VOID *A){
  if(A!=allocation){free(A);return;}
  for(size_t i=0;i<allocation_bytes;++i)assert(((unsigned char *)A)[i]==0);
  free(A);allocation=NULL;allocation_bytes=0;
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *A,UINTN N,UINT8 *Digest){return SHA256(A,N,Digest)!=NULL;}
static EFI_STATUS send_reply(VOID *C,CONST VOID *Data,UINTN N){
  assert(C==(void *)123);assert(N>=4 && N<=64);assert(calls<300);
  if(fail_send)return EFI_DEVICE_ERROR;
  memcpy(replies[calls],Data,N);replies[calls++][N]=0;return EFI_SUCCESS;
}
static EFI_STATUS log_reply(VOID *C,PIANO_FB_SEND Send){return Send(C,"INFOtest log",12);}
static void command(PIANO_FASTBOOT *S,const char *C,const char *Expected){
  calls=0;assert(PianoFastbootPacket(S,C,strlen(C))==EFI_SUCCESS);
  assert(calls>0);assert(strcmp(replies[calls-1],Expected)==0);
}
int main(void){
  PIANO_FASTBOOT S;
  assert(PianoFastbootInit(NULL,NULL,send_reply,NULL)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootInit(&S,(void *)123,NULL,NULL)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootInit(&S,(void *)123,send_reply,log_reply)==EFI_SUCCESS);
  command(&S,"getvar:version","OKAY0.4");
  command(&S,"getvar:version-malicious","FAILunknown variable");
  command(&S,"getvar:all","OKAY");assert(calls==5);
  const char *blocked[]={"flash:boot_a","erase:userdata","flashing unlock","oem unlock",
    "set_active:b","oem edl","reboot-edl","reboot-bootloader","oem poke 1 2",
    "flash:qupfw_a","update-super:super","snapshot-update:merge","format:userdata"};
  for(size_t i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i)
    command(&S,blocked[i],"FAILcommand disabled by RAM-only policy");
  const char *bad[]={"download:","download:0000000g","download:000000001",
    "download:+0000010","download:00000000","download:04000001","download:ffffffff"};
  for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i){calls=0;
    assert(PianoFastbootPacket(&S,bad[i],strlen(bad[i]))==EFI_SUCCESS);
    assert(calls==1 && strncmp(replies[0],"FAIL",4)==0 && !allocation && !S.Receiving);
  }
  char long_cmd[65];memset(long_cmd,'A',sizeof(long_cmd));calls=0;
  assert(PianoFastbootPacket(&S,long_cmd,sizeof(long_cmd))==EFI_SUCCESS);
  assert(strcmp(replies[0],"FAILcommand too long")==0);
  const char binary[]="getvar:version\0flash:boot";calls=0;
  assert(PianoFastbootPacket(&S,binary,sizeof(binary))==EFI_SUCCESS);
  assert(strcmp(replies[0],"FAILinvalid command bytes")==0);
  command(&S,"download:00000003","DATA00000003");assert(S.Receiving && !S.Complete);
  calls=0;assert(PianoFastbootPacket(&S,"a",1)==EFI_SUCCESS && !calls);
  assert(PianoFastbootPacket(&S,NULL,0)==EFI_SUCCESS && S.Received==1);
  assert(PianoFastbootPacket(&S,"bc",2)==EFI_SUCCESS && S.Complete && !S.Receiving);
  assert(strcmp(replies[0],"OKAY")==0 && memcmp(S.Download,"abc",3)==0);
  command(&S,"getvar:download-size","OKAY00000003");
  command(&S,"oem sha256","OKAY");assert(calls==3);
  assert(strcmp(replies[0],"INFOba7816bf8f01cfea414140de5dae2223")==0);
  assert(strcmp(replies[1],"INFOb00361a396177a9cb410ff61f20015ad")==0);
  command(&S,"boot","FAILRAM boot handoff not implemented in debug v1");
  command(&S,"oem discard","OKAY");assert(!allocation && !S.Complete);
  command(&S,"download:00000002","DATA00000002");calls=0;
  assert(PianoFastbootPacket(&S,"abc",3)==EFI_SUCCESS);
  assert(strcmp(replies[0],"FAILdownload overflow")==0 && !allocation && !S.Receiving);
  command(&S,"oem log","OKAY");assert(calls==2 && strcmp(replies[0],"INFOtest log")==0);
  command(&S,"reboot","OKAY");assert(S.RebootRequested);
  PianoFastbootReset(&S);command(&S,"continue","OKAY");assert(S.ExitRequested);
  PianoFastbootReset(&S);fail_send=1;
  assert(PianoFastbootPacket(&S,"reboot",6)==EFI_DEVICE_ERROR && !S.RebootRequested);
  assert(PianoFastbootPacket(&S,"download:00000003",17)==EFI_DEVICE_ERROR && !allocation);
  fail_send=0;fail_alloc=1;command(&S,"download:00000003","FAILnot enough RAM");
  PianoFastbootReset(&S);
  puts("Fastboot allowlist, binary validation, bounded RAM data, SHA256 and failed-send handling passed.");
  return 0;
}
