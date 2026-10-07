// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#define PIANO_DMA_HOST_TEST 1
#include "../../uefi/core/PianoDma.c"
EFI_BOOT_SERVICES *gBS;EFI_DXE_SERVICES *gDS;
static EFI_BOOT_SERVICES bs;static EFI_DXE_SERVICES ds;
static void *allocation;static size_t allocation_bytes;static unsigned frees,clean,invalid,both,unmaps,faults;
static UINT64 attributes=EFI_MEMORY_WB;static EFI_PHYSICAL_ADDRESS fake_iova=0x40000000;
static int map_fail,unmap_fail;
static EFI_MEMORY_TYPE expected_type=EfiBootServicesData;
static BOOLEAN debug_enabled;
static unsigned debug_calls,format_calls;
static char formatted_phase[64];
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return debug_enabled;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return debug_enabled;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){++debug_calls;}
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Size,CONST CHAR8 *Format,...){
  assert(debug_enabled);++format_calls;va_list args;va_start(args,Format);
  if(strncmp(Format,"phase=sync-cpu-quiet-summary",27)==0)strcpy(formatted_phase,"quiet-summary");
  else {assert(strncmp(Format,"phase=%a seq=",13)==0);strcpy(formatted_phase,va_arg(args,const char *));}
  va_end(args);strcpy(Buffer,"host formatted record fixture");return strlen(Buffer);
}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
VOID EFIAPI MemoryFence(VOID){ }
static EFI_MEMORY_REGION_DESCRIPTOR region;
VOID GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **Map,UINT8 *Count){*Map=&region;*Count=1;}
EFI_STATUS PianoDmaPhysicalAddress(CONST VOID *P,EFI_PHYSICAL_ADDRESS *Pa){*Pa=(UINTN)P;return EFI_SUCCESS;}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){++clean;assert(!(N&63));return P;}
VOID *EFIAPI InvalidateDataCacheRange(VOID *P,UINTN N){++invalid;assert(!(N&63));return P;}
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *P,UINTN N){++both;assert(!(N&63));return P;}
static EFI_STATUS EFIAPI alloc(EFI_ALLOCATE_TYPE T,EFI_MEMORY_TYPE M,UINTN Pages,EFI_PHYSICAL_ADDRESS *A){
  assert(T==AllocateMaxAddress && M==expected_type && !allocation);
  allocation_bytes=EFI_PAGES_TO_SIZE(Pages);assert(posix_memalign(&allocation,4096,allocation_bytes)==0);
  region.Address=(UINTN)allocation;region.Length=allocation_bytes+0x100000;
  *A=(UINTN)allocation;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pages(EFI_PHYSICAL_ADDRESS A,UINTN Pages){
  assert(A==(UINTN)allocation && EFI_PAGES_TO_SIZE(Pages)==allocation_bytes);
  free(allocation);allocation=NULL;++frees;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Out){
  Out->BaseAddress=region.Address;Out->Length=region.Length;
  Out->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;Out->Attributes=attributes;return EFI_SUCCESS;
}
static EFI_STATUS map(PIANO_DMA_DEVICE *D,EFI_PHYSICAL_ADDRESS P,UINTN N,PIANO_DMA_DIRECTION Dir,UINTN Align,EFI_PHYSICAL_ADDRESS *I,VOID **Token){
  if(map_fail)return EFI_DEVICE_ERROR;
  *I=fake_iova;*Token=(void *)123;return EFI_SUCCESS;
}
static EFI_STATUS unmap(PIANO_DMA_DEVICE *D,VOID *Token){assert(Token==(void *)123);++unmaps;return unmap_fail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static VOID fault(PIANO_DMA_DEVICE *D){++faults;}
int main(void){
  assert(RecordCrc("123456789",9)==0xcbf43926);
  assert(RecordCrc("",0)==0);
  strcpy(region.Name,"DXE_Heap");region.Address=0;region.Length=MAX_UINT64;
  gBS=&bs;gDS=&ds;bs.AllocatePages=alloc;bs.FreePages=free_pages;ds.GetMemorySpaceDescriptor=descriptor;
  PIANO_DMA_DEVICE d={.Name="ufs",.StreamId=0x60,.AddressBits=32,.CacheLine=64,.Fault=fault};PIANO_DMA_BUFFER b;
  assert(PianoDmaAllocate(&d,1000,128,64,PianoDmaFromDevice,&b)==EFI_SUCCESS);
  assert(b.MemoryType==EfiBootServicesData && !b.ExitRetained);
  assert(PianoDmaRetainForExit(&b)==EFI_ACCESS_DENIED && !b.ExitRetained);
  assert(b.Bytes==1000 && b.ReservedBytes==4096 && !(b.Physical&4095));
  assert(PianoDmaMap(&b)==EFI_NOT_READY && !b.Mapped);
  EFI_PHYSICAL_ADDRESS translated=0;
  assert(PianoDmaPhysicalToDevice(&b,b.Physical,1,&translated)==EFI_NOT_READY);
  d.Map=map;d.Unmap=unmap;assert(PianoDmaMap(&b)==EFI_SUCCESS && b.DeviceAddress==0x40000000);
  assert(PianoDmaPhysicalToDevice(&b,b.Physical+127,873,&translated)==EFI_SUCCESS && translated==0x4000007f);
  assert(PianoDmaDeviceToPhysical(&b,translated,873,&translated)==EFI_SUCCESS && translated==b.Physical+127);
  assert(PianoDmaPhysicalToDevice(&b,b.Physical+999,2,&translated)==EFI_BAD_BUFFER_SIZE);
  assert(PianoDmaDeviceToPhysical(&b,b.DeviceAddress+4095,1,&translated)==EFI_BAD_BUFFER_SIZE);
  assert(PianoDmaDeviceToPhysical(&b,b.DeviceAddress-1,1,&translated)==EFI_BAD_BUFFER_SIZE);
  assert(PianoDmaDeviceToPhysical(&b,b.DeviceAddress,MAX_UINTN,&translated)==EFI_BAD_BUFFER_SIZE);
  assert(PianoDmaBegin(&b,"NOP-RAM-mock")==EFI_SUCCESS && both==1);
  assert(PianoDmaSyncForCpu(&b)==EFI_SUCCESS && invalid==1 && b.Active);
  assert(PianoDmaFree(&b)==EFI_ACCESS_DENIED && !frees);
  assert(PianoDmaComplete(&b,EFI_SUCCESS,TRUE)==EFI_SUCCESS && invalid==2);
  unmap_fail=1;assert(PianoDmaUnmap(&b)==EFI_DEVICE_ERROR && b.Mapped);unmap_fail=0;
  assert(PianoDmaFree(&b)==EFI_SUCCESS && frees==1);
  region.Address=0;region.Length=MAX_UINT64;
  assert(PianoDmaAllocate(&d,512,128,64,PianoDmaToDevice,&b)==EFI_SUCCESS);
  fake_iova=0xfffff000;assert(PianoDmaMap(&b)==EFI_SUCCESS);
  assert(PianoDmaBegin(&b,"to-device")==EFI_SUCCESS && clean>=2);
  assert(PianoDmaSyncForCpu(&b)==EFI_ACCESS_DENIED);
  assert(PianoDmaComplete(&b,EFI_TIMEOUT,FALSE)==EFI_DEVICE_ERROR && faults==1 && b.Quarantined);
  assert(PianoDmaFree(&b)==EFI_ACCESS_DENIED);
  // The mock hardware is explicitly stopped before releasing quarantined RAM.
  assert(PianoDmaComplete(&b,EFI_TIMEOUT,TRUE)==EFI_TIMEOUT && faults==2 && !b.Quarantined);
  assert(PianoDmaFree(&b)==EFI_SUCCESS);
  region.Address=0;region.Length=MAX_UINT64;fake_iova=0x100000000ULL;
  assert(PianoDmaAllocate(&d,4096,128,64,PianoDmaBidirectional,&b)==EFI_SUCCESS);
  assert(PianoDmaMap(&b)==EFI_BAD_BUFFER_SIZE && !b.Mapped);
  d.AddressBits=64;assert(PianoDmaMap(&b)==EFI_SUCCESS);
  assert(PianoDmaFree(&b)==EFI_SUCCESS);
  region.Address=0;region.Length=MAX_UINT64;attributes=EFI_MEMORY_UC;
  assert(PianoDmaAllocate(&d,4096,128,64,PianoDmaBidirectional,&b)==EFI_UNSUPPORTED && !allocation);
  assert(PianoDmaAllocate(&d,4096,3,64,PianoDmaBidirectional,&b)==EFI_INVALID_PARAMETER);
  attributes=EFI_MEMORY_WB;region.Address=0;region.Length=MAX_UINT64;
  d.ReserveAcrossExit=TRUE;expected_type=EfiReservedMemoryType;
  assert(PianoDmaAllocate(&d,4096,4096,64,PianoDmaFromDevice,&b)==EFI_SUCCESS && b.MemoryType==EfiReservedMemoryType);
  assert(PianoDmaFree(&b)==EFI_SUCCESS); // Reserved remains reclaimable before EBS.
  region.Address=0;region.Length=MAX_UINT64;
  assert(PianoDmaAllocate(&d,4096,4096,64,PianoDmaFromDevice,&b)==EFI_SUCCESS);
  b.Active=TRUE;assert(PianoDmaRetainForExit(&b)==EFI_ACCESS_DENIED);b.Active=FALSE;
  b.Quarantined=TRUE;assert(PianoDmaRetainForExit(&b)==EFI_ACCESS_DENIED);b.Quarantined=FALSE;
  assert(PianoDmaRetainForExit(&b)==EFI_SUCCESS && b.ExitRetained);
  unsigned old_frees=frees;
  assert(PianoDmaFree(&b)==EFI_ACCESS_DENIED && PianoDmaUnmap(&b)==EFI_ACCESS_DENIED && frees==old_frees);
  assert(PianoDmaMap(&b)==EFI_INVALID_PARAMETER);
  b.ExitRetained=FALSE;assert(PianoDmaFree(&b)==EFI_SUCCESS); // Host reset simulation only.
  region.Address=0;region.Length=MAX_UINT64;fake_iova=0x40000000;
  assert(PianoDmaAllocate(&d,4096,4096,64,PianoDmaFromDevice,&b)==EFI_SUCCESS && PianoDmaMap(&b)==EFI_SUCCESS);
  assert(PianoDmaBegin(&b,"active-poll") == EFI_SUCCESS);
  unsigned old_invalid=invalid;debug_enabled=TRUE;debug_calls=format_calls=0;
  for(unsigned i=0;i<5;++i)assert(PianoDmaSyncForCpuQuiet(&b)==EFI_SUCCESS);
  assert(invalid==old_invalid+5 && b.Active && b.QuietSyncs==5 && !debug_calls && !format_calls);
  assert(PianoDmaReportQuietSync(&b)==EFI_SUCCESS && debug_calls==2 && format_calls==1 && !strcmp(formatted_phase,"quiet-summary") && b.QuietSyncReported==5);
  assert(PianoDmaReportQuietSync(&b)==EFI_SUCCESS && debug_calls==2 && format_calls==1);
  assert(PianoDmaSyncForCpu(&b)==EFI_SUCCESS && debug_calls==4 && format_calls==2 && !strcmp(formatted_phase,"sync-cpu") && b.QuietSyncs==5);
  b.Direction=PianoDmaToDevice;unsigned before_reject=invalid;
  assert(PianoDmaSyncForCpuQuiet(&b)==EFI_ACCESS_DENIED && invalid==before_reject && b.QuietSyncs==5);
  b.Direction=PianoDmaFromDevice;b.Quarantined=TRUE;
  assert(PianoDmaSyncForCpuQuiet(&b)==EFI_NOT_READY && invalid==before_reject && b.QuietSyncs==5);b.Quarantined=FALSE;
  assert(PianoDmaSyncForCpuQuiet(&b)==EFI_SUCCESS && b.QuietSyncs==6 && invalid==before_reject+1);
  debug_enabled=FALSE;
  assert(PianoDmaComplete(&b,EFI_SUCCESS,TRUE)==EFI_SUCCESS && PianoDmaFree(&b)==EFI_SUCCESS);
  puts("Unified DMA: allocation bounds, absent-backend rejection, nonidentity IOVA, cache direction, address width, active/unquiesced retention and failed-unmap handling passed.");
  return 0;
}
