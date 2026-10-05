// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual fastboot pool+ownership adapter+CPU loan. No controller or execution.
#define main prior_download_blob_suite
#include "../tools/test_fastboot_download_blob.c"
#undef main
#include "../bootprofiles/os-boot/PianoCpuImageLoan.c"
UINT64 EFIAPI InterlockedCompareExchange64(volatile UINT64 *Value,UINT64 Compare,UINT64 Exchange){UINT64 Expected=Compare;__atomic_compare_exchange_n(Value,&Expected,Exchange,FALSE,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Expected;}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
static UINTN takes;
static EFI_STATUS (*real_take)(VOID *,VOID **);
static EFI_STATUS (*real_unborrow)(VOID *,VOID *,VOID *);
static EFI_STATUS count_take(VOID *Context,VOID **Owner){++takes;return real_take(Context,Owner);}
static EFI_STATUS warning_unborrow(VOID *Context,VOID *Owner,VOID *Token){(void)Context;(void)Owner;(void)Token;return EFI_WARN_STALE_DATA;}
static EFI_STATUS no_borrow(VOID *Context,VOID *Owner,PIANO_BOOT_RANGE Range,CONST VOID **View,VOID **Token){(void)Context;(void)Owner;(void)Range;*View=NULL;*Token=NULL;return EFI_NOT_READY;}
int main(void){
  PIANO_FASTBOOT F;PIANO_FASTBOOT_DOWNLOAD_BLOB Pool={0};PIANO_LAUNCH_BLOB Blob={0};PIANO_CPU_IMAGE_LOAN Session={0};
  VOID *Owner=NULL,*Token=NULL,*Stale=NULL;CONST VOID *View=NULL;CHAR8 Copy[8];
  quiet_status=release_status=EFI_SUCCESS;download(&F);bind(&Pool,&F,&Blob);
  real_take=Blob.Take;Blob.Take=count_take;assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS && takes==1 && Owner==&Pool);
  unsigned PoolAlloc=allocations,PoolFreed=frees;
  assert(PianoCpuImageLoanInitialize(&Session)==EFI_SUCCESS && PianoCpuImageLoanInitialize(&Session)==EFI_ALREADY_STARTED);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){2,4},8,&View,&Token)==EFI_SUCCESS && View==Pool.Owned+2 && !memcmp(View,"cdef",4));
  assert(takes==1 && allocations==PoolAlloc && frees==PoolFreed && !F.BootProof.AckCompleted && !F.BootProof.DmaFreed && !F.BootProof.DmaBuffersFreed);
  assert(PianoCpuImageLoanRead(&Session,Token,0,4,Copy)==EFI_SUCCESS && !memcmp(Copy,"cdef",4));
  assert(PianoCpuImageLoanRead(&Session,Token,4,1,Copy)==EFI_BAD_BUFFER_SIZE && PianoCpuImageLoanRead(&Session,(VOID *)99,0,1,Copy)==EFI_ACCESS_DENIED);
  assert(PianoCpuImageLoanRead(&Session,Token,0,1,(VOID *)View)==EFI_INVALID_PARAMETER);
  assert(PianoCpuImageLoanRead(&Session,Token,0,1,&Session)==EFI_INVALID_PARAMETER);
  assert(PianoCpuImageLoanRevalidate(&Session,Token)==EFI_SUCCESS);
  assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_ACCESS_DENIED && Blob.Restore(Blob.Context,Owner)==EFI_ACCESS_DENIED);
  // Real fastboot state can process a new CPU download while the original pool
  // is held outside it; this is not evidence that the device service is wired.
  assert(PianoFastbootPacket(&F,"download:00000004",17)==EFI_SUCCESS && PianoFastbootPacket(&F,"WXYZ",4)==EFI_SUCCESS);
  PianoFastbootReset(&F);assert(frees==PoolFreed+1 && PianoCpuImageLoanRead(&Session,Token,0,4,Copy)==EFI_SUCCESS && !memcmp(Copy,"cdef",4));
  Stale=Token;assert(PianoCpuImageLoanEnd(&Session,Token)==EFI_SUCCESS && !Session.Active && Pool.Owned);
  assert(PianoCpuImageLoanRead(&Session,Stale,0,1,Copy)==EFI_ACCESS_DENIED);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS && Token!=Stale && takes==1);
  assert(PianoCpuImageLoanEnd(&Session,Stale)==EFI_ACCESS_DENIED && PianoCpuImageLoanEnd(&Session,Token)==EFI_SUCCESS);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){0,9},8,&View,&Token)==EFI_INVALID_PARAMETER);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){MAX_UINT64,1},8,&View,&Token)==EFI_INVALID_PARAMETER);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},7,&View,&Token)==EFI_INVALID_PARAMETER);
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS);
  Pool.Owned[4]^=1;assert(PianoCpuImageLoanRevalidate(&Session,Token)==EFI_CRC_ERROR && Session.Retained && Session.Active);
  assert(PianoCpuImageLoanEnd(&Session,Token)==EFI_ACCESS_DENIED && Blob.ZeroRelease(Blob.Context,Owner)==EFI_ACCESS_DENIED);
  // Test-only acknowledged reset of retained memory model; production leaves
  // the owner/loan resident and never releases this uncertain session itself.
  assert(Blob.Unborrow(Blob.Context,Owner,Session.SourceLoan)==EFI_SUCCESS);assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_SUCCESS);
  memset(&Session,0,sizeof(Session));download(&F);bind(&Pool,&F,&Blob);assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);
  assert(PianoCpuImageLoanInitialize(&Session)==EFI_SUCCESS);
  PIANO_LAUNCH_BLOB Unavailable=Blob;Unavailable.BorrowView=no_borrow;
  assert(PianoCpuImageLoanBegin(&Session,&Unavailable,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_NOT_READY && !Session.Active && !Session.Retained);
  real_unborrow=Blob.Unborrow;Blob.Unborrow=warning_unborrow;
  assert(PianoCpuImageLoanBegin(&Session,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS);
  assert(PianoCpuImageLoanEnd(&Session,Token)==EFI_DEVICE_ERROR && Session.Retained && Pool.ActiveLoan);
  assert(real_unborrow(Blob.Context,Owner,Session.SourceLoan)==EFI_SUCCESS);assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_SUCCESS);
  memset(&Session,0,sizeof(Session));download(&F);bind(&Pool,&F,&Blob);assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);
  PIANO_CPU_IMAGE_LOAN A={0},B={0};assert(PianoCpuImageLoanInitialize(&A)==EFI_SUCCESS && PianoCpuImageLoanInitialize(&B)==EFI_SUCCESS);
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS);VOID *AToken=Token;
  assert(PianoCpuImageLoanEnd(&A,AToken)==EFI_SUCCESS);
  assert(PianoCpuImageLoanBegin(&B,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS && Token!=AToken);
  assert(PianoCpuImageLoanRead(&B,AToken,0,1,Copy)==EFI_ACCESS_DENIED && PianoCpuImageLoanEnd(&B,AToken)==EFI_ACCESS_DENIED);
  assert(PianoCpuImageLoanEnd(&B,Token)==EFI_SUCCESS);memset(&A,0,sizeof(A));assert(PianoCpuImageLoanInitialize(&A)==EFI_SUCCESS);
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_SUCCESS && Token!=AToken);
  assert(PianoCpuImageLoanRevalidate(&A,AToken)==EFI_ACCESS_DENIED && PianoCpuImageLoanEnd(&A,Token)==EFI_SUCCESS);
  UINT8 Saved[8];memcpy(Saved,Pool.Owned,8);PIANO_CPU_IMAGE_LOAN SavedState=A;
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&A.View,&Token)==EFI_INVALID_PARAMETER && !memcmp(&A,&SavedState,sizeof(A)));
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&A.Token)==EFI_INVALID_PARAMETER && !memcmp(&A,&SavedState,sizeof(A)));
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,(CONST VOID **)Pool.Owned,&Token)==EFI_INVALID_PARAMETER && !memcmp(Saved,Pool.Owned,8) && !Pool.ActiveLoan && !A.Active);
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,(VOID **)Pool.Owned)==EFI_INVALID_PARAMETER && !memcmp(Saved,Pool.Owned,8) && !Pool.ActiveLoan && !A.Active);
  assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,(CONST VOID **)&Token,&Token)==EFI_INVALID_PARAMETER);
  mDriverLoanSequence=MAX_UINTN;assert(PianoCpuImageLoanBegin(&A,&Blob,Owner,(PIANO_BOOT_RANGE){0,8},8,&View,&Token)==EFI_OUT_OF_RESOURCES && !Pool.ActiveLoan && !A.Active);
  assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_SUCCESS);
  assert(allocations==frees);puts("Actual CPU image loan + DownloadBlob: one existing Take, zero image copy/new loan allocation, live fastboot CPU state reset/new download survival, bounds/alias/stale token, hash mutation and uncertain-unborrow retention; native ACK/DMA proof untouched, no execution/EBS permit.");
  return 0;
}
