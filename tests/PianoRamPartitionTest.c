// Actual inventory C; ARM callback boundary substituted after identity gates.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/sha.h>
#undef NULL
#include "PianoRamPartition.h"
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiLoadedImageProtocolGuid={0};
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Path;
static unsigned Case,Methods,BankCalls,ImageCalls;static UINTN Base;static UINT8 *File;static VOID *Memory;static EFI_TPL Tpl;
static EFI_GUID Env={0x90A49AFD,0x422F,0x08AE,{0x96,0x11,0xE7,0x88,0xD3,0x80,0x48,0x45}};
static VOID *Interface(void){return (VOID *)(Base+0x102e0);}
static EFI_TPL EFIAPI Raise(EFI_TPL New){EFI_TPL Old=Tpl;Tpl=New;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *N,VOID **P){(void)G;(void)N;*P=Case==1?NULL:Interface();return Case==1?EFI_NOT_FOUND:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **P){(void)T;(void)G;(void)K;*N=1;*P=malloc(sizeof(**P));(*P)[0]=(VOID *)1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Protocol(EFI_HANDLE H,EFI_GUID *G,VOID **P){(void)G;assert(H==(VOID *)1);*P=&Loaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *P){if(Case==16)return EFI_WARN_STALE_DATA;free(P);return EFI_SUCCESS;}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Out){SHA256(P,N,Out);return TRUE;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 Type,UINTN I,VOID **Buffer,UINTN *Bytes){assert(!memcmp(G,&Env,sizeof(Env))&&Type==EFI_SECTION_PE32&&!I);*Bytes=0x13000;*Buffer=malloc(*Bytes);memcpy(*Buffer,File,*Bytes);if(Case==2)((UINT8 *)*Buffer)[300]^=1;return EFI_SUCCESS;}
EFI_STATUS PianoRamHostBanks(VOID *P,UINTN Fn,PIANO_RAM_BANK *B,UINT32 *N){
  assert(P==Interface()&&Fn==Base+0x22f4&&Tpl==TPL_APPLICATION);Methods++;BankCalls++;
  if(!B){*N=Case==8?33:Case==13?1:2;if(Case==9)return EFI_WARN_STALE_DATA;if(Case==15)((UINTN *)Interface())[3]=Base+1;return EFI_BUFFER_TOO_SMALL;}
  assert(*N<=32);
  if(Case==10)return EFI_DEVICE_ERROR;
  if(Case==11){(*N)--;return EFI_SUCCESS;}
  if(Case==24){(*N)++;return EFI_SUCCESS;}
  if(Case==13){B[0]=(PIANO_RAM_BANK){0x80000000,0x60000000};return EFI_SUCCESS;}
  B[0]=(PIANO_RAM_BANK){0x80000000,0x40000000};B[1]=(PIANO_RAM_BANK){0xa00000000ULL,0x80000000};
  if(Case==23)((UINTN *)Interface())[6]=Base+0x23d4;
  if(Case==25)B[1].AvailableLength=0;
  if(Case==12)B[1].Base=0x80000000;
  if(Case==18)B[1]=(PIANO_RAM_BANK){MAX_UINT64,2};
  return EFI_SUCCESS;
}
EFI_STATUS PianoRamHostPreloaded(VOID *P,UINTN Fn,PIANO_RAM_PRELOADED *B,UINT64 *N){
  assert(P==Interface()&&Fn==Base+0x2420&&Tpl==TPL_APPLICATION);Methods++;ImageCalls++;
  if(!B){*N=Case==14?MAX_UINT64:Case==22?0:1;return Case==22?EFI_SUCCESS:EFI_BUFFER_TOO_SMALL;}
  assert(*N==1);B[0]=(PIANO_RAM_PRELOADED){0xa8000000,0x10000000,7,0};
  if(Case==19)B[0].Size=0;
  if(Case==20){St.BootServices=NULL;return EFI_SUCCESS;}
  if(Case==26)((UINTN *)Interface())[3]=Base+1;
  return EFI_SUCCESS;
}
static void Relocate(void){
  for(UINTN O=0x12000;O<0x13000;){UINT32 Page,Bytes;memcpy(&Page,File+O,4);memcpy(&Bytes,File+O+4,4);if(!Bytes)break;
    for(UINTN I=O+8;I<O+Bytes;I+=2){UINT16 E;memcpy(&E,File+I,2);if(E>>12==10){UINT64 V;UINTN Target=Page+(E&4095);memcpy(&V,(UINT8 *)Memory+Target,8);V+=Base;memcpy((UINT8 *)Memory+Target,&V,8);}}O+=Bytes;}
}
static void Run(unsigned C){
  Case=C;assert(posix_memalign(&Memory,4096,0x13000)==0);Base=(UINTN)Memory;memcpy(Memory,File,0x13000);Relocate();
  gBS=&Bs;gST=&St;Bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.LocateHandleBuffer=Handles;Bs.HandleProtocol=Protocol;Bs.FreePool=Free;St.BootServices=&Bs;Tpl=TPL_APPLICATION;
  Loaded.ImageBase=Memory;Loaded.ImageSize=0x13000;Loaded.FilePath=&Path.Header;Path.Header.Type=MEDIA_DEVICE_PATH;Path.Header.SubType=MEDIA_PIWG_FW_FILE_DP;Path.Header.Length[0]=sizeof(Path);Path.FvFileName=Env;
  if(C==3)Loaded.ImageSize--;
  if(C==4)Path.FvFileName.Data1++;
  if(C==5)((UINT8 *)Memory)[0x2300]^=1;
  if(C==6)((UINT64 *)Interface())[0]++;
  if(C==7)((UINTN *)Interface())[6]=Base+0x23d4; // old wrong preloaded offset
  if(C==17)Tpl=TPL_CALLBACK;
  PIANO_RAM_PARTITION_REPORT R;EFI_STATUS S=PianoRamPartitionInventory(C!=0,&R);
  assert(!R.OwnershipVerified);
  if(S!=EFI_SUCCESS)assert(!R.DataValid);
  if(C==0)assert(S==EFI_SUCCESS&&R.AbiVerified&&R.IdentityVerified&&!R.FetchAttempted&&!Methods&&!R.DataValid);
  else if(C==13)assert(S==EFI_SUCCESS&&R.PotentialFallback&&R.BankCount==1&&R.DataValid);
  else if(C==16||C==20){assert(S!=EFI_SUCCESS&&R.Retained);unsigned Before=Methods;assert(PianoRamPartitionInventory(TRUE,&R)==EFI_NOT_READY&&Methods==Before);}
  else if(C==21)assert(S==EFI_SUCCESS&&R.BankCount==2&&R.PreloadedCount==1&&BankCalls==2&&ImageCalls==2&&R.DataValid);
  else if(C==22)assert(S==EFI_SUCCESS&&R.BankCount==2&&!R.PreloadedCount&&ImageCalls==1&&R.DataValid);
  else assert(S!=EFI_SUCCESS);
  if(S!=EFI_SUCCESS&&R.FetchAttempted){unsigned Before=Methods;assert(PianoRamPartitionInventory(TRUE,&R)==EFI_NOT_READY&&Methods==Before&&!R.DataValid&&!R.OwnershipVerified);}
  if(C>=1&&C<=7)assert(!Methods);
  if(C==17)assert(!Methods);
  free(Memory);
}
int main(int argc,char **argv){assert(argc==2);FILE *F=fopen(argv[1],"rb");assert(F);File=malloc(0x13000);assert(fread(File,1,0x13000,F)==0x13000);fclose(F);
  for(unsigned C=0;C<27;C++){pid_t P=fork();assert(P>=0);if(!P){Run(C);_exit(0);}int S;waitpid(P,&S,0);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"ram case %u failed\n",C);return 1;}}
  puts("Actual RamPartition inventory: 27 fork identity/relocation/native ABI/default-off/bounded data/fallback/warnings/EBS/TPL cases passed; ARM callback boundary substituted, no device");free(File);return 0;
}
