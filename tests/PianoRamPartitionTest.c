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
static unsigned Case,Methods,BankCalls,ImageCalls,NativeHashes;
static UINT8 Raw402[2328];static PIANO_RAM_BANK CapturedBanks[32];static UINT32 CapturedCount,CapturedPositive;static UINTN Base;static UINT8 *File;static VOID *Memory;static EFI_TPL Tpl;
static EFI_GUID Env={0x90A49AFD,0x422F,0x08AE,{0x96,0x11,0xE7,0x88,0xD3,0x80,0x48,0x45}};
static VOID *Interface(void){return (VOID *)(Base+0x102e0);}
static EFI_TPL EFIAPI Raise(EFI_TPL New){EFI_TPL Old=Tpl;Tpl=New;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *N,VOID **P){(void)G;(void)N;*P=Case==1?NULL:Interface();return Case==1?EFI_NOT_FOUND:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **P){(void)T;(void)G;(void)K;*N=1;*P=malloc(sizeof(**P));(*P)[0]=(VOID *)1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Protocol(EFI_HANDLE H,EFI_GUID *G,VOID **P){(void)G;assert(H==(VOID *)1);*P=&Loaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *P){if(Case==16)return EFI_WARN_STALE_DATA;free(P);return EFI_SUCCESS;}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Out){++NativeHashes;SHA256(P,N,Out);return TRUE;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 Type,UINTN I,VOID **Buffer,UINTN *Bytes){assert(!memcmp(G,&Env,sizeof(Env))&&Type==EFI_SECTION_PE32&&!I);*Bytes=0x13000;*Buffer=malloc(*Bytes);memcpy(*Buffer,File,*Bytes);if(Case==2)((UINT8 *)*Buffer)[300]^=1;return EFI_SUCCESS;}
EFI_STATUS PianoRamHostBanks(VOID *P,UINTN Fn,PIANO_RAM_BANK *B,UINT32 *N){
  assert(P==Interface()&&Fn==Base+0x22f4&&Tpl==TPL_APPLICATION&&NativeHashes&&Loaded.ImageSize==0x13000);Methods++;BankCalls++;
  if(!B){*N=Case==8?33:Case==13?1:Case==30?CapturedCount:2;if(Case==9)return EFI_WARN_STALE_DATA;if(Case==15)((UINTN *)Interface())[3]=Base+1;return EFI_BUFFER_TOO_SMALL;}
  assert(*N<=32);
  if(Case==10)return EFI_DEVICE_ERROR;
  if(Case==11){(*N)--;return EFI_SUCCESS;}
  if(Case==24){(*N)++;return EFI_SUCCESS;}
  if(Case==30){assert(*N==CapturedCount);memcpy(B,CapturedBanks,CapturedCount*sizeof(*B));return EFI_SUCCESS;}
  if(Case==13){B[0]=(PIANO_RAM_BANK){0x80000000,0x60000000};return EFI_SUCCESS;}
  B[0]=(PIANO_RAM_BANK){0x80000000,0x40000000};B[1]=(PIANO_RAM_BANK){0xa00000000ULL,0x80000000};
  if(Case==23)((UINTN *)Interface())[6]=Base+0x23d4;
  if(Case==25)B[1].AvailableLength=0;
  if(Case==27)B[1]=(PIANO_RAM_BANK){B[0].Base+4096,0}; // empty observation lies inside the positive range
  if(Case==28){B[0]=(PIANO_RAM_BANK){0,0};B[1]=(PIANO_RAM_BANK){MAX_UINT64,0};}
  if(Case==29)B[1]=(PIANO_RAM_BANK){MAX_UINT64,0};
  if(Case==12)B[1].Base=0x80000000;
  if(Case==18)B[1]=(PIANO_RAM_BANK){MAX_UINT64,2};
  return EFI_SUCCESS;
}
EFI_STATUS PianoRamHostPreloaded(VOID *P,UINTN Fn,PIANO_RAM_PRELOADED *B,UINT64 *N){
  assert(P==Interface()&&Fn==Base+0x2420&&Tpl==TPL_APPLICATION);Methods++;ImageCalls++;
  if(!B){*N=Case==14?MAX_UINT64:(Case==22||Case==28||Case==30)?0:1;return (Case==22||Case==28||Case==30)?EFI_SUCCESS:EFI_BUFFER_TOO_SMALL;}
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
  else if(C==25||C==27||C==28||C==29){assert(S==EFI_SUCCESS&&R.DataValid&&R.IdentityVerified&&R.AbiVerified&&R.BankCount==2&&BankCalls==2&&!R.PotentialFallback);
    if(C==28)assert(!R.Banks[0].AvailableLength&&!R.Banks[1].AvailableLength&&!R.PreloadedCount&&ImageCalls==1);
    else assert(!R.Banks[1].AvailableLength&&R.PreloadedCount==1&&ImageCalls==2);
    if(C==27)assert(R.Banks[1].Base==R.Banks[0].Base+4096);
    if(C==29)assert(R.Banks[1].Base==MAX_UINT64);
  }
  else if(C==30){assert(S==EFI_SUCCESS&&R.IdentityVerified&&R.AbiVerified&&R.DataValid&&R.FetchAttempted&&!R.PotentialFallback&&R.BankCount==12&&!R.PreloadedCount&&BankCalls==2&&ImageCalls==1);
    UINT32 Positive=0;for(UINT32 I=0;I<R.BankCount;++I){assert(R.Banks[I].Base==CapturedBanks[I].Base&&R.Banks[I].AvailableLength==CapturedBanks[I].AvailableLength);Positive+=R.Banks[I].AvailableLength!=0;}
    assert(Positive==11&&CapturedPositive==11&&R.Banks[3].Base==0xd8600000&&R.Banks[3].AvailableLength==0);
    assert(R.Banks[9].Base==0x8b5700000ULL&&R.Banks[9].AvailableLength==0x1e00000);
    assert(R.Banks[10].Base==0x82600000&&R.Banks[10].AvailableLength==0x55a00000);
    assert(R.Banks[11].Base==0x81200000&&R.Banks[11].AvailableLength==0x40000);
  }
  else if(C==21)assert(S==EFI_SUCCESS&&R.BankCount==2&&R.PreloadedCount==1&&BankCalls==2&&ImageCalls==2&&R.DataValid);
  else if(C==22)assert(S==EFI_SUCCESS&&R.BankCount==2&&!R.PreloadedCount&&ImageCalls==1&&R.DataValid);
  else assert(S!=EFI_SUCCESS);
  if(S!=EFI_SUCCESS&&R.FetchAttempted){unsigned Before=Methods;assert(PianoRamPartitionInventory(TRUE,&R)==EFI_NOT_READY&&Methods==Before&&!R.DataValid&&!R.OwnershipVerified);}
  if(C>=1&&C<=7)assert(!Methods);
  if(C==17)assert(!Methods);
  free(Memory);
}
static UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
static VOID Capture(CONST CHAR8 *Path){
 FILE *F=fopen(Path,"rb");assert(F&&fread(Raw402,1,sizeof(Raw402),F)==sizeof(Raw402)&&fgetc(F)==EOF);fclose(F);
 static CONST UINT8 Pin[32]={0x16,0xae,0xd6,0xa8,0x15,0x30,0x9a,0xf4,0x10,0x9f,0x34,0x00,0x0f,0x43,0xde,0x00,0x58,0xd3,0x10,0xe2,0x93,0xb2,0xae,0xcc,0x29,0xc6,0xd4,0xd9,0xa7,0xed,0x82,0x8c};
 UINT8 Hash[32];SHA256(Raw402,sizeof(Raw402),Hash);assert(!memcmp(Hash,Pin,32));
 UINT32 Crc=0xffffffffU;for(UINTN I=0;I<sizeof(Raw402);++I){Crc^=Raw402[I];for(UINTN J=0;J<8;++J)Crc=(Crc>>1)^((Crc&1)?0xedb88320U:0);}assert(~Crc==0x7c271814);
 assert(Le32(Raw402)==0x9da5e0a8&&Le32(Raw402+4)==0xaf9ec4e2&&Le32(Raw402+8)==3&&Le32(Raw402+16)==15);
 for(UINTN I=0;I<15;++I){CONST UINT8 *E=Raw402+24+72*I;if(Le32(E+0x24)!=14||Le32(E+0x2c)!=1)continue;
  assert(CapturedCount<32);CapturedBanks[CapturedCount++]=(PIANO_RAM_BANK){Le64(E+0x10),Le64(E+0x40)};CapturedPositive+=Le64(E+0x40)!=0;
 }
 assert(CapturedCount==12&&CapturedPositive==11);
}
int main(int argc,char **argv){assert(argc==2||argc==3);FILE *F=fopen(argv[1],"rb");assert(F);File=malloc(0x13000);assert(fread(File,1,0x13000,F)==0x13000);fclose(F);if(argc==3)Capture(argv[2]);
 unsigned Count=argc==3?31:30;
 for(unsigned C=0;C<Count;C++){pid_t P=fork();assert(P>=0);if(!P){Run(C);_exit(0);}int S;waitpid(P,&S,0);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"ram case %u failed\n",C);return 1;}}
 printf("Actual RamPartition inventory: %u fork identity/relocation/native ABI/default-off/data/warning/cleanup cases; zero-current and all-empty observations preserved, ownership FALSE; ARM boundary fixture, no device\n",Count);free(File);return 0;
}
