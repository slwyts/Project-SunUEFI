// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoFastboot.c"
#include "../../uefi/core/PianoFastbootDownloadBlob.c"
static struct {void *pointer;size_t bytes;} pools[16];
static unsigned allocations,frees,quiet_calls;
static EFI_STATUS quiet_status,release_status;
static unsigned releases;
static PIANO_FASTBOOT *change_during_quiet;
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *A,UINTN N){return memset(A,0,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){
  for(unsigned i=0;i<16;i++)if(!pools[i].pointer){pools[i].pointer=calloc(1,N);assert(pools[i].pointer);pools[i].bytes=N;allocations++;return pools[i].pointer;}
  assert(0);return NULL;
}
VOID EFIAPI FreePool(VOID *P){
  for(unsigned i=0;i<16;i++)if(pools[i].pointer==P){
    for(size_t j=0;j<pools[i].bytes;j++)assert(((unsigned char*)P)[j]==0);
    free(P);pools[i].pointer=NULL;frees++;return;
  }assert(0);
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *A,UINTN N,UINT8 *Digest){return SHA256(A,N,Digest)!=NULL;}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}
BOOLEAN EFIAPI Sha256Init(VOID *C){return SHA256_Init(C)==1;}
BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}
BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *D){return SHA256_Final(D,C)==1;}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_STATUS EFIAPI release_pool(VOID *P){releases++;if(release_status!=EFI_SUCCESS)return release_status;FreePool(P);return EFI_SUCCESS;}
static EFI_STATUS send_reply(VOID *Context,CONST VOID *Data,UINTN Bytes){(void)Context;(void)Data;assert(Bytes);return EFI_SUCCESS;}
static EFI_STATUS quiet(VOID *Context){assert(Context==(void*)123);quiet_calls++;if(change_during_quiet){change_during_quiet->Complete=FALSE;change_during_quiet=NULL;}return quiet_status;}
static void download(PIANO_FASTBOOT *F){
  assert(PianoFastbootInit(F,NULL,send_reply,NULL)==EFI_SUCCESS);
  assert(PianoFastbootPacket(F,"download:00000008",17)==EFI_SUCCESS);
  assert(PianoFastbootPacket(F,"abcdefgh",8)==EFI_SUCCESS);
}
static void bind(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,PIANO_FASTBOOT *F,PIANO_LAUNCH_BLOB *B){memset(S,0,sizeof(*S));assert(PianoFastbootDownloadBlobBind(S,F,(void*)123,quiet,release_pool,B)==EFI_SUCCESS);}
int main(void){
  PIANO_FASTBOOT F;PIANO_FASTBOOT_DOWNLOAD_BLOB S={0};PIANO_LAUNCH_BLOB B={0};void *owner=NULL,*loan=NULL,*oldloan=NULL;const void *view=NULL;char out[8];
  quiet_status=EFI_SUCCESS;
  assert(PianoFastbootDownloadBlobBind(NULL,&F,NULL,quiet,release_pool,&B)==EFI_INVALID_PARAMETER);
  memset(&F,0,sizeof(F));assert(PianoFastbootDownloadBlobBind(&S,&F,(void*)123,quiet,release_pool,&B)==EFI_NOT_READY);
  download(&F);quiet_status=EFI_NOT_READY;
  assert(PianoFastbootDownloadBlobBind(&S,&F,(void*)123,quiet,release_pool,&B)==EFI_NOT_READY && !S.Signature);
  quiet_status=EFI_WARN_UNKNOWN_GLYPH;
  assert(PianoFastbootDownloadBlobBind(&S,&F,(void*)123,quiet,release_pool,&B)==EFI_WARN_UNKNOWN_GLYPH && !S.Signature);
  quiet_status=EFI_SUCCESS;bind(&S,&F,&B);
  assert(B.Bytes==8 && allocations==1 && F.Download==S.BoundDownload);
  assert(PianoFastbootDownloadBlobBind(&S,&F,(void*)123,quiet,release_pool,&B)==EFI_ALREADY_STARTED);
  quiet_status=EFI_DEVICE_ERROR;assert(B.Take(B.Context,&owner)==EFI_DEVICE_ERROR && !owner && F.Download);
  quiet_status=EFI_SUCCESS;change_during_quiet=&F;
  assert(B.Take(B.Context,&owner)==EFI_NOT_READY && !owner && F.Download);F.Complete=TRUE;
  assert(B.Take(B.Context,&owner)==EFI_SUCCESS && owner==&S && !F.Download && !F.Upload && !F.Complete && allocations==1);
  unsigned freed=frees;PianoFastbootReset(&F);assert(frees==freed);
  void *another_owner=NULL;assert(B.Take(B.Context,&another_owner)==EFI_ALREADY_STARTED && !another_owner);
  assert(B.Read(B.Context,owner,1,3,out)==EFI_SUCCESS && !memcmp(out,"bcd",3));
  assert(B.Read(B.Context,owner,MAX_UINT64,1,out)==EFI_BAD_BUFFER_SIZE);
  assert(B.Read(B.Context,owner,7,2,out)==EFI_BAD_BUFFER_SIZE);
  assert(B.Read(B.Context,(void*)1,0,1,out)==EFI_INVALID_PARAMETER);
  assert(B.Read(B.Context,owner,0,0,out)==EFI_INVALID_PARAMETER);
  assert(B.BorrowView(B.Context,owner,(PIANO_BOOT_RANGE){2,4},&view,&loan)==EFI_SUCCESS && !memcmp(view,"cdef",4));
  assert(B.Restore(B.Context,owner)==EFI_ACCESS_DENIED && B.ZeroRelease(B.Context,owner)==EFI_ACCESS_DENIED && frees==freed);
  assert(B.Unborrow(B.Context,owner,(void*)999)==EFI_INVALID_PARAMETER);
  oldloan=loan;assert(B.Unborrow(B.Context,owner,loan)==EFI_SUCCESS);
  assert(B.BorrowView(B.Context,owner,(PIANO_BOOT_RANGE){0,8},&view,&loan)==EFI_SUCCESS && loan!=oldloan);
  assert(B.Unborrow(B.Context,owner,oldloan)==EFI_INVALID_PARAMETER);
  assert(B.Unborrow(B.Context,owner,loan)==EFI_SUCCESS);
  assert(B.BorrowView(B.Context,owner,(PIANO_BOOT_RANGE){8,1},&view,&loan)==EFI_BAD_BUFFER_SIZE && !view && !loan);
  assert(B.Restore(B.Context,owner)==EFI_SUCCESS && F.Complete && F.UploadBorrowed && !memcmp(F.Download,"abcdefgh",8));
  assert(B.ZeroRelease(B.Context,owner)==EFI_INVALID_PARAMETER);PianoFastbootReset(&F);assert(frees==freed+1);
  download(&F);bind(&S,&F,&B);assert(B.Take(B.Context,&owner)==EFI_SUCCESS);
  assert(PianoFastbootPacket(&F,"download:00000004",17)==EFI_SUCCESS);
  assert(B.Restore(B.Context,owner)==EFI_ACCESS_DENIED && S.Owned);
  assert(B.ZeroRelease(B.Context,owner)==EFI_SUCCESS && !S.Owned);PianoFastbootReset(&F);
  download(&F);bind(&S,&F,&B);unsigned before=allocations;
  UINT8 *replacement=AllocateZeroPool(8);UINT8 *original=F.Download;F.Download=replacement;F.Upload=replacement;
  assert(B.Take(B.Context,&owner)==EFI_NOT_READY && !owner);ZeroMem(original,8);FreePool(original);PianoFastbootReset(&F);assert(allocations==before+1);
  download(&F);assert(PianoFastbootStageCopy(&F,"other",5)==EFI_SUCCESS);memset(&S,0,sizeof(S));
  assert(PianoFastbootDownloadBlobBind(&S,&F,(void*)123,quiet,release_pool,&B)==EFI_NOT_READY);PianoFastbootReset(&F);
  download(&F);bind(&S,&F,&B);assert(B.Take(B.Context,&owner)==EFI_SUCCESS);S.LoanSequence=MAX_UINTN;
  assert(B.BorrowView(B.Context,owner,(PIANO_BOOT_RANGE){0,8},&view,&loan)==EFI_OUT_OF_RESOURCES);
  assert(B.ZeroRelease(B.Context,owner)==EFI_SUCCESS && B.ZeroRelease(B.Context,owner)==EFI_INVALID_PARAMETER);
  for(unsigned warning=0;warning<2;warning++){
    download(&F);bind(&S,&F,&B);assert(B.Take(B.Context,&owner)==EFI_SUCCESS);
    release_status=warning?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    unsigned calls_before=releases,free_before=frees;UINT8 *retained=S.Owned;
    assert(B.ZeroRelease(B.Context,owner)==EFI_DEVICE_ERROR && S.Owned==retained && S.Taken && !S.Consumed);
    assert(S.ReleaseAttempted && S.ReleaseStatus==release_status && releases==calls_before+1 && frees==free_before);
    for(unsigned i=0;i<8;i++)assert(retained[i]==0);
    assert(B.Read(B.Context,owner,0,1,out)==EFI_INVALID_PARAMETER);
    assert(B.BorrowView(B.Context,owner,(PIANO_BOOT_RANGE){0,8},&view,&loan)==EFI_INVALID_PARAMETER);
    assert(B.Restore(B.Context,owner)==EFI_INVALID_PARAMETER && !F.Download);
    assert(B.ZeroRelease(B.Context,owner)==EFI_INVALID_PARAMETER && releases==calls_before+1);
    // Test-only allocator cleanup after a mock that explicitly retained it.
    // Production unknown-release ledger is never retried or dereferenced.
    FreePool(retained);release_status=EFI_SUCCESS;
  }
  assert(allocations==frees && quiet_calls>=8);
  puts("Fastboot download ownership: exact quiet, no copy, reset survival, bounds, stale loans, restore refusal and zero-release passed.");return 0;
}
