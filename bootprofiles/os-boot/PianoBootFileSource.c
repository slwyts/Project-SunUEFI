// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit SFS file -> stable readonly CPU blob; no OS launch or storage write.
#include "PianoBootFileSource.h"
#include <Guid/FileInfo.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#define SIGNATURE SIGNATURE_32('P','B','F','S')
STATIC UINTN mToken;
STATIC BOOLEAN Range(CONST VOID *P,UINTN N,UINTN *First,UINTN *Last){if(!P||!N)return FALSE;*First=(UINTN)P;if(N-1>MAX_UINTN-*First)return FALSE;*Last=*First+N-1;return TRUE;}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN AN,CONST VOID *B,UINTN BN){UINTN AF,AL,BF,BL;return !Range(A,AN,&AF,&AL)||!Range(B,BN,&BF,&BL)||(AF<=BL&&BF<=AL);}
// Every caller output is outside the producer and its immutable snapshot.
// A read into Data or an Owner/Loan output into State would otherwise mutate it.
STATIC BOOLEAN Output(PIANO_BOOT_FILE_SOURCE *S,CONST VOID *P,UINTN N){return S&&!Overlap(P,N,S,sizeof(*S))&&(!S->Data||!Overlap(P,N,S->Data,S->Bytes));}
STATIC BOOLEAN Exports(PIANO_BOOT_FILE_SOURCE *S,PIANO_BOOT_SOURCE *Source,PIANO_LAUNCH_BLOB *Blob){return Output(S,Source,sizeof(*Source))&&Output(S,Blob,sizeof(*Blob))&&!Overlap(Source,sizeof(*Source),Blob,sizeof(*Blob));}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Live(PIANO_BOOT_FILE_SOURCE *S){return !S->ServicesLost&&S->Env.BootServicesAlive&&S->Env.BootServicesAlive(S->Env.Context)==TRUE;}
STATIC VOID EFIAPI ExitNotify(EFI_EVENT E,VOID *Context){(VOID)E;PIANO_BOOT_FILE_SOURCE *S=Context;S->ServicesLost=S->Retained=TRUE;S->Ready=FALSE;}
STATIC EFI_STATUS Retain(PIANO_BOOT_FILE_SOURCE *S,EFI_STATUS Status){S->Retained=TRUE;S->Ready=FALSE;return S->Status=Exact(Status);}
STATIC EFI_STATUS Path(CHAR16 *Out,CONST CHAR16 *In){
  if(!In||In[0]!='\\'||In[1]==0||In[1]=='\\')return EFI_INVALID_PARAMETER;
  UINTN Start=1;for(UINTN I=0;I<PIANO_BOOT_FILE_PATH_CHARS;++I){CHAR16 C=In[I];Out[I]=C;
    if(!C){if(I==Start)return EFI_INVALID_PARAMETER;UINTN N=I-Start;if((N==1&&In[Start]=='.')||(N==2&&In[Start]=='.'&&In[Start+1]=='.'))return EFI_INVALID_PARAMETER;return EFI_SUCCESS;}
    if(C<32||C=='/'||C==':'||C=='*'||C=='?'||C=='"'||C=='<'||C=='>'||C=='|')return EFI_INVALID_PARAMETER;
    if(C=='\\'&&I){UINTN N=I-Start;if(!N||(N==1&&In[Start]=='.')||(N==2&&In[Start]=='.'&&In[Start+1]=='.'))return EFI_INVALID_PARAMETER;Start=I+1;}
  }return EFI_BAD_BUFFER_SIZE;
}
STATIC EFI_STATUS Fresh(PIANO_BOOT_FILE_SOURCE *S){
  if(!Live(S))return Retain(S,EFI_ABORTED);
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F=NULL;EFI_STATUS E=S->Env.Services->HandleProtocol(S->FileSystem,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&F);
  if(!Live(S))return Retain(S,EFI_ABORTED);
  if(E!=EFI_SUCCESS||F!=S->Sfs||!F||!F->OpenVolume)return Retain(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
  if(S->File&&(S->File->Read!=S->ReadFile||S->File->SetPosition!=S->SeekFile||S->File->GetPosition!=S->PositionFile||S->File->GetInfo!=S->InfoFile||S->File->Close!=S->CloseFile))return Retain(S,EFI_COMPROMISED_DATA);
  if(S->Root&&S->Root->Close!=S->CloseRoot)return Retain(S,EFI_COMPROMISED_DATA);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Information(PIANO_BOOT_FILE_SOURCE *S,BOOLEAN Initial,UINT64 *Bytes){
  ZeroMem(S->Info,sizeof(S->Info));UINTN Count=sizeof(S->Info);EFI_STATUS E=S->InfoFile(S->File,&gEfiFileInfoGuid,&Count,S->Info);
  if(!Live(S))return Retain(S,EFI_ABORTED);if(E!=EFI_SUCCESS)return Exact(E);
  if(Count<SIZE_OF_EFI_FILE_INFO+2||Count>sizeof(S->Info)||(Count&1))return EFI_COMPROMISED_DATA;
  EFI_FILE_INFO *Info=(EFI_FILE_INFO *)S->Info;
  if(Info->Size!=Count||(Info->Attribute&EFI_FILE_DIRECTORY)||!Info->FileSize)return EFI_COMPROMISED_DATA;
  UINTN N=(Count-SIZE_OF_EFI_FILE_INFO)/2,End=0;while(End<N&&Info->FileName[End])++End;if(End==N)return EFI_COMPROMISED_DATA;
  CONST CHAR16 *Leaf=S->Path;for(UINTN I=0;S->Path[I];++I)if(S->Path[I]=='\\')Leaf=S->Path+I+1;
  UINTN LeafBytes=0;while(Leaf[LeafBytes])++LeafBytes;if(End!=LeafBytes)return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<=End;++I){CHAR16 A=Info->FileName[I],B=Leaf[I];if(A>='a'&&A<='z')A-=32;if(B>='a'&&B<='z')B-=32;if(A!=B)return EFI_COMPROMISED_DATA;}
  // A readonly provider may update access-time cache as a read side effect;
  // logical/physical size, attributes, filename and creation/modification stay stable.
  ZeroMem(&Info->LastAccessTime,sizeof(Info->LastAccessTime));
  if(Initial){S->InfoBytes=Count;CopyMem(S->InitialInfo,S->Info,Count);}
  else if(Count!=S->InfoBytes||CompareMem(S->InitialInfo,S->Info,Count))return EFI_MEDIA_CHANGED;
  *Bytes=Info->FileSize;return EFI_SUCCESS;
}
STATIC EFI_STATUS DiskRead(PIANO_BOOT_FILE_SOURCE *S,UINT64 Offset,UINTN Bytes,VOID *Buffer){
  EFI_STATUS E=Fresh(S);if(E!=EFI_SUCCESS)return E;
  E=S->SeekFile(S->File,Offset);if(!Live(S))return Retain(S,EFI_ABORTED);if(E!=EFI_SUCCESS)return Exact(E);
  UINT64 Position=0;E=S->PositionFile(S->File,&Position);if(!Live(S))return Retain(S,EFI_ABORTED);if(E!=EFI_SUCCESS||Position!=Offset)return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
  UINTN Count=Bytes;E=S->ReadFile(S->File,&Count,Buffer);if(!Live(S))return Retain(S,EFI_ABORTED);if(E!=EFI_SUCCESS)return Exact(E);
  if(Count!=Bytes)return EFI_BAD_BUFFER_SIZE;E=S->PositionFile(S->File,&Position);if(!Live(S))return Retain(S,EFI_ABORTED);
  return E==EFI_SUCCESS&&Position==Offset+Bytes?EFI_SUCCESS:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
}
STATIC EFI_STATUS CloseFiles(PIANO_BOOT_FILE_SOURCE *S){
  if(S->Retained||!Live(S))return Retain(S,EFI_ABORTED);
  if(!S->File&&!S->Root)return EFI_SUCCESS;
  EFI_STATUS E=Fresh(S);if(E!=EFI_SUCCESS)return E;
  S->CloseAttempted=TRUE;
  if(S->File){E=S->CloseFile(S->File);S->FileClose=E;if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);S->File=NULL;}
  E=Fresh(S);if(E!=EFI_SUCCESS)return E;
  if(S->Root){E=S->CloseRoot(S->Root);S->RootClose=E;if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);S->Root=NULL;}
  return EFI_SUCCESS;
}
STATIC BOOLEAN Valid(PIANO_BOOT_FILE_SOURCE *S){return S&&S->Signature==SIGNATURE&&S->Ready&&!S->Consumed&&!S->Retained&&Live(S);}
STATIC CONST PIANO_CPU_INPUT_ENV *Cpu(PIANO_BOOT_FILE_SOURCE *S){return S->HasCpu?&S->Cpu:NULL;}
STATIC EFI_STATUS Reader(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer){
  PIANO_BOOT_FILE_SOURCE *S=Context;if(!Valid(S)||!Output(S,Buffer,Bytes))return EFI_INVALID_PARAMETER;
  if(Offset>S->Bytes||Bytes>S->Bytes-Offset)return EFI_BAD_BUFFER_SIZE;if(S->Busy)return EFI_ALREADY_STARTED;
  S->Busy=TRUE;EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,S->Owner,S->Data,S->Bytes);
  if(E==EFI_SUCCESS)E=PianoCpuInputCopy(Cpu(S),Buffer,S->Data+(UINTN)Offset,Bytes);
  if(E==EFI_SUCCESS)E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,S->Owner,S->Data,S->Bytes);
  S->Busy=FALSE;return Live(S)?E:Retain(S,EFI_ABORTED);
}
STATIC EFI_STATUS Take(VOID *Context,VOID **Owner){PIANO_BOOT_FILE_SOURCE *S=Context;if(!Output(S,Owner,sizeof(*Owner)))return EFI_INVALID_PARAMETER;*Owner=NULL;if(!Valid(S)||S->Busy||S->Taken)return EFI_NOT_READY;if(mToken==MAX_UINTN)return EFI_OUT_OF_RESOURCES;S->Owner=(VOID *)++mToken;S->Taken=TRUE;*Owner=S->Owner;return EFI_SUCCESS;}
STATIC BOOLEAN Owned(PIANO_BOOT_FILE_SOURCE *S,VOID *Owner){return Valid(S)&&!S->Busy&&S->Taken&&Owner&&Owner==S->Owner&&!S->ReleaseAttempted;}
STATIC EFI_STATUS Read(VOID *Context,VOID *Owner,UINT64 Offset,UINTN Bytes,VOID *Buffer){PIANO_BOOT_FILE_SOURCE *S=Context;return Owned(S,Owner)?Reader(S,Offset,Bytes,Buffer):EFI_ACCESS_DENIED;}
STATIC EFI_STATUS Borrow(VOID *Context,VOID *Owner,PIANO_BOOT_RANGE Range,CONST VOID **View,VOID **Loan){PIANO_BOOT_FILE_SOURCE *S=Context;if(!Output(S,View,sizeof(*View))||!Output(S,Loan,sizeof(*Loan))||Overlap(View,sizeof(*View),Loan,sizeof(*Loan)))return EFI_INVALID_PARAMETER;*View=NULL;*Loan=NULL;if(!Owned(S,Owner)||S->Loan)return EFI_ACCESS_DENIED;if(!Range.Bytes||Range.Offset>S->Bytes||Range.Bytes>S->Bytes-Range.Offset)return EFI_BAD_BUFFER_SIZE;if(mToken==MAX_UINTN)return EFI_OUT_OF_RESOURCES;S->Loan=(VOID *)++mToken;*Loan=S->Loan;*View=S->Data+(UINTN)Range.Offset;return EFI_SUCCESS;}
STATIC EFI_STATUS Unborrow(VOID *Context,VOID *Owner,VOID *Loan){PIANO_BOOT_FILE_SOURCE *S=Context;if(!Owned(S,Owner)||!Loan||Loan!=S->Loan)return EFI_INVALID_PARAMETER;S->Loan=NULL;return EFI_SUCCESS;}
STATIC EFI_STATUS Restore(VOID *Context,VOID *Owner){PIANO_BOOT_FILE_SOURCE *S=Context;if(!Owned(S,Owner)||S->Loan)return EFI_ACCESS_DENIED;S->Taken=FALSE;S->Owner=NULL;return EFI_SUCCESS;}
STATIC EFI_STATUS Release(PIANO_BOOT_FILE_SOURCE *S){
  if(!Live(S))return Retain(S,EFI_ABORTED);
  if(S->Retained||S->ReleaseAttempted||S->Loan||S->File||S->Root)return EFI_ACCESS_DENIED;
  S->ReleaseAttempted=TRUE;
  if(S->Exit){EFI_STATUS E=S->Env.Services->CloseEvent(S->Exit);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);S->Exit=NULL;}
  if(S->Data){
    EFI_STATUS E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,S->Owner,S->Data,S->Bytes);
    if(E==EFI_SUCCESS)E=PianoCpuInputZero(Cpu(S),S->Data,S->Bytes);
    if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
    S->Release=S->Env.Services->FreePool(S->Data);if(!Live(S)||S->Release!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:S->Release);S->Data=NULL;
  }
  S->Ready=FALSE;S->Consumed=TRUE;S->Owner=NULL;S->Taken=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS ZeroRelease(VOID *Context,VOID *Owner){PIANO_BOOT_FILE_SOURCE *S=Context;return Owned(S,Owner)?Release(S):EFI_ACCESS_DENIED;}
