// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoFastboot.c"
static char replies[300][65];static unsigned calls;static int fail_send,fail_alloc;
static void *allocation;static size_t allocation_bytes;
static struct {void *pointer;size_t bytes;} pools[16];
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *A,UINTN N){return memset(A,0,N);}
#if PIANO_USB_RAM_BOOT
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
VOID *EFIAPI AllocateZeroPool(UINTN N){
  if(fail_alloc)return NULL;
  for(unsigned i=0;i<16;++i)if(!pools[i].pointer){pools[i].pointer=calloc(1,N);pools[i].bytes=N;
    allocation=pools[i].pointer;allocation_bytes=N;return allocation;}
  assert(0);return NULL;
}
VOID EFIAPI FreePool(VOID *A){
  for(unsigned i=0;i<16;++i)if(A==pools[i].pointer) {
    for(size_t j=0;j<pools[i].bytes;++j)assert(((unsigned char *)A)[j]==0);
    free(A);pools[i].pointer=NULL;allocation=NULL;allocation_bytes=0;
    for(unsigned j=0;j<16;++j)if(pools[j].pointer){allocation=pools[j].pointer;allocation_bytes=pools[j].bytes;}
    return;
  }
  free(A);
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *A,UINTN N,UINT8 *Digest){return SHA256(A,N,Digest)!=NULL;}
static EFI_STATUS send_reply(VOID *C,CONST VOID *Data,UINTN N){
  assert(C==(void *)123);assert(N>0 && N<=64);assert(calls<300);
  if(fail_send)return EFI_DEVICE_ERROR;
  memcpy(replies[calls],Data,N);replies[calls++][N]=0;return EFI_SUCCESS;
}
static EFI_STATUS log_reply(VOID *C,PIANO_FB_SEND Send){return Send(C,"INFOtest log",12);}
static void command(PIANO_FASTBOOT *S,const char *C,const char *Expected){
  calls=0;assert(PianoFastbootPacket(S,C,strlen(C))==EFI_SUCCESS);
  assert(calls>0);assert(strcmp(replies[calls-1],Expected)==0);
}
static unsigned storage_queries,boot_queries,diagnostic_calls;
static EFI_STATUS diagnostic_storage_ready(VOID *Context){assert(Context==(VOID *)456);++storage_queries;return EFI_SUCCESS;}
static EFI_STATUS diagnostic_storage_info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info){
  assert(Context==(VOID *)456);++storage_queries;
  memcpy(Info->Name,Name,strlen(Name)+1);Info->Bytes=4096;Info->BlockSize=512;Info->ReadOnly=TRUE;Info->Token=(VOID *)789;return EFI_SUCCESS;
}
static EFI_STATUS diagnostic_storage_read(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){
  (VOID)Context;(VOID)Info;(VOID)Lba;(VOID)Bytes;(VOID)Buffer;assert(!"diagnostic mode must not read partitions");return EFI_DEVICE_ERROR;
}
#if PIANO_USB_RAM_BOOT
static EFI_STATUS diagnostic_boot_ready(VOID *Context){assert(Context==(VOID *)456);++boot_queries;return EFI_SUCCESS;}
static EFI_STATUS diagnostic_boot_validate(VOID *Context,CONST PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View){
  (VOID)Context;(VOID)Source;(VOID)View;assert(!"diagnostic mode must not validate a boot payload");return EFI_DEVICE_ERROR;
}
static EFI_STATUS diagnostic_boot_take(VOID *Context,PIANO_FASTBOOT *Source,CONST PIANO_FB_BOOT_VIEW *View,VOID **Token){
  (VOID)Context;(VOID)Source;(VOID)View;(VOID)Token;assert(!"diagnostic mode must not hand off a boot payload");return EFI_DEVICE_ERROR;
}
#endif
static EFI_STATUS diagnostic_reply(VOID *Context,PIANO_FASTBOOT *State,CONST CHAR8 *Command){
  assert(Context==(VOID *)123 && State->DiagnosticsOnly);
  assert(!strcmp(Command,"oem status") || !strcmp(Command,"oem ramlog") || !strcmp(Command,"oem screenshot"));
  ++diagnostic_calls;
  if(strcmp(Command,"oem status"))assert(PianoFastbootStageCopy(State,"frozen",6)==EFI_SUCCESS);
  return Reply(State,"OKAY");
}
static void diagnostics_only(void){
  PIANO_FASTBOOT S;
  assert(PianoFastbootInit(&S,(VOID *)123,send_reply,log_reply)==EFI_SUCCESS);
  PIANO_FB_STORAGE Storage={.Context=(VOID *)456,.Ready=diagnostic_storage_ready,.Info=diagnostic_storage_info,.ReadBlocks=diagnostic_storage_read};
  assert(PianoFastbootSetStorage(&S,&Storage)==EFI_SUCCESS);
  command(&S,"getvar:partition-size:xbl_config_a","OKAY0x0000000000001000");assert(storage_queries==2);
#if PIANO_USB_RAM_BOOT
  PIANO_FB_BOOT Boot={.Context=(VOID *)456,.MaxImageBytes=4096,.Ready=diagnostic_boot_ready,.Validate=diagnostic_boot_validate,.TakeAfterAck=diagnostic_boot_take};
  assert(PianoFastbootSetBoot(&S,&Boot)==EFI_SUCCESS);
  command(&S,"getvar:SunUEFI:ram-boot","OKAYenabled");assert(boot_queries==1);
#endif
  command(&S,"download:00000003","DATA00000003");
  assert(PianoFastbootPacket(&S,"abc",3)==EFI_SUCCESS && S.Complete && S.UploadBorrowed);
  assert(PianoFastbootStageCopy(&S,"other",5)==EFI_SUCCESS);
  assert(PianoFastbootRestrictToDiagnostics(NULL)==EFI_INVALID_PARAMETER);
  for(unsigned i=0;i<3;++i){
    S.BootPreparing=i==0;S.BootPending=i==1;S.BootTransferFrozen=i==2;
    assert(PianoFastbootRestrictToDiagnostics(&S)==EFI_NOT_READY && !S.DiagnosticsOnly && S.Download && S.Upload);
  }
  S.BootPreparing=S.BootPending=S.BootTransferFrozen=FALSE;
  S.RebootRequested=S.ExitRequested=TRUE;
  assert(PianoFastbootRestrictToDiagnostics(&S)==EFI_SUCCESS && S.DiagnosticsOnly && !allocation);
  assert(!S.Download && !S.Upload && !S.Receiving && !S.Complete && !S.RebootRequested && !S.ExitRequested);
  S.Diagnostic=diagnostic_reply;
  command(&S,"getvar:version","OKAY0.4");
  command(&S,"getvar:max-download-size","OKAY0x00000000");
  command(&S,"getvar:max-fetch-size","FAILstorage backend not ready");
  command(&S,"getvar:partition-size:xbl_config_a","FAILstorage backend not ready");
  command(&S,"getvar:SunUEFI:ram-boot","OKAYdisabled");
  command(&S,"getvar:all","OKAY");assert(calls==5 && !strcmp(replies[3],"INFOmax-download-size:0x00000000"));
  unsigned StorageQueries=storage_queries,BootQueries=boot_queries;
  const char *Blocked[]={"download:00000003","fetch:xbl_config_a:0x0:0x1","boot","reboot","continue", "oem setup","oem shell","oem simpleinit","oem boot-stable","oem sha256","oem discard","oem log","flash:boot_a"};
  for(unsigned i=0;i<sizeof(Blocked)/sizeof(Blocked[0]);++i)
    command(&S,Blocked[i],"FAILonly diagnostics available after boot failure");
  assert(storage_queries==StorageQueries && boot_queries==BootQueries && !diagnostic_calls && !allocation);
  assert(storage_queries==2 && boot_queries==(PIANO_USB_RAM_BOOT?1U:0U));
  command(&S,"oem status","OKAY");command(&S,"oem ramlog","OKAY");
  UINT8 *Upload=S.Upload;
  assert(PianoFastbootRestrictToDiagnostics(&S)==EFI_SUCCESS && S.Upload==Upload);
  command(&S,"upload","OKAY");assert(calls==3 && !strcmp(replies[1],"frozen"));
  command(&S,"oem screenshot","OKAY");command(&S,"upload","OKAY");assert(diagnostic_calls==3);
  PianoFastbootReset(&S);assert(S.DiagnosticsOnly && !S.Upload && !allocation);
  command(&S,"download:00000003","FAILonly diagnostics available after boot failure");
  command(&S,"reboot","FAILonly diagnostics available after boot failure");
  assert(!S.RebootRequested && !S.ExitRequested && !S.Receiving);
  // A partial host download is discarded before command parsing resumes.
  assert(PianoFastbootInit(&S,(VOID *)123,send_reply,NULL)==EFI_SUCCESS);
  command(&S,"download:00000003","DATA00000003");
  assert(PianoFastbootPacket(&S,"a",1)==EFI_SUCCESS && S.Receiving);
  assert(PianoFastbootRestrictToDiagnostics(&S)==EFI_SUCCESS && !S.Receiving && !allocation);
  command(&S,"getvar:version","OKAY0.4");
}
int main(void){
  PIANO_FASTBOOT S;
  assert(PianoFastbootInit(NULL,NULL,send_reply,NULL)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootInit(&S,(void *)123,NULL,NULL)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootInit(&S,(void *)123,send_reply,log_reply)==EFI_SUCCESS);
  command(&S,"getvar:version","OKAY0.4");
  command(&S,"getvar:SunUEFI:ram-boot","OKAYdisabled");
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
  command(&S,"upload","OKAY");assert(calls==3 && !strcmp(replies[0],"DATA00000003") && !memcmp(replies[1],"abc",3));
  assert(PianoFastbootStageCopy(&S,"frozen",6)==EFI_SUCCESS && !S.UploadBorrowed);
  command(&S,"upload","OKAY");assert(calls==3 && !strcmp(replies[0],"DATA00000006") && !memcmp(replies[1],"frozen",6));
  command(&S,"oem sha256","OKAY");assert(calls==3);
  assert(strcmp(replies[0],"INFOba7816bf8f01cfea414140de5dae2223")==0);
  assert(strcmp(replies[1],"INFOb00361a396177a9cb410ff61f20015ad")==0);
#if PIANO_USB_RAM_BOOT
  command(&S,"boot","FAILRAM boot backend unavailable");
#else
  command(&S,"boot","FAILRAM boot handoff not implemented in debug v1");
#endif
  command(&S,"oem discard","OKAY");assert(!allocation && !S.Complete);
  command(&S,"upload","FAILno staged RAM payload");
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
  fail_alloc=0;diagnostics_only();
  puts("Fastboot allowlist, binary validation, bounded RAM data, frozen upload, zero/free, SHA256 and failed-send handling passed.");
  return 0;
}
