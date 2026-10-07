// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoCpuImageLoan.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/SynchronizationLib.h>
STATIC volatile UINT64 mDriverLoanSequence;
STATIC EFI_STATUS ReserveToken(UINTN *Sequence){
  for(UINTN Retry=0;Retry<64;++Retry){UINT64 Old=InterlockedCompareExchange64(&mDriverLoanSequence,0,0);
    if(Old>=MAX_UINTN)return EFI_OUT_OF_RESOURCES;
    if(InterlockedCompareExchange64(&mDriverLoanSequence,Old,Old+1)==Old){*Sequence=(UINTN)(Old+1);return EFI_SUCCESS;}
  }return EFI_NOT_READY;
}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS Retain(PIANO_CPU_IMAGE_LOAN *S,EFI_STATUS Status){S->Retained=TRUE;S->Busy=FALSE;return S->Status=Exact(Status);}
STATIC BOOLEAN Valid(PIANO_CPU_IMAGE_LOAN *S,VOID *Token){return S && S->Signature==PIANO_CPU_IMAGE_LOAN_SIGNATURE && S->Active && !S->Retained && !S->Busy && Token && Token==S->Token && S->Owner && S->SourceLoan && S->View;}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN AN,CONST VOID *B,UINTN BN){UINTN X=(UINTN)A,Y=(UINTN)B;return X>MAX_UINTN-AN || Y>MAX_UINTN-BN || (X<Y+BN && Y<X+AN);}
EFI_STATUS PianoCpuImageLoanInitialize(PIANO_CPU_IMAGE_LOAN *S){
  if(!S)return EFI_INVALID_PARAMETER;if(S->Signature)return EFI_ALREADY_STARTED;
  ZeroMem(S,sizeof(*S));S->Signature=PIANO_CPU_IMAGE_LOAN_SIGNATURE;return EFI_SUCCESS;
}
EFI_STATUS PianoCpuImageLoanBegin(PIANO_CPU_IMAGE_LOAN *S,CONST PIANO_LAUNCH_BLOB *B,VOID *Owner,
  PIANO_BOOT_RANGE Range,UINT64 MaxBytes,CONST VOID **View,VOID **Token){
  return PianoCpuImageLoanBeginWithCpu(S,B,Owner,Range,MaxBytes,NULL,View,Token);
}
EFI_STATUS PianoCpuImageLoanBeginWithCpu(PIANO_CPU_IMAGE_LOAN *S,CONST PIANO_LAUNCH_BLOB *B,VOID *Owner,
  PIANO_BOOT_RANGE Range,UINT64 MaxBytes,CONST PIANO_CPU_INPUT_ENV *Cpu,CONST VOID **View,VOID **Token){
  if(!View || !Token || Overlap(View,sizeof(*View),Token,sizeof(*Token)))return EFI_INVALID_PARAMETER;
  // Do not clear caller outputs until source alias checks are possible. They
  // may themselves point into state/owned data; rejection must not mutate it.
  if(!S || S->Signature!=PIANO_CPU_IMAGE_LOAN_SIGNATURE || Overlap(View,sizeof(*View),S,sizeof(*S)) ||
     Overlap(Token,sizeof(*Token),S,sizeof(*S)) || !B || !Owner || !B->BorrowView || !B->Unborrow || !B->Bytes ||
     Overlap(View,sizeof(*View),B,sizeof(*B)) || Overlap(Token,sizeof(*Token),B,sizeof(*B)) ||
     !MaxBytes || !Range.Bytes || Range.Bytes>MAX_UINTN || Range.Bytes>MaxBytes || Range.Offset>B->Bytes || Range.Bytes>B->Bytes-Range.Offset)return EFI_INVALID_PARAMETER;
  if(MaxBytes>PIANO_CPU_INPUT_MAX_BYTES)return EFI_BAD_BUFFER_SIZE;
  if(Cpu&&(Overlap(Cpu,sizeof(*Cpu),S,sizeof(*S))||Overlap(Cpu,sizeof(*Cpu),B,sizeof(*B))))return EFI_INVALID_PARAMETER;
  if(S->Active || S->Retained || S->Busy)return EFI_ACCESS_DENIED;
  S->Busy=TRUE;EFI_STATUS Capacity=PianoCpuInputAuthorize(Cpu,Range.Bytes,&S->Memory);if(Capacity!=EFI_SUCCESS){S->Busy=FALSE;return Capacity;}
  S->HasCpu=Cpu!=NULL;if(Cpu)S->Cpu=*Cpu;
  UINTN Sequence=0;EFI_STATUS Reserved=ReserveToken(&Sequence);if(Reserved!=EFI_SUCCESS){S->Busy=FALSE;return Reserved;}
  S->Blob=*B;S->Owner=Owner;S->Range=Range;S->View=NULL;S->SourceLoan=NULL;
  EFI_STATUS Raw=S->Blob.BorrowView(S->Blob.Context,Owner,Range,&S->View,&S->SourceLoan);
  if(Raw!=EFI_SUCCESS){if(S->View || S->SourceLoan || !EFI_ERROR(Raw))return Retain(S,Raw);S->Owner=NULL;S->Busy=FALSE;return S->Status=Raw;}
  S->Active=TRUE;
  if(!S->View || !S->SourceLoan || Overlap(S->View,(UINTN)Range.Bytes,S,sizeof(*S)))return Retain(S,EFI_COMPROMISED_DATA);
  if(Overlap(View,sizeof(*View),S->View,(UINTN)Range.Bytes) || Overlap(Token,sizeof(*Token),S->View,(UINTN)Range.Bytes)){
    EFI_STATUS End=Exact(S->Blob.Unborrow(S->Blob.Context,Owner,S->SourceLoan));if(End!=EFI_SUCCESS)return Retain(S,End);
    S->Active=FALSE;S->Owner=S->SourceLoan=NULL;S->View=NULL;S->Busy=FALSE;return S->Status=EFI_INVALID_PARAMETER;
  }
  EFI_STATUS Validated=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,B->Context,Owner,S->View,Range.Bytes);
  if(Validated==EFI_SUCCESS)Validated=PianoCpuInputHash(S->HasCpu?&S->Cpu:NULL,S->View,(UINTN)Range.Bytes,S->Sha256);
  if(Validated==EFI_SUCCESS)Validated=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,B->Context,Owner,S->View,Range.Bytes);
  if(Validated!=EFI_SUCCESS)return Retain(S,Validated);
  S->Sequence=Sequence;S->Token=(VOID *)Sequence;*View=S->View;*Token=S->Token;S->Busy=FALSE;return S->Status=EFI_SUCCESS;
}
EFI_STATUS PianoCpuImageLoanRead(PIANO_CPU_IMAGE_LOAN *S,VOID *Token,UINT64 Offset,UINTN Bytes,VOID *Buffer){
  if(!Valid(S,Token))return EFI_ACCESS_DENIED;if(Offset>S->Range.Bytes || Bytes>S->Range.Bytes-Offset)return EFI_BAD_BUFFER_SIZE;
  if(!Bytes)return EFI_SUCCESS;
  if(!Buffer || Overlap(Buffer,Bytes,S,sizeof(*S)) || Overlap(Buffer,Bytes,S->View,(UINTN)S->Range.Bytes))return EFI_INVALID_PARAMETER;
  S->Busy=TRUE;EFI_STATUS Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,S->Blob.Context,S->Owner,S->View,S->Range.Bytes);
  if(Status==EFI_SUCCESS)Status=PianoCpuInputCopy(S->HasCpu?&S->Cpu:NULL,Buffer,(CONST UINT8 *)S->View+(UINTN)Offset,Bytes);
  if(Status==EFI_SUCCESS)Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,S->Blob.Context,S->Owner,S->View,S->Range.Bytes);
  S->Busy=FALSE;
  return Status;
}
EFI_STATUS PianoCpuImageLoanRevalidate(PIANO_CPU_IMAGE_LOAN *S,VOID *Token){
  if(!Valid(S,Token))return EFI_ACCESS_DENIED;UINT8 Hash[32];
  S->Busy=TRUE;EFI_STATUS Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,S->Blob.Context,S->Owner,S->View,S->Range.Bytes);
  if(Status==EFI_SUCCESS)Status=PianoCpuInputHash(S->HasCpu?&S->Cpu:NULL,S->View,(UINTN)S->Range.Bytes,Hash);
  if(Status==EFI_SUCCESS)Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->Memory,S->Blob.Context,S->Owner,S->View,S->Range.Bytes);
  if(Status!=EFI_SUCCESS)return Retain(S,Status);
  if(CompareMem(Hash,S->Sha256,32))return Retain(S,EFI_CRC_ERROR);S->Busy=FALSE;return S->Status=EFI_SUCCESS;
}
EFI_STATUS PianoCpuImageLoanEnd(PIANO_CPU_IMAGE_LOAN *S,VOID *Token){
  if(!Valid(S,Token))return EFI_ACCESS_DENIED;
  EFI_STATUS Status=Exact(S->Blob.Unborrow(S->Blob.Context,S->Owner,S->SourceLoan));
  if(Status!=EFI_SUCCESS)return Retain(S,Status);
  S->Active=FALSE;S->Owner=S->SourceLoan=S->Token=NULL;S->View=NULL;ZeroMem(S->Sha256,sizeof(S->Sha256));
  return S->Status=EFI_SUCCESS;
}
