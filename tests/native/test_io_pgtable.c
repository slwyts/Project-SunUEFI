// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoIoPageTable.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
int main(void){
  void *memory=NULL;assert(posix_memalign(&memory,4096,PIANO_IO_PT_BYTES)==0);
  PIANO_IO_PAGE_TABLE t;assert(PianoIoPageTableInit(&t,memory,0xd7000000,PIANO_IO_PT_BYTES)==EFI_SUCCESS);
  assert(t.Tables[1]==0xd7001003 && t.Tables[512]==0xd7002003 && t.Tables[519]==0xd7009003);
  UINT64 pa;assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE,FALSE,&pa)==EFI_NO_MAPPING);
  assert(PianoIoPageTableMap(&t,PIANO_IOVA_BASE,0xc0000000,8192,PianoDmaToDevice)==EFI_SUCCESS);
  assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE+4123,FALSE,&pa)==EFI_SUCCESS && pa==0xc000101b);
  assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE,TRUE,&pa)==EFI_ACCESS_DENIED);
  assert(PianoIoPageTableMap(&t,PIANO_IOVA_BASE+4096,0xc1000000,8192,PianoDmaBidirectional)==EFI_ALREADY_STARTED);
  assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE+8192,FALSE,&pa)==EFI_NO_MAPPING);
  assert(PianoIoPageTableUnmap(&t,PIANO_IOVA_BASE,8192)==EFI_SUCCESS);
  assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE,FALSE,&pa)==EFI_NO_MAPPING);
  assert(PianoIoPageTableMap(&t,PIANO_IOVA_BASE+0x200000,0xc1000000,4096,PianoDmaFromDevice)==EFI_SUCCESS);
  assert(PianoIoPageTableTranslate(&t,PIANO_IOVA_BASE+0x200fff,TRUE,&pa)==EFI_SUCCESS && pa==0xc1000fff);
  assert(PianoIoPageTableMap(&t,PIANO_IOVA_BASE+PIANO_IOVA_BYTES-4096,0xc2000000,8192,PianoDmaBidirectional)==EFI_INVALID_PARAMETER);
  assert(PianoIoPageTableMap(&t,PIANO_IOVA_BASE,0x1000000000000ULL,4096,PianoDmaBidirectional)==EFI_BAD_BUFFER_SIZE);
  free(memory);puts("Owned LPAE tables: three levels, nonidentity mappings, permissions, cross-table offsets, unmap and bounds passed.");return 0;
}
