// SPDX-License-Identifier: BSD-2-Clause-Patent
// One synchronous FAT session followed by mandatory independent whole-gap proof.
#include "PianoUfsBoundedFileSystemTest.h"
#include "PianoUfsBoundedTransport.h"
#ifndef PIANO_UFS_BOUNDED_FS_FORMAT_HEADER
#define PIANO_UFS_BOUNDED_FS_FORMAT_HEADER "PianoUfsBoundedFsFormat.h"
#endif
#include PIANO_UFS_BOUNDED_FS_FORMAT_HEADER
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#define FILE_BYTES 8193U
STATIC UINT8 mTx[FILE_BYTES],mRx[FILE_BYTES],mZero[4096],mRestoreRx[4096];
STATIC BOOLEAN mStarted,mReturned,mLease,mRecoveryLease,mVerified,mFileMatched;
STATIC EFI_STATUS mFirst=EFI_NOT_STARTED,mFinal=EFI_NOT_STARTED;
STATIC UINTN mScanned,mNonzero,mRestoreAttempts,mRestored,mFormatBlocks,mRestoreDone;
STATIC EFI_LBA mLastRestore;
STATIC BOOLEAN mRestoreAttempted,mRestoreReturned;
STATIC EFI_STATUS mRestoreWrite=EFI_NOT_STARTED,mRestoreSync=EFI_NOT_STARTED,mRestoreRead=EFI_NOT_STARTED,mRestoreQuiet=EFI_NOT_STARTED;
typedef struct {BOOLEAN Attempted,Returned;EFI_STATUS Status;UINTN Bytes;} STEP;
enum {Header,Format0,Format1,Format2,Format3,Connect,SimpleFs,OpenVolume,Create,FileWrite,FileFlush,FileClose,Reopen,Info,FileRead,Eof,ReadClose,RootClose,CloseRecovery,AcquireRecovery,RecoveryGate,ScanRestore,FinalSync,VerifyWhole,ReleaseRecovery,Steps};
STATIC STEP mSteps[Steps];
STATIC EFI_STATUS Norm(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC VOID Begin(UINTN Id){mSteps[Id].Attempted=TRUE;mSteps[Id].Returned=FALSE;}
STATIC EFI_STATUS End(UINTN Id,EFI_STATUS S){if(S!=EFI_SUCCESS && !EFI_ERROR(S))S=EFI_DEVICE_ERROR;mSteps[Id].Returned=TRUE;mSteps[Id].Status=S;if(S!=EFI_SUCCESS && mFirst==EFI_NOT_STARTED)mFirst=S;return S;}
STATIC BOOLEAN Zero(CONST UINT8 *P){for(UINTN I=0;I<4096;++I)if(P[I])return FALSE;return TRUE;}
STATIC EFI_STATUS Quiet(CONST PIANO_UFS_WINDOW_IO *T){BOOLEAN Q=FALSE;EFI_STATUS S=T->Io.Quiesced(T->Io.Context,&Q);return S==EFI_SUCCESS && Q?EFI_SUCCESS:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS ExactRead(CONST PIANO_UFS_WINDOW_IO *T,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){SetMem(Buffer,Bytes,0xCC);UINTN Got=0;EFI_STATUS S=T->Io.Read(T->Io.Context,4,Lba,Bytes,Buffer,&Got);EFI_STATUS Q=Quiet(T);if(Q!=EFI_SUCCESS)return Q;return S==EFI_SUCCESS && Got==Bytes?EFI_SUCCESS:S==EFI_SUCCESS?EFI_BAD_BUFFER_SIZE:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS HeaderCheck(VOID){
  STATIC CONST UINT8 Expected[32]={0x1e,0x6d,0x98,0x31,0x4d,0x88,0x0f,0x88,0xd6,0x53,0x7f,0xb4,0x66,0xd3,0xec,0x87,0xb2,0x94,0x42,0x25,0x38,0xbd,0xc5,0x79,0x5c,0x57,0x4b,0x6a,0x1e,0x71,0x3a,0xdd};
  STATIC CONST UINT32 Lbas[4]={0,1,3,5};UINT8 Hash[32];
  if(PIANO_UFS_FAT_FORMAT_PC_VERIFIED!=TRUE || CompareMem(mPianoFatLbas,Lbas,sizeof(Lbas)) || CompareMem(mPianoFatImageSha,Expected,32))return EFI_SECURITY_VIOLATION;
  for(UINTN I=0;I<4;++I)if(!Sha256HashAll(mPianoFatBlocks[I],4096,Hash) || CompareMem(Hash,mPianoFatBlockHashes[I],32))return EFI_SECURITY_VIOLATION;
  UINTN Size=Sha256GetContextSize();if(Size==0 || Size>4096)return EFI_UNSUPPORTED;VOID *Context=AllocatePool(Size);if(Context==NULL)return EFI_OUT_OF_RESOURCES;
  BOOLEAN Ok=Sha256Init(Context);UINTN Extent=0;
  for(UINTN Lba=0;Lba<3584 && Ok;++Lba){CONST UINT8 *Data=mZero;if(Extent<4 && mPianoFatLbas[Extent]==Lba)Data=mPianoFatBlocks[Extent++];Ok=Sha256Update(Context,Data,4096);}
  if(Ok)Ok=Sha256Final(Context,Hash);FreePool(Context);return Ok && CompareMem(Hash,Expected,32)==0?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
}
VOID PianoReportBoundedFileSystemTest(VOID){
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_SESSION started=%u returned=%u lease=%u recovery_lease=%u first=%r final=%r format_blocks=%lu file_bytes=8193 file_match=%u scanned=%lu nonzero=%lu restore_attempts=%lu restored=%lu whole_verified=%u\n",mStarted,mReturned,mLease,mRecoveryLease,mFirst,mFinal,(UINT64)mFormatBlocks,mFileMatched,(UINT64)mScanned,(UINT64)mNonzero,(UINT64)mRestoreAttempts,(UINT64)mRestored,mVerified));
  for(UINTN I=0;I<Steps;++I)DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_STEP id=%u attempted=%u returned=%u status=%r bytes=%lu\n",(UINT32)I,mSteps[I].Attempted,mSteps[I].Returned,mSteps[I].Status,(UINT64)mSteps[I].Bytes));
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_RESTORE_LAST physical=%lu attempted=%u returned=%u transferred=%lu write=%r sync=%r read=%r quiet=%r\n",mLastRestore,mRestoreAttempted,mRestoreReturned,(UINT64)mRestoreDone,mRestoreWrite,mRestoreSync,mRestoreRead,mRestoreQuiet));
}
EFI_STATUS PianoUfsRunBoundedFileSystemTest(VOID){
  if(mStarted)return EFI_ACCESS_DENIED;mStarted=TRUE;for(UINTN I=0;I<Steps;++I)mSteps[I].Status=EFI_NOT_STARTED;
  EFI_STATUS S;EFI_TPL Old=TPL_APPLICATION;EFI_HANDLE Handle=PianoUfsBoundedTransportHandle();EFI_BLOCK_IO_PROTOCOL *Block=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;
  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL,*Reader=NULL;PIANO_UFS_WINDOW_IO Recovery;ZeroMem(&Recovery,sizeof(Recovery));
  CONST PIANO_UFS_WINDOW_STATE *State=PianoUfsBoundedTransportState();
  Begin(Header);S=End(Header,HeaderCheck());if(EFI_ERROR(S))goto BeforeLease;
  if(Handle==NULL || State==NULL || !State->Opened || State->Closed || State->Dirty || State->NeedsRecovery || State->Quarantined || State->WriteAttempts){S=EFI_ACCESS_DENIED;goto BeforeLease;}
  S=gBS->HandleProtocol(Handle,&gEfiBlockIoProtocolGuid,(VOID **)&Block);
  if(EFI_ERROR(S) || Block==NULL || Block->Media==NULL || Block->Media->BlockSize!=4096 || Block->Media->LastBlock!=3583 || Block->Media->ReadOnly){S=EFI_ACCESS_DENIED;goto BeforeLease;}
  // Synchronous File calls only. FatFsLock=TPL_CALLBACK; no FileEx/async tasks,
  // no WaitForEvent, no user input. All inner IO releases restore CALLBACK.
  Old=gBS->RaiseTPL(TPL_CALLBACK);mLease=TRUE;
  for(UINTN I=0;I<FILE_BYTES;++I)mTx[I]=(UINT8)((I*37U+(I>>5))^0xA5U);
  STATIC CONST CHAR8 Marker[]="PIANO-BOUNDED-SFS-SESSION-v1";CopyMem(mTx,Marker,sizeof(Marker));
  for(UINTN I=0;I<4;++I){Begin(Format0+I);S=End(Format0+I,Block->WriteBlocks(Block,Block->Media->MediaId,mPianoFatLbas[I],4096,(VOID *)mPianoFatBlocks[I]));if(EFI_ERROR(S))goto Recover;++mFormatBlocks;mSteps[Format0+I].Bytes=4096;}
  Begin(Connect);S=End(Connect,gBS->ConnectController(Handle,NULL,NULL,TRUE));if(EFI_ERROR(S))goto Recover;
  Begin(SimpleFs);S=End(SimpleFs,gBS->HandleProtocol(Handle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs));if(EFI_ERROR(S) || Fs==NULL){if(!EFI_ERROR(S))S=End(SimpleFs,EFI_DEVICE_ERROR);goto Recover;}
  Begin(OpenVolume);S=End(OpenVolume,Fs->OpenVolume(Fs,&Root));if(EFI_ERROR(S) || Root==NULL){if(!EFI_ERROR(S))S=End(OpenVolume,EFI_DEVICE_ERROR);goto Recover;}
  Begin(Create);S=End(Create,Root->Open(Root,&File,L"SUNTEST.BIN",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0));if(EFI_ERROR(S) || File==NULL){if(!EFI_ERROR(S))S=End(Create,EFI_DEVICE_ERROR);goto Recover;}
  UINTN Bytes=FILE_BYTES;Begin(FileWrite);S=File->Write(File,&Bytes,mTx);mSteps[FileWrite].Bytes=Bytes;if(S==EFI_SUCCESS && Bytes!=FILE_BYTES)S=EFI_BAD_BUFFER_SIZE;S=End(FileWrite,S);if(EFI_ERROR(S))goto Recover;
  Begin(FileFlush);S=End(FileFlush,File->Flush(File));if(EFI_ERROR(S))goto Recover;
  Begin(FileClose);S=End(FileClose,File->Close(File));File=NULL;if(EFI_ERROR(S))goto Recover;
  Begin(Reopen);S=End(Reopen,Root->Open(Root,&Reader,L"SUNTEST.BIN",EFI_FILE_MODE_READ,0));if(EFI_ERROR(S) || Reader==NULL){if(!EFI_ERROR(S))S=End(Reopen,EFI_DEVICE_ERROR);goto Recover;}
  union {UINT64 Align;UINT8 Data[256];} InfoStorage;UINT8 *InfoBuffer=InfoStorage.Data;Bytes=sizeof(InfoStorage.Data);Begin(Info);S=Reader->GetInfo(Reader,&gEfiFileInfoGuid,&Bytes,InfoBuffer);EFI_FILE_INFO *F=(EFI_FILE_INFO *)InfoBuffer;
  if(S==EFI_SUCCESS && (Bytes<SIZE_OF_EFI_FILE_INFO || Bytes>sizeof(InfoStorage.Data) || F->Size< SIZE_OF_EFI_FILE_INFO || F->Size>Bytes || F->FileSize!=FILE_BYTES || (F->Attribute&EFI_FILE_DIRECTORY)))S=EFI_COMPROMISED_DATA;S=End(Info,S);if(EFI_ERROR(S))goto Recover;
  SetMem(mRx,sizeof(mRx),0xCC);Bytes=FILE_BYTES;Begin(FileRead);S=Reader->Read(Reader,&Bytes,mRx);mSteps[FileRead].Bytes=Bytes;
  if(S==EFI_SUCCESS && (Bytes!=FILE_BYTES || CompareMem(mTx,mRx,FILE_BYTES)))S=EFI_CRC_ERROR;S=End(FileRead,S);if(EFI_ERROR(S))goto Recover;mFileMatched=TRUE;
  UINT8 Byte;Bytes=1;Begin(Eof);S=Reader->Read(Reader,&Bytes,&Byte);if(S==EFI_SUCCESS && Bytes!=0)S=EFI_COMPROMISED_DATA;S=End(Eof,S);
Recover:
  if(Reader!=NULL){Begin(ReadClose);EFI_STATUS Close=End(ReadClose,Reader->Close(Reader));Reader=NULL;if(S==EFI_SUCCESS)S=Close;}
  if(File!=NULL){Begin(FileClose);EFI_STATUS Close=End(FileClose,File->Close(File));File=NULL;if(S==EFI_SUCCESS)S=Close;}
  if(Root!=NULL){Begin(RootClose);EFI_STATUS Close=End(RootClose,Root->Close(Root));Root=NULL;if(S==EFI_SUCCESS)S=Close;}
  Begin(CloseRecovery);EFI_STATUS Restore=End(CloseRecovery,PianoUfsBoundedTransportCloseForRecovery(&Recovery));if(EFI_ERROR(Restore))goto Unsafe;
  Begin(AcquireRecovery);Restore=End(AcquireRecovery,Recovery.Acquire(Recovery.Io.Context));if(EFI_ERROR(Restore))goto Unsafe;mRecoveryLease=TRUE;
  PIANO_UFS_WRITE_GUARD Guard;ZeroMem(&Guard,sizeof(Guard));Begin(RecoveryGate);Restore=Recovery.Io.ReadGuard(Recovery.Io.Context,4,&Guard);if(Restore==EFI_SUCCESS)Restore=PianoUfsWriteTestCheckGuard(&Guard);if(Restore==EFI_SUCCESS)Restore=Quiet(&Recovery);Restore=End(RecoveryGate,Restore);if(EFI_ERROR(Restore))goto Unsafe;
  // Private write/sync leaves also independently check full baseline and live
  // dual GPT before each mutation. Never restore across changed ownership.
  Begin(ScanRestore);
  for(EFI_LBA Lba=375040;Lba<=378623;++Lba){
    Restore=ExactRead(&Recovery,Lba,4096,mRestoreRx);if(EFI_ERROR(Restore))break;++mScanned;if(Zero(mRestoreRx))continue;++mNonzero;
    mLastRestore=Lba;mRestoreAttempted=TRUE;mRestoreReturned=FALSE;mRestoreWrite=mRestoreSync=mRestoreRead=mRestoreQuiet=EFI_NOT_STARTED;++mRestoreAttempts;mRestoreDone=0;
    UINTN Done=0;Restore=Norm(Recovery.Io.WriteFua(Recovery.Io.Context,4,Lba,4096,mZero,&Done));mRestoreReturned=TRUE;mRestoreDone=Done;if(Restore==EFI_SUCCESS && Done!=4096)Restore=EFI_BAD_BUFFER_SIZE;mRestoreWrite=Restore;
    mRestoreQuiet=Quiet(&Recovery);if(mRestoreQuiet!=EFI_SUCCESS)Restore=mRestoreQuiet;if(EFI_ERROR(Restore))break;
    Restore=Norm(Recovery.Io.Sync(Recovery.Io.Context,4,375040,14680064));mRestoreSync=Restore;mRestoreQuiet=Quiet(&Recovery);if(mRestoreQuiet!=EFI_SUCCESS)Restore=mRestoreQuiet;if(EFI_ERROR(Restore))break;
    Restore=ExactRead(&Recovery,Lba,4096,mRestoreRx);if(Restore==EFI_SUCCESS && !Zero(mRestoreRx))Restore=EFI_CRC_ERROR;mRestoreRead=Restore;if(EFI_ERROR(Restore))break;++mRestored;
  }
  mSteps[ScanRestore].Bytes=mScanned*4096;Restore=End(ScanRestore,Restore);if(EFI_ERROR(Restore))goto Unsafe;
  Begin(FinalSync);Restore=Recovery.Io.Sync(Recovery.Io.Context,4,375040,14680064);if(Restore==EFI_SUCCESS)Restore=Quiet(&Recovery);Restore=End(FinalSync,Restore);if(EFI_ERROR(Restore))goto Unsafe;
  Begin(VerifyWhole);Restore=End(VerifyWhole,PianoUfsBoundedTransportVerifyRestored());if(EFI_ERROR(Restore))goto Unsafe;mVerified=TRUE;
  Begin(ReleaseRecovery);Restore=End(ReleaseRecovery,Recovery.Release(Recovery.Io.Context,FALSE));mRecoveryLease=FALSE;if(EFI_ERROR(Restore))goto Unsafe;
  State=PianoUfsBoundedTransportState();if(State==NULL || State->Dirty || State->NeedsRecovery || State->Quarantined || !State->RestoreVerified){Restore=EFI_DEVICE_ERROR;goto Unsafe;}
  mFinal=mFirst==EFI_NOT_STARTED?EFI_SUCCESS:mFirst;mReturned=TRUE;mLease=FALSE;PianoReportBoundedFileSystemTest();gBS->RestoreTPL(Old);return mFinal;
Unsafe:
  // Retain the outer TPL and any recovery lease/Active DMA; no retry/reset/boot.
  mVerified=FALSE;mFinal=Restore==EFI_SUCCESS?EFI_DEVICE_ERROR:Restore;PianoReportBoundedFileSystemTest();CpuDeadLoop();return mFinal;
BeforeLease:
  if(mFirst==EFI_NOT_STARTED)mFirst=S;mFinal=S;mReturned=TRUE;PianoReportBoundedFileSystemTest();return S;
}
