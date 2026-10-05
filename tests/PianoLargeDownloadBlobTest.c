// Actual adapter + CPU loan; host allocation/memory authority only.
#define main LegacyDownloadMain
#include "../tools/test_fastboot_download_blob.c"
#undef main
#define Retain CpuLoanRetain
#define Overlap CpuLoanOverlap
#include "../bootprofiles/os-boot/PianoCpuImageLoan.c"
#undef Overlap
#undef Retain
UINT64 EFIAPI InterlockedCompareExchange64(volatile UINT64 *V,UINT64 C,UINT64 E){UINT64 Old=C;__atomic_compare_exchange_n(V,&Old,E,FALSE,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Old;}
static BOOLEAN live=TRUE;static UINTN slices,checks,spans;static BOOLEAN cancel_zero;
static PIANO_FASTBOOT_DOWNLOAD_BLOB Pool;static PIANO_LAUNCH_BLOB Blob;static VOID *Owner;
static BOOLEAN Alive(VOID*C){assert(C==&Pool);return live;}
static EFI_STATUS Slice(VOID*C,UINTN Budget){assert(C==&Pool&&Budget==1000&&live);++slices;
 if(Pool.Busy){assert(Blob.Restore(Blob.Context,Owner)!=EFI_SUCCESS);assert(Blob.ZeroRelease(Blob.Context,Owner)!=EFI_SUCCESS);}
 return cancel_zero?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS Memory(VOID*C,PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Pool);++checks;
 *P=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=1,.DramBytes=16ULL*1024*1024*1024,
 .NormalBytes=4ULL*1024*1024*1024,.FullDdr=TRUE,.FixedReservations=TRUE,.DynamicReservations=TRUE,.RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};return EFI_SUCCESS;}
static EFI_STATUS Validate(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Pool&&P->BootEpoch==1);return EFI_SUCCESS;}
static EFI_STATUS Buffer(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P,CONST VOID*Producer,VOID*Token,CONST VOID*Base,UINT64 Bytes){
 assert(C==&Pool&&Producer==&Pool&&P->BootEpoch==1&&Bytes==Pool.Bytes);++spans;
 if(Pool.Taken)assert(Token==&Pool&&Base==Pool.Owned);else assert(!Token&&Base==Pool.BoundDownload);return EFI_SUCCESS;}
int main(VOID){
 quiet_status=release_status=EFI_SUCCESS;PIANO_FASTBOOT F={0};UINTN Bytes=PIANO_CPU_INPUT_LOW_BYTES+65537;
 UINT8*Data=AllocateZeroPool(Bytes);memset(Data,0x5a,Bytes);
 // Future platform-owned source; today's command engine is deliberately
 // unchanged64MiB and cannot create this source from a download command.
 F.Download=F.Upload=Data;F.UploadBytes=F.Expected=F.Received=Bytes;F.UploadBorrowed=F.Complete=TRUE;
 assert(PianoFastbootDownloadBlobBind(&Pool,&F,(VOID*)123,quiet,release_pool,&Blob)==EFI_NOT_READY&&!Pool.Signature);
 PIANO_CPU_INPUT_ENV Env={&Pool,Alive,Slice,Memory,Validate,Buffer};
 assert(PianoFastbootDownloadBlobBindWithCpu(&Pool,&F,(VOID*)123,quiet,release_pool,&Env,&Blob)==EFI_SUCCESS&&checks==1);
 assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS&&Owner==&Pool&&!F.Download&&spans>=3);
 PIANO_CPU_IMAGE_LOAN Loan={0};assert(PianoCpuImageLoanInitialize(&Loan)==EFI_SUCCESS);CONST VOID*View=NULL;VOID*Token=NULL;
 assert(PianoCpuImageLoanBegin(&Loan,&Blob,Owner,(PIANO_BOOT_RANGE){0,Bytes},Bytes,&View,&Token)==EFI_NOT_READY&&!Loan.Active);
 assert(PianoCpuImageLoanBeginWithCpu(&Loan,&Blob,Owner,(PIANO_BOOT_RANGE){0,Bytes},Bytes,&Env,&View,&Token)==EFI_SUCCESS&&View==Data);
 assert(slices>1024&&!Loan.Retained&&!Pool.Retained);
 UINT8*Copy=malloc(Bytes);assert(Copy);assert(PianoCpuImageLoanRead(&Loan,Token,0,Bytes,Copy)==EFI_SUCCESS&&!memcmp(Copy,Data,Bytes));free(Copy);
 assert(PianoCpuImageLoanRevalidate(&Loan,Token)==EFI_SUCCESS&&PianoCpuImageLoanEnd(&Loan,Token)==EFI_SUCCESS);
 UINT8 Tail[7];assert(Blob.Read(Blob.Context,Owner,Bytes-7,7,Tail)==EFI_SUCCESS&&Tail[0]==0x5a);
 assert(Blob.Read(Blob.Context,Owner,0,16,Data)==EFI_INVALID_PARAMETER);
 assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_SUCCESS&&Pool.Consumed&&allocations==frees&&slices>4*1024);
 printf("Actual large DownloadBlob/CpuLoan: %llu bytes, %llu slices, full incremental hashes/copy/revalidate/zero, legacy refused, quiet/take semantics kept; transport64MiB unchanged\n",(unsigned long long)Bytes,(unsigned long long)slices);
 // A slice error while destructive clearing is armed retains the source and
 // prohibits restore/retry, without falsely acknowledging FreePool.
 memset(&Pool,0,sizeof(Pool));memset(&F,0,sizeof(F));Data=AllocateZeroPool(Bytes);memset(Data,0x6b,Bytes);
 F.Download=F.Upload=Data;F.UploadBytes=F.Expected=F.Received=Bytes;F.UploadBorrowed=F.Complete=TRUE;
 assert(PianoFastbootDownloadBlobBindWithCpu(&Pool,&F,(VOID*)123,quiet,release_pool,&Env,&Blob)==EFI_SUCCESS);
 assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);cancel_zero=TRUE;unsigned PreviousFrees=frees;
 assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_DEVICE_ERROR&&Pool.Retained&&Pool.ReleaseAttempted&&frees==PreviousFrees&&Data[0]==0x6b);
 assert(Blob.Restore(Blob.Context,Owner)!=EFI_SUCCESS&&Blob.ZeroRelease(Blob.Context,Owner)!=EFI_SUCCESS);
 // Fixture-only cleanup of quarantined host memory; production never retries.
 memset(Data,0,Bytes);FreePool(Data);assert(allocations==frees);return 0;
}
