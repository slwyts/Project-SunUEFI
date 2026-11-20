// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small libc surface required by lwext4, using ordinary EDK2 allocations.
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <stdlib.h>
#include <string.h>
typedef struct {UINTN Size;} ALLOCATION;
void *malloc(size_t N){if(!N||N>MAX_UINTN-sizeof(ALLOCATION))return NULL;ALLOCATION *P=AllocatePool(N+sizeof(*P));if(!P)return NULL;P->Size=N;return P+1;}
void free(void *P){if(P)FreePool((ALLOCATION *)P-1);}
void *calloc(size_t N,size_t S){if(S&&N>MAX_UINTN/S)return NULL;void *P=malloc(N*S);if(P)ZeroMem(P,N*S);return P;}
void *realloc(void *P,size_t N){if(!P)return malloc(N);if(!N){free(P);return NULL;}void *Q=malloc(N);if(!Q)return NULL;CopyMem(Q,P,MIN(N,((ALLOCATION *)P-1)->Size));free(P);return Q;}
void *memcpy(void *D,const void *S,size_t N){return CopyMem(D,S,N);}
void *memmove(void *D,const void *S,size_t N){return CopyMem(D,S,N);}
void *memset(void *D,int V,size_t N){return SetMem(D,N,(UINT8)V);}
int memcmp(const void *A,const void *B,size_t N){return (int)CompareMem(A,B,N);}
size_t strlen(const char *S){return AsciiStrLen(S);}
int strcmp(const char *A,const char *B){return (int)AsciiStrCmp(A,B);}
int strncmp(const char *A,const char *B,size_t N){return (int)AsciiStrnCmp(A,B,N);}
char *strcpy(char *D,const char *S){char *R=D;do{*D++=*S;}while(*S++);return R;}
char *strncpy(char *D,const char *S,size_t N){char *R=D;while(N&&*S){*D++=*S++;--N;}while(N--)*D++=0;return R;}
void qsort(void *P,size_t N,size_t S,int (*Cmp)(const void *,const void *)){
  UINT8 *B=P;for(UINTN I=1;I<N;++I)for(UINTN J=I;J&&Cmp(B+(J-1)*S,B+J*S)>0;--J)
    for(UINTN K=0;K<S;++K){UINT8 X=B[(J-1)*S+K];B[(J-1)*S+K]=B[J*S+K];B[J*S+K]=X;}
}
