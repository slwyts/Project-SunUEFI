// SPDX-License-Identifier: BSD-2-Clause-Patent
// Owned ARM LPAE S1 tables: 4 KiB granule, 39-bit input, 48-bit output,
// fixed 16 MiB IOVA arena. No inherited table is edited or guessed.
#include "PianoIoPageTable.h"
#include <Library/BaseMemoryLib.h>
#define PA_MASK 0x0000FFFFFFFFF000ULL
#define TABLE_DESC 3ULL
#define PAGE_BASE (BIT54|BIT53|BIT10|BIT9|BIT5|3ULL)
STATIC EFI_STATUS Range(UINT64 Iova,UINTN Bytes) {
  if((Iova&4095) || (Bytes&4095) || !Bytes || Iova<PIANO_IOVA_BASE ||
     Iova-PIANO_IOVA_BASE>=PIANO_IOVA_BYTES || Bytes>PIANO_IOVA_BYTES-(Iova-PIANO_IOVA_BASE))
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
STATIC UINT64 *Page(PIANO_IO_PAGE_TABLE *T,UINT64 Iova) {
  UINTN Index=(UINTN)((Iova-PIANO_IOVA_BASE)>>12);
  return T->Tables+1024+Index;
}
EFI_STATUS PianoIoPageTableInit(PIANO_IO_PAGE_TABLE *T,VOID *Memory,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes) {
  if(T==NULL || Memory==NULL || ((UINTN)Memory&4095) || (Physical&4095) ||
     Bytes<PIANO_IO_PT_BYTES || Physical>PA_MASK || PIANO_IO_PT_BYTES-1>0x0000FFFFFFFFFFFFULL-Physical)
    return EFI_INVALID_PARAMETER;
  T->Tables=Memory;T->Physical=Physical;ZeroMem(Memory,PIANO_IO_PT_BYTES);
  T->Tables[(PIANO_IOVA_BASE>>30)&511]=(Physical+4096)|TABLE_DESC;
  for(UINTN I=0;I<8;++I)T->Tables[512+I]=(Physical+(2+I)*4096)|TABLE_DESC;
  return EFI_SUCCESS;
}
EFI_STATUS PianoIoPageTableMap(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,UINT64 Physical,UINTN Bytes,PIANO_DMA_DIRECTION Dir) {
  if(T==NULL || T->Tables==NULL || Dir>PianoDmaBidirectional)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Range(Iova,Bytes);if(EFI_ERROR(Status))return Status;
  if((Physical&4095) || Physical>PA_MASK || Bytes-1>0x0000FFFFFFFFFFFFULL-Physical)return EFI_BAD_BUFFER_SIZE;
  for(UINTN I=0;I<Bytes/4096;++I)if(*Page(T,Iova+I*4096)!=0)return EFI_ALREADY_STARTED;
  UINT64 Permissions=Dir==PianoDmaToDevice?(BIT7|BIT6):BIT6;
  for(UINTN I=0;I<Bytes/4096;++I)*Page(T,Iova+I*4096)=(Physical+I*4096)|PAGE_BASE|Permissions;
  return EFI_SUCCESS;
}
EFI_STATUS PianoIoPageTableUnmap(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,UINTN Bytes) {
  if(T==NULL || T->Tables==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Range(Iova,Bytes);if(EFI_ERROR(Status))return Status;
  for(UINTN I=0;I<Bytes/4096;++I)if((*Page(T,Iova+I*4096)&3)!=3)return EFI_NOT_FOUND;
  for(UINTN I=0;I<Bytes/4096;++I)*Page(T,Iova+I*4096)=0;
  return EFI_SUCCESS;
}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,BOOLEAN Write,UINT64 *Physical) {
  if(T==NULL || T->Tables==NULL || Physical==NULL ||
     Iova<PIANO_IOVA_BASE || Iova-PIANO_IOVA_BASE>=PIANO_IOVA_BYTES)return EFI_INVALID_PARAMETER;
  UINT64 Entry=*Page(T,Iova&~4095ULL);
  if((Entry&3)!=3 || !(Entry&BIT10))return EFI_NO_MAPPING;
  if(Write && (Entry&BIT7))return EFI_ACCESS_DENIED;
  *Physical=(Entry&PA_MASK)|(Iova&4095);return EFI_SUCCESS;
}
