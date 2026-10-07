// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoCpuInput.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Ready(CONST PIANO_LINUX_MEMORY_PROOF *P,UINT64 Bytes){
 return P->Revision==1&&P->Status==EFI_SUCCESS&&P->BootEpoch&&P->DramBytes&&P->NormalBytes>=Bytes&&
   P->NormalBytes<=P->DramBytes&&!P->UnresolvedReservations&&P->FullDdr==TRUE&&
   P->FixedReservations==TRUE&&P->DynamicReservations==TRUE&&P->RuntimeRegions==TRUE&&
   P->CacheVerified==TRUE&&P->OwnershipVerified==TRUE;
}
EFI_STATUS PianoCpuInputSlice(CONST PIANO_CPU_INPUT_ENV *E){
 if(!E)return EFI_SUCCESS;
 if(!E->BootServicesAlive||!E->ServiceSlice)return EFI_NOT_READY;
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 EFI_STATUS S=Exact(E->ServiceSlice(E->Context,1000));
 return E->BootServicesAlive(E->Context)==TRUE?S:EFI_ABORTED;
}
EFI_STATUS PianoCpuInputAuthorize(CONST PIANO_CPU_INPUT_ENV *E,UINT64 Bytes,PIANO_LINUX_MEMORY_PROOF *P){
 if(!P||!Bytes||Bytes>PIANO_CPU_INPUT_MAX_BYTES||Bytes>MAX_UINTN)return EFI_BAD_BUFFER_SIZE;
 ZeroMem(P,sizeof(*P));P->Status=EFI_NOT_READY;
 if(Bytes<=PIANO_CPU_INPUT_LOW_BYTES&&!E)return EFI_SUCCESS;
 if(!E||!E->BootServicesAlive||!E->ServiceSlice)return EFI_NOT_READY;
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 if(!E->CheckMemory||!E->ValidateMemory||!E->ValidateBuffer)
   return Bytes<=PIANO_CPU_INPUT_LOW_BYTES&&!E->CheckMemory&&!E->ValidateMemory&&!E->ValidateBuffer?EFI_SUCCESS:EFI_NOT_READY;
 EFI_STATUS S=Exact(E->CheckMemory(E->Context,P));
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 if(S!=EFI_SUCCESS)return S;
 if(!Ready(P,Bytes))return EFI_NOT_READY;
 PIANO_LINUX_MEMORY_PROOF Saved=*P;
 S=Exact(E->ValidateMemory(E->Context,P));
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 if(CompareMem(P,&Saved,sizeof(*P)))return EFI_COMPROMISED_DATA;
 return S;
}
EFI_STATUS PianoCpuInputValidateBuffer(CONST PIANO_CPU_INPUT_ENV *E,CONST PIANO_LINUX_MEMORY_PROOF *P,
 CONST VOID *Producer,VOID *Owner,CONST VOID *Base,UINT64 Bytes){
 if(!Producer||!Base||!Bytes||Bytes>PIANO_CPU_INPUT_MAX_BYTES||Bytes>MAX_UINTN||
    (UINTN)Base>MAX_UINTN-(UINTN)Bytes)return EFI_INVALID_PARAMETER;
 if(Bytes<=PIANO_CPU_INPUT_LOW_BYTES&&(!E||!E->ValidateBuffer))return EFI_SUCCESS;
 if(!E||!P||!Ready(P,Bytes)||!E->ValidateMemory||!E->ValidateBuffer||
    !E->BootServicesAlive||!E->ServiceSlice)return EFI_NOT_READY;
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 PIANO_LINUX_MEMORY_PROOF Saved=*P;EFI_STATUS S=Exact(E->ValidateMemory(E->Context,P));
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 if(S!=EFI_SUCCESS)return S;
 if(CompareMem(P,&Saved,sizeof(*P)))return EFI_COMPROMISED_DATA;
 S=Exact(E->ValidateBuffer(E->Context,P,Producer,Owner,Base,Bytes));
 if(E->BootServicesAlive(E->Context)!=TRUE)return EFI_ABORTED;
 return CompareMem(P,&Saved,sizeof(*P))?EFI_COMPROMISED_DATA:S;
}
STATIC EFI_STATUS Bounds(CONST PIANO_CPU_INPUT_ENV *E,CONST VOID *P,UINTN N){
 if(!P||N>PIANO_CPU_INPUT_MAX_BYTES||(UINTN)P>MAX_UINTN-N)return EFI_INVALID_PARAMETER;
 if(N>PIANO_CPU_INPUT_LOW_BYTES&&!E)return EFI_NOT_READY;
 return EFI_SUCCESS;
}
EFI_STATUS PianoCpuInputCopy(CONST PIANO_CPU_INPUT_ENV *E,VOID *To,CONST VOID *From,UINTN Bytes){
 EFI_STATUS S=Bounds(E,To,Bytes);if(S!=EFI_SUCCESS)return S;S=Bounds(E,From,Bytes);if(S!=EFI_SUCCESS)return S;
 if(Bytes&&(UINTN)To<(UINTN)From+Bytes&&(UINTN)From<(UINTN)To+Bytes)return EFI_INVALID_PARAMETER;
 for(UINTN At=0;At<Bytes;){S=PianoCpuInputSlice(E);if(S!=EFI_SUCCESS)return S;
   UINTN N=MIN((UINTN)PIANO_CPU_INPUT_CHUNK,Bytes-At);CopyMem((UINT8 *)To+At,(CONST UINT8 *)From+At,N);At+=N;
 }return PianoCpuInputSlice(E);
}
EFI_STATUS PianoCpuInputZero(CONST PIANO_CPU_INPUT_ENV *E,VOID *Base,UINTN Bytes){
 EFI_STATUS S=Bounds(E,Base,Bytes);if(S!=EFI_SUCCESS)return S;
 for(UINTN At=0;At<Bytes;){S=PianoCpuInputSlice(E);if(S!=EFI_SUCCESS)return S;
   UINTN N=MIN((UINTN)PIANO_CPU_INPUT_CHUNK,Bytes-At);
   for(UINTN I=0;I<N;++I)((volatile UINT8 *)Base)[At+I]=0;At+=N;
 }return PianoCpuInputSlice(E);
}
EFI_STATUS PianoCpuInputHash(CONST PIANO_CPU_INPUT_ENV *E,CONST VOID *Base,UINTN Bytes,UINT8 Hash[32]){
 EFI_STATUS S=Bounds(E,Base,Bytes);if(S!=EFI_SUCCESS||!Hash)return S!=EFI_SUCCESS?S:EFI_INVALID_PARAMETER;
 UINT64 Context[512]={0};UINT8 Out[32];UINTN Size=Sha256GetContextSize();
 if(!Size||Size>sizeof(Context)||!Sha256Init(Context))return EFI_DEVICE_ERROR;
 for(UINTN At=0;At<Bytes;){S=PianoCpuInputSlice(E);if(S!=EFI_SUCCESS)goto Done;
   UINTN N=MIN((UINTN)PIANO_CPU_INPUT_CHUNK,Bytes-At);
   if(!Sha256Update(Context,(CONST UINT8 *)Base+At,N)){S=EFI_DEVICE_ERROR;goto Done;}At+=N;
 }
 S=PianoCpuInputSlice(E);if(S!=EFI_SUCCESS)goto Done;
 if(!Sha256Final(Context,Out)){S=EFI_DEVICE_ERROR;goto Done;}CopyMem(Hash,Out,32);
Done:
 ZeroMem(Context,sizeof(Context));ZeroMem(Out,sizeof(Out));return S;
}