EFI_STATUS PianoBootFileDispose(PIANO_BOOT_FILE_SOURCE *S){if(!S||S->Signature!=SIGNATURE)return EFI_INVALID_PARAMETER;if(S->Consumed)return EFI_SUCCESS;if(S->Busy||S->Taken||S->Loan)return EFI_ACCESS_DENIED;return Release(S);}
EFI_STATUS PianoBootFileExport(PIANO_BOOT_FILE_SOURCE *S,PIANO_BOOT_SOURCE *Source,PIANO_LAUNCH_BLOB *Blob){
  if(!Exports(S,Source,Blob))return EFI_INVALID_PARAMETER;ZeroMem(Source,sizeof(*Source));ZeroMem(Blob,sizeof(*Blob));if(!Valid(S)||S->Busy)return EFI_NOT_READY;
  *Source=(PIANO_BOOT_SOURCE){S,Reader,S->Bytes};*Blob=(PIANO_LAUNCH_BLOB){S,S->Bytes,Take,Read,Borrow,Unborrow,Restore,ZeroRelease};return EFI_SUCCESS;
}
EFI_STATUS PianoBootFileLoad(PIANO_BOOT_FILE_SOURCE *S,CONST PIANO_BOOT_FILE_ENV *Env,CONST PIANO_BOOT_FILE_SPEC *Spec,PIANO_BOOT_SOURCE *Source,PIANO_LAUNCH_BLOB *Blob){
  if(!S||!Env||!Spec||!Exports(S,Source,Blob)||!Spec->FileSystem||!Spec->MaxBytes||Spec->MaxBytes>PIANO_CPU_INPUT_MAX_BYTES)return EFI_INVALID_PARAMETER;
  if(Spec->MaxBytes>PIANO_BOOT_FILE_LOW_BUDGET&&!Env->Cpu)return EFI_NOT_READY;
  if(Env->Cpu&&Overlap(Env->Cpu,sizeof(*Env->Cpu),S,sizeof(*S)))return EFI_INVALID_PARAMETER;
  ZeroMem(Source,sizeof(*Source));ZeroMem(Blob,sizeof(*Blob));if(S->Signature)return EFI_ALREADY_STARTED;
  if(!Env->Services||!Env->BootServicesAlive||!Env->Services->HandleProtocol||!Env->Services->AllocatePool||!Env->Services->FreePool||!Env->Services->RaiseTPL||!Env->Services->RestoreTPL||!Env->Services->CreateEventEx||!Env->Services->CloseEvent)return EFI_UNSUPPORTED;
  S->Env=*Env;if(!Live(S))return EFI_NOT_READY;EFI_TPL T=Env->Services->RaiseTPL(TPL_HIGH_LEVEL);Env->Services->RestoreTPL(T);if(!Live(S))return EFI_ABORTED;if(T!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  if(Env->Cpu){S->Cpu=*Env->Cpu;S->HasCpu=TRUE;S->Env.Cpu=&S->Cpu;}
  EFI_STATUS Capacity=PianoCpuInputAuthorize(Cpu(S),Spec->MaxBytes,&S->Memory);
  if(Capacity!=EFI_SUCCESS)return Capacity;
  EFI_STATUS E=Path(S->Path,Spec->AbsolutePath);if(E!=EFI_SUCCESS)return E;
  S->Signature=SIGNATURE;S->Busy=TRUE;S->FileSystem=Spec->FileSystem;S->HasExpected=Spec->ExpectedSha256!=NULL;if(S->HasExpected)CopyMem(S->ExpectedSha256,Spec->ExpectedSha256,32);
  E=Env->Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitNotify,S,&gEfiEventExitBootServicesGuid,&S->Exit);if(!Live(S)||E!=EFI_SUCCESS||!S->Exit){Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);goto Done;}
  E=Env->Services->HandleProtocol(S->FileSystem,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&S->Sfs);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_SUCCESS||!S->Sfs||!S->Sfs->OpenVolume){E=E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);goto Failure;}
  E=S->Sfs->OpenVolume(S->Sfs,&S->Root);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_SUCCESS||!S->Root||!S->Root->Open||!S->Root->Close){Retain(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);goto Done;}S->CloseRoot=S->Root->Close;
  E=S->Root->Open(S->Root,&S->File,S->Path,EFI_FILE_MODE_READ,0);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_SUCCESS||!S->File||!S->File->Close||!S->File->Read||!S->File->SetPosition||!S->File->GetPosition||!S->File->GetInfo){Retain(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);goto Done;}
  S->CloseFile=S->File->Close;S->ReadFile=S->File->Read;S->SeekFile=S->File->SetPosition;S->PositionFile=S->File->GetPosition;S->InfoFile=S->File->GetInfo;
  UINTN Need=0;E=S->InfoFile(S->File,&gEfiFileInfoGuid,&Need,NULL);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_BUFFER_TOO_SMALL||Need<SIZE_OF_EFI_FILE_INFO+2||Need>sizeof(S->Info)){E=E==EFI_BUFFER_TOO_SMALL?EFI_BAD_BUFFER_SIZE:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);goto Failure;}
  UINT64 Bytes=0;E=Information(S,TRUE,&Bytes);if(E!=EFI_SUCCESS)goto Failure;if(S->InfoBytes!=Need){E=EFI_MEDIA_CHANGED;goto Failure;}if(Bytes>Spec->MaxBytes||Bytes>MAX_UINTN){E=EFI_BAD_BUFFER_SIZE;goto Failure;}S->Bytes=(UINTN)Bytes;
  E=Env->Services->AllocatePool(EfiLoaderData,S->Bytes,(VOID **)&S->Data);if(!Live(S)||E!=EFI_SUCCESS||!S->Data){Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);goto Done;}
  E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->Data,S->Bytes);
  if(E!=EFI_SUCCESS){Retain(S,E);goto Done;}
  if(!Sha256GetContextSize()||Sha256GetContextSize()>sizeof(S->ShaContext)||!Sha256Init(S->ShaContext)){E=EFI_DEVICE_ERROR;goto Failure;}
  for(UINT64 Offset=0;Offset<Bytes;){
    E=PianoCpuInputSlice(Cpu(S));if(E!=EFI_SUCCESS)goto Failure;
    UINTN N=(UINTN)MIN((UINT64)PIANO_CPU_INPUT_CHUNK,Bytes-Offset);E=DiskRead(S,Offset,N,S->Data+(UINTN)Offset);if(E!=EFI_SUCCESS)goto Failure;
    if(!Sha256Update(S->ShaContext,S->Data+(UINTN)Offset,N)){E=EFI_DEVICE_ERROR;goto Failure;}Offset+=N;++S->Chunks;
  }
  E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->Data,S->Bytes);if(E!=EFI_SUCCESS){Retain(S,E);goto Done;}
  if(!Sha256Final(S->ShaContext,S->Sha256)){E=EFI_DEVICE_ERROR;goto Failure;}
  E=Fresh(S);if(E!=EFI_SUCCESS)goto Failure;E=Information(S,FALSE,&Bytes);if(E!=EFI_SUCCESS)goto Failure;
  UINT64 Position=0;E=S->PositionFile(S->File,&Position);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_SUCCESS||Position!=S->Bytes){E=E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);goto Failure;}
  UINT8 Extra=0;UINTN N=1;E=S->ReadFile(S->File,&N,&Extra);if(!Live(S)){Retain(S,EFI_ABORTED);goto Done;}if(E!=EFI_SUCCESS||N!=0){E=E==EFI_SUCCESS?EFI_MEDIA_CHANGED:Exact(E);goto Failure;}
  if(S->HasExpected&&CompareMem(S->ExpectedSha256,S->Sha256,32)){E=EFI_SECURITY_VIOLATION;goto Failure;}
  E=CloseFiles(S);if(E!=EFI_SUCCESS)goto Done;
  E=PianoCpuInputValidateBuffer(Cpu(S),&S->Memory,S,NULL,S->Data,S->Bytes);if(E!=EFI_SUCCESS){Retain(S,E);goto Done;}
  S->Ready=TRUE;S->Status=EFI_SUCCESS;S->Busy=FALSE;E=PianoBootFileExport(S,Source,Blob);goto Done;
