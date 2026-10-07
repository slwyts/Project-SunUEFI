// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/components/os-boot/PianoLinuxEfiSession.h"
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Protocol/LoadedImage.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}
BOOLEAN EFIAPI Sha256Init(VOID *C){return SHA256_Init(C)==1;}
BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}
BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *D){return SHA256_Final(D,C)==1;}
EFI_GUID gFdtTableGuid={.Data1=1},gEfiLoadedImageProtocolGuid={.Data1=2},gEfiLoadFile2ProtocolGuid={.Data1=3},gEfiDevicePathProtocolGuid={.Data1=4},gEfiEventBeforeExitBootServicesGuid={.Data1=5},gEfiEventExitBootServicesGuid={.Data1=6};
VOID*EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID*EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
static EFI_TPL tpl=TPL_APPLICATION;static EFI_BOOT_SERVICES *bs;static EFI_SYSTEM_TABLE st;static EFI_CONFIGURATION_TABLE table;
static EFI_LOADED_IMAGE_PROTOCOL *loaded;static BOOLEAN image_live,bs_alive=TRUE;static UINTN scenario,takes,loans,releases,unloans,allocs,frees,loads,starts,unloads,pumps,retires,installed,closed;
static UINT8 kernel[1024],dtb[4096],small_initrd[17],*initrd=small_initrd;static UINTN initrd_bytes=sizeof(small_initrd);static PIANO_LINUX_EFI_SESSION session;static jmp_buf halt;
static struct{VOID*ptr;UINTN bytes;}pools[16];static struct{EFI_EVENT_NOTIFY fn;VOID*ctx;BOOLEAN live;}events[2];
static struct src{UINT8*data;UINTN bytes;BOOLEAN owner,loan;}sources[3];
static UINTN memory_calls;
static UINTN cpu_slices,live_usb_slices,retired_cpu_slices,cpu_buffers,cpu_checks;
static EFI_STATUS cpu_slice(VOID*c,UINTN budget){assert(c==(VOID*)1&&budget==1000&&bs_alive);++cpu_slices;
 if(!retires)++live_usb_slices;else ++retired_cpu_slices;return scenario==40?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS cpu_memory(VOID*c,PIANO_LINUX_MEMORY_PROOF*p){assert(c==(VOID*)1&&!starts&&!takes);++cpu_checks;
 *p=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=scenario==41?2:1,.DramBytes=16470685696ULL,
 .NormalBytes=4ULL*1024*1024*1024,.FullDdr=scenario!=37,.FixedReservations=TRUE,.DynamicReservations=TRUE,
 .RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};return EFI_SUCCESS;}
static EFI_STATUS cpu_validate(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*p){assert(c==(VOID*)1&&p->OwnershipVerified);return scenario==38?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS cpu_buffer(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*p,CONST VOID*producer,VOID*owner,CONST VOID*base,UINT64 bytes){
 assert(c==(VOID*)1&&p->OwnershipVerified);CONST struct src*s=producer;
 assert(s>=sources&&s<sources+3&&owner==s&&s->owner&&s->loan&&base==s->data&&bytes==s->bytes);++cpu_buffers;
 return scenario==39&&s==&sources[2]?EFI_ACCESS_DENIED:EFI_SUCCESS;}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(halt,1);}
static VOID fail(VOID*c,EFI_STATUS s){assert(c==(VOID*)1&&s!=EFI_SUCCESS);longjmp(halt,1);}
static BOOLEAN alive(VOID*c){return bs_alive;}
static EFI_STATUS take(VOID*c,VOID**o){struct src*s=c;assert(!s->owner);s->owner=TRUE;*o=s;++takes;if(scenario==22)return EFI_DEVICE_ERROR;if(scenario==23)return EFI_WARN_STALE_DATA;return EFI_SUCCESS;}
static EFI_STATUS read_source(VOID*c,VOID*o,UINT64 a,UINTN n,VOID*b){struct src*s=c;assert(o==s&&s->owner&&a+n<=s->bytes);memcpy(b,s->data+a,n);return EFI_SUCCESS;}
static EFI_STATUS borrow(VOID*c,VOID*o,PIANO_BOOT_RANGE r,CONST VOID**v,VOID**l){struct src*s=c;assert(o==s&&s->owner&&!s->loan&&r.Offset==0&&r.Bytes==s->bytes);s->loan=TRUE;*v=s->data;*l=s;++loans;
 if(scenario==24)return EFI_DEVICE_ERROR;if(scenario==25)return EFI_WARN_STALE_DATA;if(scenario==27)*v=&session;if(scenario==28&&s==&sources[1])*v=kernel;if(scenario==32){*l=NULL;return EFI_DEVICE_ERROR;}if(scenario==33)*v=(VOID*)(MAX_UINTN-1);return EFI_SUCCESS;}
