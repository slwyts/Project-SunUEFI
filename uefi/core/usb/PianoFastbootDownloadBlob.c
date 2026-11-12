// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFastbootDownloadBlob.h"
#include <Library/BaseMemoryLib.h>

#define BLOB_SIGNATURE SIGNATURE_32('F','B','D','B')
STATIC CONST PIANO_CPU_INPUT_ENV *Cpu(PIANO_FASTBOOT_DOWNLOAD_BLOB *S){return S->HasCpu?&S->Cpu:NULL;}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){UINTN X=(UINTN)A,Y=(UINTN)B;return X>MAX_UINTN-An||Y>MAX_UINTN-Bn||(X<Y+Bn&&Y<X+An);}
STATIC BOOLEAN Output(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,CONST VOID *P,UINTN N){return P&&N&&!Overlap(P,N,S,sizeof(*S))&&
  (!S->Source||!Overlap(P,N,S->Source,sizeof(*S->Source)))&&(!S->Owned||!Overlap(P,N,S->Owned,S->Bytes));}
STATIC EFI_STATUS Retain(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,EFI_STATUS E){S->Retained=TRUE;S->Busy=FALSE;return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS TakeDone(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,EFI_STATUS E){S->Busy=FALSE;return E;}

STATIC BOOLEAN DownloadReady(CONST PIANO_FASTBOOT *S) {
  return S!=NULL && S->Download!=NULL && S->Complete && !S->Receiving &&
    !S->RebootRequested && !S->ExitRequested && S->Expected!=0 &&
    // Adapter source ceiling only. Packet admission/advertisement still uses
    // the unchanged64MiB PIANO_FASTBOOT_MAX_DOWNLOAD in the transport engine.
    S->Expected<=PIANO_CPU_INPUT_MAX_BYTES && S->Received==S->Expected &&
    S->UploadBorrowed && S->Upload==S->Download && S->UploadBytes==S->Received;
}
STATIC BOOLEAN Owned(CONST PIANO_FASTBOOT_DOWNLOAD_BLOB *S,CONST VOID *Owner) {
  return S!=NULL && S->Signature==BLOB_SIGNATURE && Owner==S &&
    S->Taken && !S->Consumed && !S->ReleaseAttempted && !S->Busy && !S->Retained && S->Owned!=NULL;
}
STATIC EFI_STATUS Take(VOID *Context,VOID **Owner) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(S==NULL||!Output(S,Owner,sizeof(*Owner)))return EFI_INVALID_PARAMETER;
  *Owner=NULL;
  if(S==NULL || S->Signature!=BLOB_SIGNATURE)return EFI_INVALID_PARAMETER;
  if(S->Taken || S->Consumed || S->Busy || S->Retained)return EFI_ALREADY_STARTED;
  if(!DownloadReady(S->Source) || S->Source->Received!=S->Bytes ||
     S->Source->Download!=S->BoundDownload)return EFI_NOT_READY;
  S->Busy=TRUE;
  EFI_STATUS Status=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->BoundDownload,S->Bytes);
  if(Status!=EFI_SUCCESS)return TakeDone(S,Status);
  if(!DownloadReady(S->Source)||S->Source->Received!=S->Bytes||S->Source->Download!=S->BoundDownload)return TakeDone(S,EFI_NOT_READY);
  Status=S->ReadyToTake(S->QuietContext);
  if(Status!=EFI_SUCCESS)return TakeDone(S,Status);
  // The acknowledgement may have changed source state. Recheck before moving
  // ownership; after this point ordinary FastbootReset cannot free our pool.
  if(!DownloadReady(S->Source) || S->Source->Received!=S->Bytes ||
     S->Source->Download!=S->BoundDownload)return TakeDone(S,EFI_NOT_READY);
  Status=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->BoundDownload,S->Bytes);
  if(Status!=EFI_SUCCESS)return TakeDone(S,Status);
  if(!DownloadReady(S->Source)||S->Source->Received!=S->Bytes||S->Source->Download!=S->BoundDownload)return TakeDone(S,EFI_NOT_READY);
  PIANO_FASTBOOT *F=S->Source;
  S->Owned=F->Download;S->Taken=TRUE;
  F->Download=NULL;F->Upload=NULL;F->UploadBytes=0;F->UploadBorrowed=FALSE;
  F->Expected=0;F->Received=0;F->Receiving=FALSE;F->Complete=FALSE;
  *Owner=S;return TakeDone(S,EFI_SUCCESS);
}
STATIC EFI_STATUS Read(VOID *Context,VOID *Owner,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(!Owned(S,Owner) || !Output(S,Buffer,Bytes))return EFI_INVALID_PARAMETER;
  if(Offset>S->Bytes || Bytes>S->Bytes-Offset)return EFI_BAD_BUFFER_SIZE;
  S->Busy=TRUE;EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,Owner,S->Owned,S->Bytes);
  if(E==EFI_SUCCESS)E=PianoCpuInputCopy(Cpu(S),Buffer,S->Owned+(UINTN)Offset,Bytes);
  if(E==EFI_SUCCESS)E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,Owner,S->Owned,S->Bytes);
  S->Busy=FALSE;return E;
}
STATIC EFI_STATUS Borrow(VOID *Context,VOID *Owner,PIANO_BOOT_RANGE Range,CONST VOID **View,VOID **Loan) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(S==NULL||!Output(S,View,sizeof(*View))||!Output(S,Loan,sizeof(*Loan))||Overlap(View,sizeof(*View),Loan,sizeof(*Loan)))return EFI_INVALID_PARAMETER;
  *View=NULL;*Loan=NULL;
  if(!Owned(S,Owner))return EFI_INVALID_PARAMETER;
  if(S->ActiveLoan!=NULL)return EFI_ALREADY_STARTED;
  if(Range.Bytes==0 || Range.Offset>S->Bytes || Range.Bytes>S->Bytes-Range.Offset)return EFI_BAD_BUFFER_SIZE;
  EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,Owner,S->Owned,S->Bytes);if(E!=EFI_SUCCESS)return E;
  if(S->LoanSequence==MAX_UINTN)return EFI_OUT_OF_RESOURCES;
  // Opaque non-dereferenced token. A stale token cannot release a later loan.
  S->LoanSequence++;S->ActiveLoan=(VOID *)S->LoanSequence;
  *View=S->Owned+(UINTN)Range.Offset;*Loan=S->ActiveLoan;return EFI_SUCCESS;
}
STATIC EFI_STATUS Unborrow(VOID *Context,VOID *Owner,VOID *Loan) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(!Owned(S,Owner) || Loan==NULL || Loan!=S->ActiveLoan)return EFI_INVALID_PARAMETER;
  S->ActiveLoan=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS Restore(VOID *Context,VOID *Owner) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(!Owned(S,Owner))return EFI_INVALID_PARAMETER;
  if(S->ActiveLoan!=NULL)return EFI_ACCESS_DENIED;
  EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,Owner,S->Owned,S->Bytes);if(E!=EFI_SUCCESS)return E;
  PIANO_FASTBOOT *F=S->Source;
  if(F==NULL || F->Download!=NULL || F->Upload!=NULL || F->UploadBytes!=0 || F->UploadBorrowed ||
     F->Expected!=0 || F->Received!=0 || F->Complete || F->Receiving ||
     F->RebootRequested || F->ExitRequested)return EFI_ACCESS_DENIED;
  F->Download=S->Owned;F->Expected=S->Bytes;F->Received=S->Bytes;F->Complete=TRUE;
  F->Upload=S->Owned;F->UploadBytes=S->Bytes;F->UploadBorrowed=TRUE;
  S->Owned=NULL;S->BoundDownload=NULL;S->Taken=FALSE;S->Consumed=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS ZeroRelease(VOID *Context,VOID *Owner) {
  PIANO_FASTBOOT_DOWNLOAD_BLOB *S=Context;
  if(!Owned(S,Owner))return EFI_INVALID_PARAMETER;
  if(S->ActiveLoan!=NULL)return EFI_ACCESS_DENIED;
  // A VOID MemoryAllocationLib FreePool cannot acknowledge resource release.
  // Once attempted, failure/unknown ownership fences all source access and
  // retries: the payload is zeroed and cannot be restored as a complete image.
  S->Busy=TRUE;EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,Owner,S->Owned,S->Bytes);
  if(E!=EFI_SUCCESS)return Retain(S,E);
  S->ReleaseAttempted=TRUE;
  E=PianoCpuInputZero(Cpu(S),S->Owned,S->Bytes);if(E!=EFI_SUCCESS)return Retain(S,E);
  S->ReleaseStatus=S->ReleasePool(S->Owned);
  if(S->ReleaseStatus!=EFI_SUCCESS)
    return Retain(S,S->ReleaseStatus);
  S->Owned=NULL;S->BoundDownload=NULL;S->Taken=FALSE;S->Consumed=TRUE;S->Busy=FALSE;return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootDownloadBlobBind(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,
  PIANO_FASTBOOT *F,VOID *QuietContext,PIANO_DOWNLOAD_QUIET ReadyToTake,EFI_FREE_POOL ReleasePool,PIANO_LAUNCH_BLOB *Blob) {
  return PianoFastbootDownloadBlobBindWithCpu(S,F,QuietContext,ReadyToTake,ReleasePool,NULL,Blob);
}
EFI_STATUS PianoFastbootDownloadBlobBindWithCpu(PIANO_FASTBOOT_DOWNLOAD_BLOB *S,
  PIANO_FASTBOOT *F,VOID *QuietContext,PIANO_DOWNLOAD_QUIET ReadyToTake,EFI_FREE_POOL ReleasePool,
  CONST PIANO_CPU_INPUT_ENV *Config,PIANO_LAUNCH_BLOB *Blob) {
  if(S==NULL || F==NULL || ReadyToTake==NULL || ReleasePool==NULL || Blob==NULL)return EFI_INVALID_PARAMETER;
  if(Overlap(Blob,sizeof(*Blob),S,sizeof(*S))||Overlap(Blob,sizeof(*Blob),F,sizeof(*F))||
     (Config&&(Overlap(Config,sizeof(*Config),S,sizeof(*S))||Overlap(Config,sizeof(*Config),F,sizeof(*F)))))return EFI_INVALID_PARAMETER;
  if(S->Signature!=0||S->Busy||S->Retained)return EFI_ALREADY_STARTED;
  if(!DownloadReady(F))return EFI_NOT_READY;
  if(Overlap(Blob,sizeof(*Blob),F->Download,F->Received))return EFI_INVALID_PARAMETER;
  PIANO_CPU_INPUT_ENV Saved={0};if(Config)Saved=*Config;PIANO_LINUX_MEMORY_PROOF Memory;
  EFI_STATUS Capacity=PianoCpuInputAuthorize(Config?&Saved:NULL,F->Received,&Memory);if(Capacity!=EFI_SUCCESS)return Capacity;
  EFI_STATUS Status=ReadyToTake(QuietContext);
  if(Status!=EFI_SUCCESS)return Status;
  if(!DownloadReady(F))return EFI_NOT_READY;
  *S=(PIANO_FASTBOOT_DOWNLOAD_BLOB){.Signature=BLOB_SIGNATURE,.Source=F,
    .QuietContext=QuietContext,.ReadyToTake=ReadyToTake,.ReleasePool=ReleasePool,
    .BoundDownload=F->Download,.Bytes=F->Received,.Cpu=Saved,.Memory=Memory,.HasCpu=Config!=NULL};
  Status=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->BoundDownload,S->Bytes);
  if(Status!=EFI_SUCCESS)return Retain(S,Status);
  *Blob=(PIANO_LAUNCH_BLOB){.Context=S,.Bytes=S->Bytes,.Take=Take,.Read=Read,
    .BorrowView=Borrow,.Unborrow=Unborrow,.Restore=Restore,.ZeroRelease=ZeroRelease};
  return EFI_SUCCESS;
}
