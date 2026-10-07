#define _GNU_SOURCE
#define PIANO_EFI_TRACE_HOST_TEST 1
#include "PianoEfiHandoffTraceTestShim.h"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>
#include "../../uefi/components/linux-loader/PianoEfiHandoffTrace.c"

static EFI_BOOT_SERVICES *services;
static char output[128 * 1024];
static size_t output_size;
static int mode, exit_calls, map_calls;
static EFI_STATUS exit_result;
static UINTN key=41;

UINTN SerialPortWrite(UINT8 *buffer,UINTN size) {
  assert(output_size+size<sizeof(output));memcpy(output+output_size,buffer,size);output_size+=size;output[output_size]=0;return size;
}
static UINT32 independent_crc(EFI_BOOT_SERVICES *table) {
  EFI_BOOT_SERVICES copy=*table;copy.Hdr.CRC32=0;
  return (UINT32)crc32(0,(const unsigned char *)&copy,copy.Hdr.HeaderSize);
}
static EFI_STATUS get_map(UINTN *size,EFI_MEMORY_DESCRIPTOR *map,UINTN *mapkey,UINTN *stride,UINT32 *version) {
  ++map_calls;
  if(mode==6)return EFI_SUCCESS; // malformed successful NULL outputs
  if(!size||!mapkey||!stride||!version)return EFI_INVALID_PARAMETER;
  *mapkey=key;*stride=48;*version=1;
  if(mode==1){*size=TRACE_MAP_CAPACITY+48;return EFI_SUCCESS;}
  if(mode==2){*stride=8;*size=16;return EFI_SUCCESS;}
  if(mode==3){*size=95;return EFI_SUCCESS;}
  if(mode==4){*size=96;return EFI_SUCCESS;} // input capacity is tested separately
  if(mode==5){*version=2;*size=96;return EFI_SUCCESS;}
  if(!map||*size<96){*size=96;return EFI_BUFFER_TOO_SMALL;}
  unsigned char *bytes=(void *)map;memset(bytes,0,96);
  EFI_MEMORY_DESCRIPTOR descriptor={.Type=7,.PhysicalStart=0xbd930000,.NumberOfPages=16,.Attribute=8};
  memcpy(bytes,&descriptor,sizeof(descriptor));descriptor.PhysicalStart=0xd1000000;memcpy(bytes+48,&descriptor,sizeof(descriptor));
  *size=96;return EFI_SUCCESS;
}
static EFI_STATUS exit_boot(EFI_HANDLE handle,UINTN mapkey) {
  (void)handle;assert(mapkey==key);++exit_calls;
  if(exit_result==EFI_SUCCESS){assert(mprotect(services,4096,PROT_NONE)==0);}
  return exit_result;
}
static void reset_fixture(void) {
  if(services){assert(mprotect(services,4096,PROT_READ|PROT_WRITE)==0);assert(munmap(services,4096)==0);}
  services=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(services!=MAP_FAILED);
  memset(&mTrace,0,sizeof(mTrace));output_size=0;output[0]=0;mode=0;exit_calls=map_calls=0;key=41;exit_result=EFI_INVALID_PARAMETER;
  *services=(EFI_BOOT_SERVICES){.Hdr={.Signature=EFI_BOOT_SERVICES_SIGNATURE,.HeaderSize=sizeof(*services)},.GetMemoryMap=get_map,.ExitBootServices=exit_boot};
  services->Hdr.CRC32=independent_crc(services);
}
static void capture(void) {
  unsigned char bytes[96];UINTN size=sizeof(bytes),stride,mapkey;UINT32 version;
  assert(services->GetMemoryMap(&size,(void *)bytes,&mapkey,&stride,&version)==EFI_SUCCESS);
}
int main(void) {
  reset_fixture();EFI_BOOT_SERVICES original=*services;
  assert(PianoEfiHandoffTraceInstall(services)==EFI_SUCCESS);
  assert(services->Hdr.CRC32==independent_crc(services));
  assert(PianoEfiHandoffTraceInstall(services)==EFI_ALREADY_STARTED);
  assert(PianoEfiHandoffTraceRestore()==EFI_SUCCESS);assert(memcmp(&original,services,sizeof(original))==0);
  assert(PianoEfiHandoffTraceRestore()==EFI_NOT_STARTED);

  reset_fixture();services->Hdr.CRC32^=1;assert(PianoEfiHandoffTraceInstall(services)==EFI_CRC_ERROR);
  services->Hdr.CRC32^=1;services->Hdr.HeaderSize=sizeof(*services)+1;assert(PianoEfiHandoffTraceInstall(services)==EFI_INVALID_PARAMETER);

  reset_fixture();assert(PianoEfiHandoffTraceInstall(services)==EFI_SUCCESS);
  UINTN size=0,stride,mapkey;UINT32 version;
  assert(services->GetMemoryMap(&size,NULL,&mapkey,&stride,&version)==EFI_BUFFER_TOO_SMALL);assert(!mTrace.HaveMap);
  capture();assert(mTrace.HaveMap&&mTrace.MapSize==96&&mTrace.DescriptorSize==48);
  assert(services->ExitBootServices(NULL,key)==EFI_INVALID_PARAMETER);assert(!PianoEfiHandoffTraceExited());
  assert(strstr(output,"MAP index=1 type=7")&&strstr(output,"EBS_RETURN attempt=1"));
  ++key;capture();EFI_GET_MEMORY_MAP saved_get=services->GetMemoryMap;EFI_EXIT_BOOT_SERVICES saved_exit=services->ExitBootServices;
  exit_result=EFI_SUCCESS;
  assert(saved_exit(NULL,key)==EFI_SUCCESS); // real PROT_NONE table proves no success-path BS dereference
  assert(PianoEfiHandoffTraceExited());assert(PianoEfiHandoffTraceRestore()==EFI_ACCESS_DENIED);
  assert(PianoEfiHandoffTraceInstall(services)==EFI_ACCESS_DENIED);
  assert(saved_get(NULL,NULL,NULL,NULL,NULL)==EFI_ACCESS_DENIED);
  assert(saved_exit(NULL,key)==EFI_ACCESS_DENIED);assert(exit_calls==2);
  assert(strstr(output,"EBS_RETURN attempt=2 status=0x0 success=1"));

  for(int test=1;test<=6;++test){
    reset_fixture();assert(PianoEfiHandoffTraceInstall(services)==EFI_SUCCESS);mode=test;
    unsigned char bytes[96];size=test==4?8:sizeof(bytes);
    if(test==6)assert(services->GetMemoryMap(NULL,NULL,NULL,NULL,NULL)==EFI_SUCCESS);
    else assert(services->GetMemoryMap(&size,(void *)bytes,&mapkey,&stride,&version)==EFI_SUCCESS);
    assert(!mTrace.HaveMap);assert(PianoEfiHandoffTraceRestore()==EFI_SUCCESS);
  }
  reset_fixture();assert(PianoEfiHandoffTraceInstall(services)==EFI_SUCCESS);
  services->ExitBootServices=exit_boot;assert(PianoEfiHandoffTraceRestore()==EFI_ACCESS_DENIED);
  assert(services->ExitBootServices==exit_boot); // do not overwrite another owner
  assert(mprotect(services,4096,PROT_READ|PROT_WRITE)==0);assert(munmap(services,4096)==0);services=NULL;
  puts("EFI trace: CRC, restore, retries, bounds, stride/version and post-EBS PROT_NONE passed");return 0;
}