static EFI_STATUS unborrow(VOID*c,VOID*o,VOID*l){struct src*s=c;assert(o==s&&l==s&&s->loan&&!image_live&&!installed);if(scenario==13)return EFI_DEVICE_ERROR;s->loan=FALSE;++unloans;return EFI_SUCCESS;}
static EFI_STATUS release(VOID*c,VOID*o){struct src*s=c;assert(o==s&&s->owner&&!s->loan&&!image_live&&!installed);s->owner=FALSE;++releases;return EFI_SUCCESS;}
static EFI_TPL EFIAPI raise(EFI_TPL n){EFI_TPL o=tpl;tpl=n;return o;}static VOID EFIAPI restore(EFI_TPL o){tpl=o;}
static EFI_STATUS EFIAPI allocate(EFI_MEMORY_TYPE t,UINTN n,VOID**p){assert(t==EfiLoaderData);for(UINTN i=0;i<16;i++)if(!pools[i].ptr){pools[i].ptr=calloc(1,n);pools[i].bytes=n;*p=pools[i].ptr;++allocs;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI free_pool(VOID*p){assert(bs_alive&&!image_live);if(scenario==11)return EFI_WARN_UNKNOWN_GLYPH;for(UINTN i=0;i<16;i++)if(pools[i].ptr==p){free(p);pools[i].ptr=NULL;++frees;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI config(EFI_GUID*g,VOID*p){assert(g==&gFdtTableGuid&&bs_alive);table.VendorGuid=*g;table.VendorTable=p;st.NumberOfTableEntries=p?1:0;st.ConfigurationTable=&table;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI install(EFI_HANDLE*h,...){assert(!installed);installed=1;*h=(VOID*)3;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE h,...){assert(h==(VOID*)3&&installed&&!image_live);if(scenario==9)return EFI_DEVICE_ERROR;installed=0;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI locate_path(EFI_GUID*g,EFI_DEVICE_PATH_PROTOCOL**p,EFI_HANDLE*h){assert(g==&gEfiLoadFile2ProtocolGuid&&(*p)->Type==MEDIA_DEVICE_PATH);if(scenario==17){*h=(VOID*)9;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI create(UINT32 t,EFI_TPL p,EFI_EVENT_NOTIFY f,CONST VOID*c,CONST EFI_GUID*g,EFI_EVENT*e){assert(p==TPL_NOTIFY&&t==EVT_NOTIFY_SIGNAL);UINTN i=g==&gEfiEventBeforeExitBootServicesGuid?0:1;events[i].fn=f;events[i].ctx=(VOID*)c;events[i].live=TRUE;*e=&events[i];return EFI_SUCCESS;}
static EFI_STATUS EFIAPI close_event(EFI_EVENT e){assert(bs_alive);if(scenario==12)return EFI_DEVICE_ERROR;for(UINTN i=0;i<2;i++)if(e==&events[i]){events[i].live=FALSE;++closed;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI load(BOOLEAN b,EFI_HANDLE p,EFI_DEVICE_PATH_PROTOCOL*path,VOID*d,UINTN n,EFI_HANDLE*h){assert(!b&&p==(VOID*)2&&!path&&d==kernel&&n==1024&&!retires);++loads;if(scenario==7)return EFI_SECURITY_VIOLATION;image_live=TRUE;assert(mprotect(loaded,4096,PROT_READ|PROT_WRITE)==0);*loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=kernel,.ImageSize=8192,.ImageCodeType=EfiLoaderCode};*h=(VOID*)4;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID*g,VOID**p){assert(h==(VOID*)4&&g==&gEfiLoadedImageProtocolGuid);*p=image_live?loaded:NULL;return image_live?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
static EFI_STATUS EFIAPI unload(EFI_HANDLE h){assert(h==(VOID*)4&&image_live);++unloads;image_live=FALSE;return EFI_SUCCESS;}
static EFI_STATUS pump(VOID*c,UINTN b){assert(b==1000&&image_live&&!retires);++pumps;return scenario==8?EFI_ABORTED:EFI_SUCCESS;}
static EFI_STATUS memory(VOID*c,CONST VOID*d,UINTN n,PIANO_LINUX_MEMORY_PROOF*p){assert(!starts&&fdt_check_header(d)==0&&n==(UINTN)fdt_totalsize(d));++memory_calls;if(scenario==1||(scenario==21&&memory_calls==2))return EFI_NOT_READY;*p=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=scenario==18&&memory_calls==2?2:1,.DramBytes=16470685696ULL,.NormalBytes=4ULL*1024*1024*1024,.FullDdr=TRUE,.FixedReservations=TRUE,.DynamicReservations=TRUE,.RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=scenario!=2};return EFI_SUCCESS;}
static EFI_STATUS validate_memory(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*p){assert(c==(VOID*)1&&p->OwnershipVerified);return scenario==16?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS validate_retired(VOID*c,CONST PIANO_LINUX_RETIRE_PROOF*p){assert(c==(VOID*)1&&retires==1&&p->Clean&&!p->Retained);return scenario==20?EFI_ACCESS_DENIED:EFI_SUCCESS;}
static EFI_STATUS retire(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*m,PIANO_LINUX_RETIRE_PROOF*r){assert(pumps==1&&loads==1&&!starts&&tpl==TPL_APPLICATION&&installed&&m->OwnershipVerified);++retires;if(scenario==3)return EFI_DEVICE_ERROR;*r=(PIANO_LINUX_RETIRE_PROOF){.Revision=1,.ExpectedOwners=31,.RetiredOwners=31,.Status=EFI_SUCCESS,.Clean=TRUE,.NoDma=TRUE,.AtApplication=TRUE,.Retained=scenario==4};return EFI_SUCCESS;}
static EFI_STATUS EFIAPI start(EFI_HANDLE h,UINTN*n,CHAR16**d){assert(h==(VOID*)4&&retires==1&&image_live&&loaded->LoadOptions&&installed);++starts;
 EFI_DEVICE_PATH_PROTOCOL end={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};UINTN bytes=0;assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,NULL)==EFI_BUFFER_TOO_SMALL&&bytes==initrd_bytes);
 UINT8*out=malloc(initrd_bytes);assert(out);bytes=initrd_bytes;assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,out)==EFI_SUCCESS&&!memcmp(out,initrd,initrd_bytes));
 assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,kernel)==EFI_INVALID_PARAMETER);
 assert(session.Load.LoadFile(&session.Load,&end,FALSE,(UINTN *)&session,NULL)==EFI_INVALID_PARAMETER);
 assert(session.Load.LoadFile(&session.Load,&end,TRUE,&bytes,out)==EFI_UNSUPPORTED);end.Length[0]=5;assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,out)==EFI_INVALID_PARAMETER);free(out);
 int chosen=fdt_path_offset(table.VendorTable,"/chosen"),len;assert(fdt_getprop(table.VendorTable,chosen,"linux,initrd-start",&len)==NULL);assert(!strcmp(fdt_getprop(table.VendorTable,chosen,"bootargs",&len),"rdinit=/init ro"));
 if(scenario==5||scenario==6){events[scenario==5?0:1].fn(&events[scenario==5?0:1],events[scenario==5?0:1].ctx);if(scenario==6)bs_alive=FALSE;assert(mprotect(bs,4096,PROT_NONE)==0);return EFI_SUCCESS;}
 if(scenario==10){loaded->ImageBase=(VOID*)0x8888;return EFI_SUCCESS;}
 if(scenario==14){table.VendorTable=(VOID*)0x9999;}
 if(scenario==15)return EFI_LOAD_ERROR;
 image_live=FALSE;assert(mprotect(loaded,4096,PROT_NONE)==0);*n=0;*d=NULL;return EFI_SUCCESS;}
static VOID p16(UINTN o,UINT16 v){memcpy(kernel+o,&v,2);}static VOID p32(UINTN o,UINT32 v){memcpy(kernel+o,&v,4);}static VOID p64(UINTN o,UINT64 v){memcpy(kernel+o,&v,8);}
int main(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);assert(scenario<42);p16(0,0x5a4d);p32(60,128);p32(128,0x4550);p16(132,0xaa64);p16(134,1);p16(148,240);p16(150,2);p16(152,0x20b);p32(168,4096);p64(176,0x10000000);p32(184,4096);p32(188,512);p32(208,8192);p32(212,512);p16(220,10);p32(260,16);memcpy(kernel+392,".text",5);p32(400,512);p32(404,4096);p32(408,512);p32(412,512);p32(428,0x60000020);
 if(scenario>=35){initrd_bytes=PIANO_CPU_INPUT_LOW_BYTES+1;initrd=calloc(1,initrd_bytes);assert(initrd);}
 assert(fdt_create_empty_tree(dtb,sizeof(dtb))==0);int node=fdt_add_subnode(dtb,0,"chosen");UINT32 v=1;assert(fdt_setprop(dtb,node,"linux,initrd-start",&v,4)==0);assert(fdt_pack(dtb)==0);memset(initrd,0xa1,initrd_bytes);
 sources[0]=(struct src){kernel,sizeof(kernel)};sources[1]=(struct src){dtb,(UINTN)fdt_totalsize(dtb)};sources[2]=(struct src){initrd,initrd_bytes};
 PIANO_LAUNCH_BLOB blobs[3];for(UINTN i=0;i<3;i++)blobs[i]=(PIANO_LAUNCH_BLOB){.Context=&sources[i],.Bytes=sources[i].bytes,.Take=take,.Read=read_source,.BorrowView=borrow,.Unborrow=unborrow,.ZeroRelease=release};
 bs=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);loaded=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bs!=MAP_FAILED&&loaded!=MAP_FAILED);
 *bs=(EFI_BOOT_SERVICES){.RaiseTPL=raise,.RestoreTPL=restore,.AllocatePool=allocate,.FreePool=free_pool,.InstallConfigurationTable=config,.InstallMultipleProtocolInterfaces=install,.UninstallMultipleProtocolInterfaces=uninstall,.LocateDevicePath=locate_path,.CreateEventEx=create,.CloseEvent=close_event,.LoadImage=load,.HandleProtocol=handle,.UnloadImage=unload,.StartImage=start};st.BootServices=bs;
 PIANO_LINUX_EFI_ENV env={.Context=(VOID*)1,.Services=bs,.SystemTable=&st,.ParentImage=(VOID*)2,.MaxKernelBytes=4096,.MaxLoadedBytes=8192,.MaxDtbBytes=4096,.MaxInitrdBytes=4096,.ExpectedDramBytes=16470685696ULL,.ExpectedOwners=31,.CommandLine=L"rdinit=/init ro",.BootServicesAlive=alive,.ServiceSlice=pump,.CheckMemory=memory,.ValidateMemory=scenario==19?NULL:validate_memory,.PrepareHandoff=retire,.ValidateRetired=validate_retired,.FailStop=fail};
 PIANO_CPU_INPUT_ENV cpu_env={.Context=(VOID*)1,.BootServicesAlive=alive,.ServiceSlice=cpu_slice,.CheckMemory=cpu_memory,.ValidateMemory=cpu_validate,.ValidateBuffer=cpu_buffer};
 if(scenario>=35){env.MaxInitrdBytes=PIANO_CPU_INPUT_MAX_BYTES;env.MaxSourceBytes=PIANO_CPU_INPUT_MAX_BYTES;env.Cpu=scenario==36?NULL:&cpu_env;}
 if(scenario==26)blobs[1].Context=blobs[0].Context;
 if(scenario==29){blobs[0].Bytes=PIANO_LINUX_LOW_SOURCE_BUDGET-16;env.MaxKernelBytes=PIANO_LINUX_LOW_SOURCE_BUDGET;}
 if(scenario==30){blobs[0].Bytes=MAX_UINT64;env.MaxKernelBytes=MAX_UINT64;}
 if(scenario==31)blobs[1].Context=&session.Status;
 CONST PIANO_LAUNCH_BLOB*Kernel=scenario==34?&session.Sources[0]:&blobs[0];
 BOOLEAN fatal=FALSE;EFI_STATUS status=EFI_SUCCESS;if(!setjmp(halt))status=PianoLinuxEfiSessionRun(&session,&env,Kernel,&blobs[1],&blobs[2]);else fatal=TRUE;
 if(scenario==35){assert(!fatal&&status==EFI_ABORTED&&starts==1&&retires==1&&releases==3&&cpu_checks==1&&cpu_buffers>=4&&live_usb_slices>0&&retired_cpu_slices>1024);}
 else if(scenario==36||scenario==37||scenario==38)assert(!fatal&&status==EFI_NOT_READY&&!takes&&!starts&&!retires&&!allocs);
 else if(scenario==39)assert(fatal&&session.Retained&&!starts&&!retires&&!releases&&cpu_buffers==3);
 else if(scenario==40)assert(!fatal&&status==EFI_DEVICE_ERROR&&!starts&&!retires&&releases==3&&allocs==frees);
 else if(scenario==41)assert(!fatal&&status==EFI_NOT_READY&&!starts&&!retires&&releases==3&&allocs==frees);
 else if(scenario==1||scenario==2||scenario==16)assert(!fatal&&status==EFI_NOT_READY&&!starts&&!retires&&!loads&&releases==3&&allocs==frees);
 else if(scenario==3||scenario==4||scenario==5||scenario==6||scenario==20)assert(fatal&&session.Retained&&releases==0);
 else if(scenario==17)assert(!fatal&&status==EFI_ALREADY_STARTED&&!starts&&!retires&&!loads&&releases==3&&allocs==frees);
 else if(scenario==18||scenario==21)assert(!fatal&&status==EFI_NOT_READY&&retires==1&&!starts&&releases==3&&allocs==frees&&session.OwnersRetired);
 else if(scenario==19)assert(!fatal&&status==EFI_NOT_READY&&!takes&&!starts&&!retires);
 else if(scenario==26||scenario==31||scenario==34)assert(!fatal&&status==EFI_INVALID_PARAMETER&&!takes&&!loans&&!releases&&!allocs);
 else if(scenario==29||scenario==30)assert(!fatal&&status==EFI_BAD_BUFFER_SIZE&&!takes&&!loans&&!releases&&!allocs);
 else if((scenario>=22&&scenario<=28)||scenario==32||scenario==33){assert(fatal&&session.Retained&&!releases&&!unloans&&!frees&&!starts&&!retires);if(scenario==28)assert(takes==2&&loans==2);else assert(takes==1);}
 else if(scenario==7||scenario==8)assert(!fatal&&!starts&&!retires&&releases==3&&allocs==frees&&status==(scenario==7?EFI_SECURITY_VIOLATION:EFI_ABORTED));
 else if(scenario>=9&&scenario<=14)assert(!fatal&&session.Retained&&releases==0);
 else assert(!fatal&&status==(scenario==15?EFI_LOAD_ERROR:EFI_ABORTED)&&session.OwnersRetired&&session.StartReturned&&!session.Retained&&releases==3&&unloans==3&&allocs==frees&&!installed);
 assert(mprotect(bs,4096,PROT_READ|PROT_WRITE)==0&&mprotect(loaded,4096,PROT_READ|PROT_WRITE)==0);for(UINTN i=0;i<16;i++)free(pools[i].ptr);munmap(bs,4096);munmap(loaded,4096);if(initrd!=small_initrd)free(initrd);
 printf("Actual Linux EFI session scenario%lu passed; CPU slices=%lu live-usb-phase=%lu retired-CPU-only=%lu\n",(unsigned long)scenario,(unsigned long)cpu_slices,(unsigned long)live_usb_slices,(unsigned long)retired_cpu_slices);return 0;}
