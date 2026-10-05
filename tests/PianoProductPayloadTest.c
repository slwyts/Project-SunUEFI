// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#define PIANO_PRODUCT_PAYLOAD_HOST_TEST 1
#define MDEPKG_NDEBUG 1
#include "../bootprofiles/uefi-app/PianoProductPayload.c"
#include "../bootprofiles/uefi-app/PianoFastbootBoot.c"
static UINT64 handoff[2];UINTN PianoProductHostHandoffAddress;
static UINT8 bank[16384];static UINT8 start_be[8],end_be[8];
static EFI_MEMORY_REGION_DESCRIPTOR regions[4];static UINT8 region_count;
static BOOLEAN bad_fdt,hash_failure;
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *A,UINTN N){return memset(A,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *D){return !hash_failure && SHA256(P,N,D)!=NULL;}
VOID GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **Map,UINT8 *Count){*Map=regions;*Count=region_count;}
INT32 EFIAPI FdtCheckHeader(CONST VOID *F){assert(F==bank);return bad_fdt?-1:0;}
UINT32 EFIAPI SwapBytes32(UINT32 V){return __builtin_bswap32(V);}
UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
INT32 EFIAPI FdtPathOffset(CONST VOID *F,CONST CHAR8 *P){assert(F==bank && !strcmp(P,"/chosen"));return 1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *F,INT32 N,CONST CHAR8 *P,INT32 *L){assert(F==bank && N==1);*L=8;return !strcmp(P,"linux,initrd-start")?start_be:end_be;}
static void be(UINT8 *P,UINT64 V){for(unsigned I=0;I<8;I++)P[I]=(UINT8)(V>>(56-8*I));}
static void init(const char *file){
  FILE *f=fopen(file,"rb");assert(f);memset(bank,0,sizeof(bank));assert(fread(bank+4096+64,1,4096,f)==4096);assert(fgetc(f)==EOF);fclose(f);
  bank[7]=64; // Actual FdtTotalSize macro reads the big-endian header word.
  APP_HEADER *h=(APP_HEADER *)(bank+4096);memcpy(h->Magic,mMagic,16);h->Version=1;h->HeaderBytes=64;h->AppBytes=4096;memcpy(h->AppHash,mPianoProductSimpleInitSha256,32);
  memset(regions,0,sizeof(regions));region_count=2;strcpy(regions[0].Name,"BootHandoff");regions[0].Address=(UINTN)handoff;regions[0].Length=sizeof(handoff);
  strcpy(regions[1].Name,"Kernel");regions[1].Address=(UINTN)bank;regions[1].Length=sizeof(bank);
  handoff[0]=0x534E554546494448ULL;handoff[1]=(UINTN)bank;PianoProductHostHandoffAddress=(UINTN)handoff;
  be(start_be,(UINTN)(bank+4096));be(end_be,(UINTN)(bank+4096+64+4096));bad_fdt=hash_failure=FALSE;
}
int main(int argc,char **argv){
  assert(argc==2);init(argv[1]);PIANO_PRODUCT_PAYLOAD_VIEW v={0},other={0};
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SUCCESS && v.Image==bank+4160 && v.Bytes==4096 && v.Lease);
  assert(PianoProductValidateSimpleInit(&v)==EFI_SUCCESS);
  CONST VOID *fdt=NULL;assert(PianoProductPayloadGetFdt(&v,&fdt)==EFI_SUCCESS && fdt==bank);
  assert(PianoProductAcquireSimpleInit(&other)==EFI_ALREADY_STARTED && !other.Image && !other.Lease);
  other=v;other.Lease=(VOID *)99;assert(PianoProductValidateSimpleInit(&other)==EFI_ACCESS_DENIED);
  bank[4160+700]^=1;assert(PianoProductValidateSimpleInit(&v)==EFI_SECURITY_VIOLATION);bank[4160+700]^=1;
  assert(PianoProductReleaseSimpleInit(&v)==EFI_SUCCESS && !memcmp(bank+4160,"MZ",2));
  assert(PianoProductValidateSimpleInit(&v)==EFI_ACCESS_DENIED && PianoProductReleaseSimpleInit(&v)==EFI_ACCESS_DENIED);
  init(argv[1]);assert(PianoProductAcquireSimpleInit(&other)==EFI_SUCCESS && other.Lease!=v.Lease);
  assert(PianoProductValidateSimpleInit(&v)==EFI_ACCESS_DENIED);assert(PianoProductReleaseSimpleInit(&other)==EFI_SUCCESS);
  init(argv[1]);handoff[0]=0;assert(PianoProductAcquireSimpleInit(&v)==EFI_NOT_FOUND);
  init(argv[1]);handoff[1]=1;assert(PianoProductAcquireSimpleInit(&v)==EFI_NOT_FOUND);
  init(argv[1]);strcpy(regions[0].Name,"Other");assert(PianoProductAcquireSimpleInit(&v)==EFI_ACCESS_DENIED);
  init(argv[1]);bad_fdt=TRUE;assert(PianoProductAcquireSimpleInit(&v)==EFI_COMPROMISED_DATA);
  init(argv[1]);be(end_be,(UINTN)(bank+4096));assert(PianoProductAcquireSimpleInit(&v)==EFI_BAD_BUFFER_SIZE);
  init(argv[1]);be(start_be,1);assert(PianoProductAcquireSimpleInit(&v)==EFI_BAD_BUFFER_SIZE);
  init(argv[1]);((APP_HEADER *)(bank+4096))->HeaderBytes=65;assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  init(argv[1]);((APP_HEADER *)(bank+4096))->AppBytes=8192;assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  init(argv[1]);((APP_HEADER *)(bank+4096))->AppHash[0]^=1;assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  init(argv[1]);hash_failure=TRUE;assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  // Real ABL v3/v4 semantics: vendor prefix before generic APP, bootconfig tail.
  init(argv[1]);memmove(bank+8192,bank+4096,4160);memset(bank+4096,0xa5,4096);
  memcpy(bank+12352,"bootconfig-data\n#BOOTCONFIG\n",27);be(end_be,(UINTN)(bank+12400));
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SUCCESS && v.Image==bank+8256);
  assert(PianoProductValidateSimpleInit(&v)==EFI_SUCCESS);assert(PianoProductReleaseSimpleInit(&v)==EFI_SUCCESS);
  // Bogus matching text in vendor bytes cannot bypass version/digest/PE pins.
  init(argv[1]);memmove(bank+8192,bank+4096,4160);memset(bank+4096,0,4096);
  memcpy(bank+4096,mMagic,16);be(end_be,(UINTN)(bank+12352));
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SUCCESS && v.Image==bank+8256);
  assert(PianoProductReleaseSimpleInit(&v)==EFI_SUCCESS);
  // Two completely valid copies are ambiguous and rejected.
  init(argv[1]);memcpy(bank+8448,bank+4096,4160);be(end_be,(UINTN)(bank+12608));
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  // A valid APP elsewhere in mapped RAM, outside declared initrd, is ignored.
  init(argv[1]);be(start_be,(UINTN)(bank+8448));be(end_be,(UINTN)(bank+12608));
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SECURITY_VIOLATION);
  init(argv[1]);strcpy(regions[1].Name,"DXE_Heap_Upper");
  assert(PianoProductAcquireSimpleInit(&v)==EFI_SUCCESS);assert(PianoProductPayloadGetFdt(&v,&fdt)==EFI_SUCCESS&&fdt==bank);assert(PianoProductReleaseSimpleInit(&v)==EFI_SUCCESS);
  init(argv[1]);strcpy(regions[1].Name,"DXE_Heap");regions[1].Length=8192;region_count=4;
  strcpy(regions[2].Name,"Piano_HWFence_Reserved");regions[2].Address=(UINTN)(bank+8192);regions[2].Length=4096;
  strcpy(regions[3].Name,"DXE_Heap_Upper");regions[3].Address=(UINTN)(bank+12288);regions[3].Length=4096;
  assert(PianoProductAcquireSimpleInit(&v)==EFI_BAD_BUFFER_SIZE); // APP span crosses protected hole
  init(argv[1]);strcpy(regions[1].Name,"DXE_Heap_Upper");regions[1].Length=32;
  assert(PianoProductAcquireSimpleInit(&v)==EFI_NOT_FOUND); // DT header cannot cross row boundary
  init(argv[1]);mGeneration=MAX_UINTN;assert(PianoProductAcquireSimpleInit(&v)==EFI_OUT_OF_RESOURCES);
  puts("Product payload actual C: real bounded handoff/FDT/APPv1/digest/PE, stale loans and no-free release passed; no hardware.");return 0;
}
