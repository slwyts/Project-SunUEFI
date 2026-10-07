// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real unified operations and gates. Boundary callbacks are host fixtures.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/components/os-boot/PianoCpuInput.h"
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}
BOOLEAN EFIAPI Sha256Init(VOID *P){return SHA256_Init(P)==1;}
BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}
BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *H){return SHA256_Final(H,C)==1;}
static UINTN scenario,slices,checks,validators,spans,cases;
static BOOLEAN live=TRUE;
static UINT8 source[65537],destination[65537];
static BOOLEAN Alive(VOID *C){assert(C==source);return live;}
static EFI_STATUS Slice(VOID *C,UINTN B){assert(C==source&&B==1000);++slices;
 if(scenario==8)return EFI_WARN_STALE_DATA;if(scenario==9){live=FALSE;return EFI_SUCCESS;}
 return EFI_SUCCESS;}
static EFI_STATUS Memory(VOID *C,PIANO_LINUX_MEMORY_PROOF *P){assert(C==source);++checks;
 *P=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=1,.DramBytes=16ULL*1024*1024*1024,
 .NormalBytes=4ULL*1024*1024*1024,.FullDdr=TRUE,.FixedReservations=TRUE,.DynamicReservations=TRUE,
 .RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};
 if(scenario==1)P->FullDdr=FALSE;if(scenario==2)P->UnresolvedReservations=1;
 if(scenario==3)P->NormalBytes=64*1024*1024;if(scenario==4)P->OwnershipVerified=2;
 return scenario==5?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS Validate(VOID *C,CONST PIANO_LINUX_MEMORY_PROOF *P){assert(C==source);++validators;
 if(scenario==6)return EFI_NOT_READY;if(scenario==7)((PIANO_LINUX_MEMORY_PROOF *)P)->BootEpoch=2;
 return EFI_SUCCESS;}
static EFI_STATUS Buffer(VOID *C,CONST PIANO_LINUX_MEMORY_PROOF *P,CONST VOID *Producer,VOID *Owner,CONST VOID *Base,UINT64 Bytes){
 assert(C==source&&P->BootEpoch==1&&Producer==source&&Owner==source&&Base==source&&Bytes==sizeof(source));++spans;
 return scenario==10?EFI_ACCESS_DENIED:EFI_SUCCESS;}
int main(VOID){
 PIANO_CPU_INPUT_ENV env={source,Alive,Slice,Memory,Validate,Buffer};PIANO_LINUX_MEMORY_PROOF proof;
 assert(PianoCpuInputAuthorize(NULL,PIANO_CPU_INPUT_LOW_BYTES,&proof)==EFI_SUCCESS&&proof.Status==EFI_NOT_READY);++cases;
 assert(PianoCpuInputAuthorize(NULL,PIANO_CPU_INPUT_LOW_BYTES+1,&proof)==EFI_NOT_READY);++cases;
 assert(PianoCpuInputAuthorize(&env,PIANO_CPU_INPUT_MAX_BYTES+1,&proof)==EFI_BAD_BUFFER_SIZE&&!checks);++cases;
 assert(PianoCpuInputAuthorize(&env,PIANO_CPU_INPUT_MAX_BYTES,&proof)==EFI_SUCCESS&&checks==1&&validators==1);++cases;
 for(scenario=1;scenario<=7;++scenario){EFI_STATUS s=PianoCpuInputAuthorize(&env,PIANO_CPU_INPUT_MAX_BYTES,&proof);
   assert(s==(scenario==5?EFI_DEVICE_ERROR:scenario==7?EFI_COMPROMISED_DATA:EFI_NOT_READY));++cases;}
 scenario=0;assert(PianoCpuInputAuthorize(&env,PIANO_CPU_INPUT_MAX_BYTES,&proof)==EFI_SUCCESS);
 assert(PianoCpuInputValidateBuffer(&env,&proof,source,source,source,sizeof(source))==EFI_SUCCESS&&spans==1);++cases;
 scenario=10;assert(PianoCpuInputValidateBuffer(&env,&proof,source,source,source,sizeof(source))==EFI_ACCESS_DENIED);++cases;
 scenario=0;for(UINTN i=0;i<sizeof(source);++i)source[i]=(UINT8)(i*31);
 slices=0;assert(PianoCpuInputCopy(&env,destination,source,sizeof(source))==EFI_SUCCESS&&slices==3&&!memcmp(destination,source,sizeof(source)));++cases;
 assert(PianoCpuInputCopy(&env,source+1,source,65536)==EFI_INVALID_PARAMETER);++cases;
 assert(PianoCpuInputCopy(&env,(VOID *)(MAX_UINTN-4),source,16)==EFI_INVALID_PARAMETER);++cases;
 UINT8 hash[32],expected[32];SHA256(source,sizeof(source),expected);slices=0;
 assert(PianoCpuInputHash(&env,source,sizeof(source),hash)==EFI_SUCCESS&&slices==3&&!memcmp(hash,expected,32));++cases;
 scenario=8;memset(hash,0xa5,sizeof(hash));assert(PianoCpuInputHash(&env,source,sizeof(source),hash)==EFI_DEVICE_ERROR);
 for(UINTN i=0;i<sizeof(hash);++i)assert(hash[i]==0xa5);++cases;
 memset(destination,0xa5,sizeof(destination));assert(PianoCpuInputZero(&env,destination,sizeof(destination))==EFI_DEVICE_ERROR);
 assert(destination[0]==0xa5&&destination[65536]==0xa5);++cases;
 scenario=9;assert(PianoCpuInputCopy(&env,destination,source,sizeof(source))==EFI_ABORTED&&destination[0]==0xa5);++cases;
 scenario=0;live=TRUE;slices=0;assert(PianoCpuInputZero(&env,destination,sizeof(destination))==EFI_SUCCESS&&slices==3);
 for(UINTN i=0;i<sizeof(destination);++i)assert(destination[i]==0);++cases;
 printf("Actual unified CPU input: %lu gate/1GiB cap/full-memory/owner/mutation/boundedSHA-copy-zero/slice-warning/EBS cases passed; host callback boundary only\n",(unsigned long)cases);return 0;
}
