// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual session harness. EFI protocols/storage are honest memory-only mocks.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoUfsBoundedFileSystemTest.c"
EFI_BOOT_SERVICES *gBS;static EFI_BOOT_SERVICES bs;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiSimpleFileSystemProtocolGuid,gEfiFileInfoGuid;
static EFI_BLOCK_IO_MEDIA media;static EFI_BLOCK_IO_PROTOCOL block;static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_FILE_PROTOCOL root,file,reader;static PIANO_UFS_WINDOW_STATE state;
static UINT8 disk[14680064],filedata[8193];static UINTN position;
static EFI_TPL tpl;static unsigned raises,lowers,format_calls,file_writes,restore_writes,scans,deadloops,cases;
static BOOLEAN recovery_held;
static int bad_format,connect_error,open_error,write_error,short_file_write,flush_error,read_mismatch,bad_info,bad_eof,close_error;
static int missing_fs,null_root,create_error,null_file,read_error,short_read;
static int acquire_error,guard_error,read_restore_error,write_restore_error,short_restore,unknown_quiet,sync_error,verify_error,release_error;
static jmp_buf env;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN N){return FALSE;}VOID EFIAPI DebugPrint(UINTN N,CONST CHAR8 *F,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI AllocatePool(UINTN N){return malloc(N);}VOID EFIAPI FreePool(VOID *P){free(P);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *H){return SHA256(P,N,H)!=NULL;}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}BOOLEAN EFIAPI Sha256Init(VOID *C){return SHA256_Init(C)==1;}
BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *H){return SHA256_Final(H,C)==1;}
VOID EFIAPI CpuDeadLoop(VOID){++deadloops;assert(tpl==TPL_CALLBACK && mLease && !mReturned && state.NeedsRecovery);longjmp(env,1);}
CONST PIANO_UFS_WINDOW_STATE *PianoUfsBoundedTransportState(VOID){return &state;}EFI_HANDLE PianoUfsBoundedTransportHandle(VOID){return (VOID *)4;}
static EFI_TPL EFIAPI raise(EFI_TPL T){assert(T==TPL_CALLBACK && tpl==TPL_APPLICATION);++raises;EFI_TPL Old=tpl;tpl=T;return Old;}
static VOID EFIAPI lower(EFI_TPL T){assert(T==TPL_APPLICATION && tpl==TPL_CALLBACK && !state.Dirty && !state.NeedsRecovery && state.RestoreVerified);tpl=T;++lowers;}
static EFI_STATUS EFIAPI handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){assert(H==(VOID *)4);if(G==&gEfiBlockIoProtocolGuid){*P=&block;return EFI_SUCCESS;}assert(tpl==TPL_CALLBACK && G==&gEfiSimpleFileSystemProtocolGuid);*P=missing_fs?NULL:&fs;return missing_fs?EFI_NOT_FOUND:EFI_SUCCESS;}
static EFI_STATUS EFIAPI connect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN Recursive){assert(tpl==TPL_CALLBACK && H==(VOID *)4 && Recursive);return connect_error?EFI_NOT_FOUND:EFI_SUCCESS;}
static EFI_STATUS EFIAPI format(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Data){assert(tpl==TPL_CALLBACK && B==&block && Id==1 && Bytes==4096 && (Lba==0 || Lba==1 || Lba==3 || Lba==5));++format_calls;state.Dirty=state.NeedsRecovery=TRUE;++state.WriteAttempts;memcpy(disk+Lba*4096,Data,4096);return bad_format==(int)format_calls?EFI_TIMEOUT:EFI_SUCCESS;}
static EFI_STATUS EFIAPI openvol(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F,EFI_FILE_PROTOCOL **R){assert(tpl==TPL_CALLBACK && F==&fs);*R=null_root?NULL:&root;return open_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI openfile(EFI_FILE_PROTOCOL *R,EFI_FILE_PROTOCOL **F,CHAR16 *Name,UINT64 Mode,UINT64 Attr){assert(tpl==TPL_CALLBACK && R==&root && Name[0]=='S' && Attr==0);if(Mode&EFI_FILE_MODE_CREATE){*F=null_file || create_error?NULL:&file;if(create_error)return EFI_OUT_OF_RESOURCES;}else{*F=&reader;position=0;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI filewrite(EFI_FILE_PROTOCOL *F,UINTN *Bytes,VOID *Data){assert(tpl==TPL_CALLBACK && F==&file && *Bytes==8193);++file_writes;memcpy(filedata,Data,8193);memcpy(disk+6*4096,Data,8193);state.WriteAttempts+=3;*Bytes=short_file_write?8192:8193;return write_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI flush(EFI_FILE_PROTOCOL *F){assert(F==&file && tpl==TPL_CALLBACK);return flush_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI closefile(EFI_FILE_PROTOCOL *F){assert(tpl==TPL_CALLBACK);return close_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI info(EFI_FILE_PROTOCOL *F,EFI_GUID *G,UINTN *Bytes,VOID *Data){assert(F==&reader && G==&gEfiFileInfoGuid && tpl==TPL_CALLBACK);EFI_FILE_INFO *I=Data;memset(Data,0,*Bytes);I->Size=SIZE_OF_EFI_FILE_INFO;I->FileSize=bad_info?8192:8193;*Bytes=SIZE_OF_EFI_FILE_INFO;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI fileread(EFI_FILE_PROTOCOL *F,UINTN *Bytes,VOID *Data){assert(F==&reader && tpl==TPL_CALLBACK);if(position==8193){*Bytes=bad_eof?1:0;return EFI_SUCCESS;}assert(*Bytes==8193);if(read_error)return EFI_DEVICE_ERROR;memcpy(Data,filedata,8193);if(short_read)*Bytes=8192;if(read_mismatch)((UINT8 *)Data)[9]^=1;position=8193;return EFI_SUCCESS;}
static EFI_STATUS rec_acquire(VOID *C){assert(tpl==TPL_CALLBACK && state.Closed && !recovery_held);if(acquire_error)return EFI_TIMEOUT;recovery_held=TRUE;return EFI_SUCCESS;}
static EFI_STATUS rec_release(VOID *C,BOOLEAN Quarantine){assert(recovery_held && tpl==TPL_CALLBACK);recovery_held=FALSE;if(release_error){state.NeedsRecovery=TRUE;state.RestoreVerified=FALSE;return EFI_TIMEOUT;}return EFI_SUCCESS;}
static EFI_STATUS rec_guard(VOID *C,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G){assert(recovery_held && Lun==4);*G=(PIANO_UFS_WRITE_GUARD){.Lun=4,.Collected=31,.CapacityBytes=1551892480,.Fua=!guard_error,.UnitWriteProtect=1};return EFI_SUCCESS;}
static EFI_STATUS rec_quiet(VOID *C,BOOLEAN *Q){assert(recovery_held);*Q=!(unknown_quiet && restore_writes);return EFI_SUCCESS;}
static EFI_STATUS rec_read(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){assert(recovery_held && Lun==4 && Lba>=375040 && Lba<=378623 && Bytes==4096);++scans;memcpy(Buffer,disk+(Lba-375040)*4096,4096);*Got=4096;return read_restore_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS rec_write(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Data,UINTN *Done){assert(recovery_held && Lun==4 && Bytes==4096 && Lba>=375040 && Lba<=378623 && mRestoreAttempted && !mRestoreReturned && mRestoreAttempts==restore_writes+1);++restore_writes;*Done=short_restore?512:4096;memcpy(disk+(Lba-375040)*4096,Data,*Done);return write_restore_error?EFI_TIMEOUT:EFI_SUCCESS;}
static EFI_STATUS rec_sync(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){assert(recovery_held && Lun==4 && Lba==375040 && Bytes==14680064);return sync_error?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS PianoUfsBoundedTransportCloseForRecovery(PIANO_UFS_WINDOW_IO *R){assert(tpl==TPL_CALLBACK);state.Closed=TRUE;state.ConsumersDetached=TRUE;*R=(PIANO_UFS_WINDOW_IO){.Io={&state,rec_read,rec_write,rec_sync,rec_guard,rec_quiet},.Acquire=rec_acquire,.Release=rec_release};return EFI_SUCCESS;}
EFI_STATUS PianoUfsBoundedTransportVerifyRestored(VOID){assert(recovery_held && tpl==TPL_CALLBACK);for(UINTN I=0;I<14680064;++I)assert(disk[I]==0);if(verify_error)return EFI_CRC_ERROR;state.Dirty=state.NeedsRecovery=state.Quarantined=FALSE;state.RestoreVerified=TRUE;return EFI_SUCCESS;}
static void fresh(void){++cases;memset(disk,0,sizeof(disk));memset(&state,0,sizeof(state));state.Opened=TRUE;tpl=TPL_APPLICATION;raises=lowers=format_calls=file_writes=restore_writes=scans=deadloops=0;recovery_held=FALSE;position=0;missing_fs=null_root=create_error=null_file=read_error=short_read=0;bad_format=connect_error=open_error=write_error=short_file_write=flush_error=read_mismatch=bad_info=bad_eof=close_error=acquire_error=guard_error=read_restore_error=write_restore_error=short_restore=unknown_quiet=sync_error=verify_error=release_error=0;
 mStarted=mReturned=mLease=mRecoveryLease=mVerified=mFileMatched=mRestoreAttempted=mRestoreReturned=FALSE;mFirst=mFinal=EFI_NOT_STARTED;mScanned=mNonzero=mRestoreAttempts=mRestored=mFormatBlocks=mRestoreDone=0;memset(mSteps,0,sizeof(mSteps));
 media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=TRUE,.BlockSize=4096,.LastBlock=3583};block=(EFI_BLOCK_IO_PROTOCOL){.Media=&media,.WriteBlocks=format};fs.OpenVolume=openvol;root.Open=openfile;root.Close=closefile;file.Write=filewrite;file.Flush=flush;file.Close=closefile;reader.Read=fileread;reader.GetInfo=info;reader.Close=closefile;
}
static void restored(BOOLEAN Passed){EFI_STATUS S=PianoUfsRunBoundedFileSystemTest();assert((S==EFI_SUCCESS)==Passed && mReturned && mVerified && mScanned==3584 && mRestored==mNonzero && mRestoreAttempts==mRestored && raises==1 && lowers==1 && !deadloops && !state.Dirty && !state.NeedsRecovery);assert(PianoUfsRunBoundedFileSystemTest()==EFI_ACCESS_DENIED);}
static void unsafe(void){if(!setjmp(env)){PianoUfsRunBoundedFileSystemTest();assert(!"Unverified session must not return");}assert(deadloops==1 && !mReturned && mLease && !mVerified && !lowers && state.NeedsRecovery);}
int main(void){gBS=&bs;bs.RaiseTPL=raise;bs.RestoreTPL=lower;bs.HandleProtocol=handle;bs.ConnectController=connect;
 fresh();restored(TRUE);assert(mFileMatched && mFormatBlocks==4 && mNonzero==7 && file_writes==1);
 fresh();bad_format=2;restored(FALSE);assert(mFormatBlocks==1 && mNonzero==2);
 fresh();connect_error=1;restored(FALSE);fresh();open_error=1;restored(FALSE);fresh();write_error=1;restored(FALSE);fresh();short_file_write=1;restored(FALSE);fresh();flush_error=1;restored(FALSE);fresh();read_mismatch=1;restored(FALSE);fresh();bad_info=1;restored(FALSE);fresh();bad_eof=1;restored(FALSE);fresh();close_error=1;restored(FALSE);
 fresh();missing_fs=1;restored(FALSE);fresh();null_root=1;restored(FALSE);fresh();create_error=1;restored(FALSE);fresh();null_file=1;restored(FALSE);fresh();read_error=1;restored(FALSE);fresh();short_read=1;restored(FALSE);
 fresh();acquire_error=1;unsafe();fresh();guard_error=1;unsafe();fresh();read_restore_error=1;unsafe();fresh();write_restore_error=1;unsafe();assert(mRestoreAttempts==1 && mRestoreReturned && !mRestored);
 fresh();short_restore=1;unsafe();fresh();unknown_quiet=1;unsafe();assert(restore_writes==1 && !mRestored);
 fresh();sync_error=1;unsafe();fresh();verify_error=1;unsafe();fresh();release_error=1;unsafe();
 printf("Actual bounded FAT/SFS session harness: %u success/error/whole-gap-restore/fence cases passed; mocked EFI/storage, no device.\n",cases);
}