Failure:
  S->Status=Exact(E);if(!S->Retained){EFI_STATUS Close=CloseFiles(S);if(Close==EFI_SUCCESS){S->Cleanup=Release(S);}else S->Cleanup=Close;}
  if(S->Cleanup!=EFI_SUCCESS)E=S->Cleanup;
Done:
  S->Busy=FALSE;ZeroMem(S->ShaContext,sizeof(S->ShaContext));return S->Retained?S->Status:E;
}
EFI_STATUS PianoBootFileLoadBundle(PIANO_BOOT_FILE_SOURCE *States,UINTN Count,CONST PIANO_BOOT_FILE_ENV *E,CONST PIANO_BOOT_FILE_SPEC *Specs,UINT64 Budget,PIANO_BOOT_SOURCE *Readers,PIANO_LAUNCH_BLOB *Blobs){
  if(!States||!E||!Specs||!Readers||!Blobs||!Count||Count>3||!Budget||Budget>PIANO_CPU_INPUT_MAX_BYTES)return EFI_INVALID_PARAMETER;
  if(Overlap(Readers,Count*sizeof(*Readers),Blobs,Count*sizeof(*Blobs)))return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<Count;++I)if(!Output(&States[I],Readers,Count*sizeof(*Readers))||!Output(&States[I],Blobs,Count*sizeof(*Blobs)))return EFI_INVALID_PARAMETER;
  ZeroMem(Readers,Count*sizeof(*Readers));ZeroMem(Blobs,Count*sizeof(*Blobs));
  UINT64 Sum=0;for(UINTN I=0;I<Count;++I){if(!Specs[I].MaxBytes||Specs[I].MaxBytes>Budget-Sum)return EFI_BAD_BUFFER_SIZE;Sum+=Specs[I].MaxBytes;}
  PIANO_LINUX_MEMORY_PROOF Memory;EFI_STATUS Capacity=PianoCpuInputAuthorize(E->Cpu,Budget,&Memory);
  if(Capacity!=EFI_SUCCESS)return Capacity;
  for(UINTN I=0;I<Count;++I){EFI_STATUS S=PianoBootFileLoad(&States[I],E,&Specs[I],&Readers[I],&Blobs[I]);if(S!=EFI_SUCCESS){for(UINTN J=0;J<I;++J){EFI_STATUS F=PianoBootFileDispose(&States[J]);if(F!=EFI_SUCCESS)S=F;}ZeroMem(Readers,Count*sizeof(*Readers));ZeroMem(Blobs,Count*sizeof(*Blobs));return S;}}return EFI_SUCCESS;
}
