// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only emitter executing the production formatter/journal code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../uefi/core/PianoNvFvb.h"
#include <Library/BaseMemoryLib.h>
static UINT8 slots[2][768][4096];
VOID *EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID *EFIAPI SetMem(VOID*p,UINTN n,UINT8 v){return memset(p,v,n);}VOID *EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
static EFI_STATUS Read(VOID*c,UINT32 s,UINT32 b,UINTN n,VOID*p){if(c!=slots||s>1||b>767||n!=4096)return EFI_INVALID_PARAMETER;memcpy(p,slots[s][b],4096);return EFI_SUCCESS;}
static EFI_STATUS Write(VOID*c,UINT32 s,UINT32 b,UINTN n,CONST VOID*p){if(c!=slots||s>1||b>767||n!=4096)return EFI_INVALID_PARAMETER;memcpy(slots[s][b],p,4096);return EFI_SUCCESS;}
static EFI_STATUS Flush(VOID*c){return c==slots?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
static int emit(CONST char*path,CONST VOID*p,size_t n){FILE*f=fopen(path,"wb");if(!f)return 1;int bad=fwrite(p,1,n,f)!=n;if(fclose(f))bad=1;return bad;}
int main(int argc,char**argv){if(argc!=5||strlen(argv[1])!=32)return 2;PIANO_UFS_PRODUCT_NV_IO io={.Context=slots,.LayoutId=1,.Read=Read,.Write=Write,.Flush=Flush};for(int i=0;i<16;i++){unsigned v;if(sscanf(argv[1]+i*2,"%2x",&v)!=1)return 2;io.VolumeUuid[i]=(UINT8)v;}
 UINT8*m=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES),*s=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES);PIANO_NV_JOURNAL*j=calloc(1,sizeof(*j));if(!m||!s||!j)return 3;
 if(PianoNvJournalBind(j,&io,m,s)!=EFI_SUCCESS||PianoNvJournalRecover(j)!=EFI_NOT_FOUND||PianoNvFvbBuildBlank(s,PIANO_NV_SNAPSHOT_BYTES)!=EFI_SUCCESS||PianoNvJournalCommit(j,s)!=EFI_SUCCESS)return 4;
 int rc=emit(argv[2],m,PIANO_NV_SNAPSHOT_BYTES)||emit(argv[3],slots[0],sizeof(slots[0]))||emit(argv[4],slots[1],sizeof(slots[1]));free(j);free(m);free(s);return rc;
}
