// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoNvFvbBinding.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/Variable.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiVariableArchProtocolGuid={.Data1=1},gEfiLoadedImageProtocolGuid={.Data1=2},gEfiEventExitBootServicesGuid={.Data1=3},gEfiEventVirtualAddressChangeGuid={.Data1=4},gEfiFirmwareVolumeBlockProtocolGuid={.Data1=5};
static EFI_BOOT_SERVICES bs;static EFI_RUNTIME_SERVICES rt;static EFI_LOADED_IMAGE_PROTOCOL loaded;
static PIANO_NV_FVB_BINDING *binding;static PIANO_NV_JOURNAL *journal;static UINT8 *mirror;
static UINT32 scenario,creates,installs;static EFI_EVENT_NOTIFY notify[2];static VOID *contexts[2];
VOID *EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID *EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}VOID EFIAPI CpuDeadLoop(VOID){abort();}
EFI_STATUS PianoNvFvbInitialize(PIANO_NV_FVB*b,PIANO_NV_JOURNAL*j){b->Signature=1;b->Journal=j;return EFI_SUCCESS;}
VOID PianoNvFvbFenceRuntime(PIANO_NV_FVB*b){b->Runtime=TRUE;b->Journal->Runtime=TRUE;}
EFI_STATUS PianoNvFvbConvertVirtual(PIANO_NV_FVB*b,EFI_CONVERT_POINTER c){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID*g,VOID**p){assert(h==(VOID*)7&&g==&gEfiLoadedImageProtocolGuid);*p=&loaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI locate(EFI_GUID*g,VOID*r,VOID**p){assert(g==&gEfiVariableArchProtocolGuid);*p=NULL;return scenario==3?EFI_SUCCESS:EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI map(UINTN*n,EFI_MEMORY_DESCRIPTOR*m,UINTN*k,UINTN*s,UINT32*v){assert(*n>=3*sizeof(*m));*n=3*sizeof(*m);*s=sizeof(*m);*v=1;*k=1;
 VOID*ptrs[]={binding,journal,mirror};UINTN sizes[]={sizeof(*binding),sizeof(*journal),PIANO_NV_SNAPSHOT_BYTES};for(UINTN i=0;i<3;i++)m[i]=(EFI_MEMORY_DESCRIPTOR){.Type=scenario==2?EfiBootServicesData:EfiRuntimeServicesData,.PhysicalStart=(UINTN)ptrs[i],.NumberOfPages=(sizes[i]+4095)/4096,.Attribute=EFI_MEMORY_RUNTIME};return EFI_SUCCESS;}
static EFI_STATUS EFIAPI create(UINT32 t,EFI_TPL p,EFI_EVENT_NOTIFY fn,CONST VOID*c,CONST EFI_GUID*g,EFI_EVENT*out){assert(creates<2&&p==TPL_NOTIFY);notify[creates]=fn;contexts[creates]= (VOID*)c;*out=(VOID*)(UINTN)(++creates);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI install(EFI_HANDLE*h,...){++installs;*h=(VOID*)8;return EFI_SUCCESS;}
int main(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);binding=aligned_alloc(4096,4096);journal=aligned_alloc(4096,16384);mirror=aligned_alloc(4096,PIANO_NV_SNAPSHOT_BYTES);assert(binding&&journal&&mirror);memset(binding,0,4096);memset(journal,0,16384);
 journal->Ready=TRUE;journal->Mirror=mirror;journal->Sequence=1;journal->Io.VolumeUuid[0]=1;
 UINTN code=(UINTN)PianoNvFvbPublish;loaded=(EFI_LOADED_IMAGE_PROTOCOL){.ImageCodeType=scenario==1?EfiLoaderCode:EfiRuntimeServicesCode,.ImageDataType=EfiRuntimeServicesData,.ImageBase=(VOID*)(code&~65535ULL),.ImageSize=131072};
 bs=(EFI_BOOT_SERVICES){.HandleProtocol=handle,.GetMemoryMap=map,.LocateProtocol=locate,.CreateEventEx=create,.InstallMultipleProtocolInterfaces=install};gBS=&bs;gRT=&rt;
 EFI_STATUS e=PianoNvFvbPublish((VOID*)7,binding,journal);if(scenario==1||scenario==2){assert(e==EFI_ACCESS_DENIED&&!creates&&!installs);}else if(scenario==3){assert(e==EFI_ALREADY_STARTED&&!creates&&!installs);}else{assert(e==EFI_SUCCESS&&binding->Installed&&creates==2&&installs==1&&binding->Info.VariableBytes==262144);notify[0]((VOID*)1,contexts[0]);assert(journal->Runtime&&binding->Fvb.Runtime);}
 free(binding);free(journal);free(mirror);puts("Actual runtime FVB publication guards passed");return 0;}
