// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoProductOsController.h"
#include "../../uefi/core/PianoUfsProductVolume.h"
EFI_GUID gEfiSimpleFileSystemProtocolGuid={.Data1=1},gEfiBlockIoProtocolGuid={.Data1=2},gEfiDevicePathProtocolGuid={.Data1=3},gEfiEventExitBootServicesGuid={.Data1=4};
VOID *EFIAPI ZeroMem(VOID *p,UINTN n){return memset(p,0,n);}VOID *EFIAPI CopyMem(VOID *a,CONST VOID *b,UINTN n){return memmove(a,b,n);}INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return memcmp(a,b,n);}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}BOOLEAN EFIAPI Sha256Init(VOID *p){return SHA256_Init(p)==1;}BOOLEAN EFIAPI Sha256Update(VOID *p,CONST VOID *b,UINTN n){return SHA256_Update(p,b,n)==1;}BOOLEAN EFIAPI Sha256Final(VOID *p,UINT8 *b){return SHA256_Final(b,p)==1;}
static EFI_BOOT_SERVICES bs;static EFI_SYSTEM_TABLE st;static EFI_TPL tpl=TPL_APPLICATION;static BOOLEAN alive=TRUE;
static PIANO_PRODUCT_RUNTIME_PROTOCOL runtime;static PIANO_PRODUCT_OWNERS owners;static PIANO_PRODUCT_OS_CONTROLLER controller;
static EFI_BLOCK_IO_PROTOCOL block;static EFI_BLOCK_IO_MEDIA media;static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;static UINT8 path[40],uuid[16]={1};
static UINTN scenario,volume_calls,locate_calls,file_calls,session_calls,events;
static BOOLEAN Live(VOID *p){(VOID)p;return alive;}static BOOLEAN EFIAPI RuntimeLive(PIANO_PRODUCT_RUNTIME_PROTOCOL *p){return p==&runtime&&alive;}
static EFI_TPL EFIAPI Raise(EFI_TPL n){EFI_TPL old=tpl;tpl=n;return old;}static VOID EFIAPI Restore(EFI_TPL n){tpl=n;}
static EFI_STATUS EFIAPI Event(UINT32 t,EFI_TPL p,EFI_EVENT_NOTIFY n,CONST VOID *c,CONST EFI_GUID *g,EFI_EVENT *e){(VOID)t;(VOID)p;(VOID)n;(VOID)c;(VOID)g;*e=(VOID*)1;++events;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT e){(VOID)e;return EFI_SUCCESS;}static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *g,VOID *r,VOID **p){(VOID)g;(VOID)r;*p=&runtime;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *k,UINTN *n,EFI_HANDLE **h){(VOID)t;(VOID)g;(VOID)k;++locate_calls;if(scenario==2){*n=0;*h=NULL;return EFI_NOT_FOUND;}*n=scenario==4?2:1;*h=calloc(*n,sizeof(**h));for(UINTN i=0;i<*n;++i)(*h)[i]=(VOID*)(UINTN)(i+7);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p){(VOID)h;if(g==&gEfiBlockIoProtocolGuid)*p=&block;else if(g==&gEfiDevicePathProtocolGuid)*p=path;else if(g==&gEfiSimpleFileSystemProtocolGuid)*p=&sfs;else abort();return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *p,EFI_FILE_PROTOCOL **f){(VOID)p;(VOID)f;abort();}
static EFI_STATUS Selection(VOID *p,PIANO_PRODUCT_OS_FAMILY f,UINT64 q){(VOID)p;assert(f==PianoProductOsStable||f==PianoProductOsNext);return q==11&&scenario!=1?EFI_SUCCESS:EFI_ACCESS_DENIED;}
static EFI_STATUS Volume(VOID *p,EFI_BLOCK_IO_PROTOCOL **b,UINT8 u[16]){(VOID)p;++volume_calls;*b=&block;memcpy(u,uuid,16);return EFI_SUCCESS;}
static EFI_STATUS Slice(VOID *p,UINTN n){(VOID)p;(VOID)n;return EFI_SUCCESS;}
static EFI_STATUS Memory(VOID *p,PIANO_LINUX_MEMORY_PROOF *m){(VOID)p;if(scenario==0)return EFI_NOT_READY;*m=(PIANO_LINUX_MEMORY_PROOF){1,EFI_SUCCESS,1,4ULL<<30,2ULL<<30,0,TRUE,TRUE,TRUE,TRUE,TRUE,TRUE};return EFI_SUCCESS;}
static EFI_STATUS Validate(VOID *p,CONST PIANO_LINUX_MEMORY_PROOF *m){(VOID)p;assert(m->BootEpoch==1);return EFI_SUCCESS;}
static EFI_STATUS Buffer(VOID *p,CONST PIANO_LINUX_MEMORY_PROOF *m,CONST VOID *producer,VOID *owner,CONST VOID *base,UINT64 bytes){(VOID)p;(VOID)m;(VOID)producer;(VOID)owner;(VOID)base;(VOID)bytes;return EFI_SUCCESS;}
static EFI_STATUS LinuxMemory(VOID *p,CONST VOID *d,UINTN n,PIANO_LINUX_MEMORY_PROOF *m){(VOID)d;(VOID)n;return Memory(p,m);}
static VOID Fail(VOID *p,EFI_STATUS e){(VOID)p;(VOID)e;abort();}
static EFI_STATUS Arm(VOID *p,EFI_HANDLE h,CONST EFI_LOADED_IMAGE_PROTOCOL *l){(VOID)p;(VOID)h;(VOID)l;abort();}static EFI_STATUS Disarm(VOID *p,EFI_HANDLE h){(VOID)p;(VOID)h;abort();}
EFI_STATUS PianoBootFileLoadBundle(PIANO_BOOT_FILE_SOURCE *f,UINTN n,CONST PIANO_BOOT_FILE_ENV *e,CONST PIANO_BOOT_FILE_SPEC *s,UINT64 b,PIANO_BOOT_SOURCE *r,PIANO_LAUNCH_BLOB *l){(VOID)f;(VOID)e;(VOID)r;(VOID)l;assert(n==3&&b>64ULL*1024*1024);assert(s[2].AbsolutePath[0]=='\\');++file_calls;return EFI_NOT_FOUND;}
EFI_STATUS PianoBootFileDispose(PIANO_BOOT_FILE_SOURCE *f){(VOID)f;abort();}
EFI_STATUS PianoLinuxEfiSessionRun(PIANO_LINUX_EFI_SESSION *s,CONST PIANO_LINUX_EFI_ENV *e,CONST PIANO_LAUNCH_BLOB *k,CONST PIANO_LAUNCH_BLOB *d,CONST PIANO_LAUNCH_BLOB *i){(VOID)s;(VOID)e;(VOID)k;(VOID)d;(VOID)i;++session_calls;abort();}
int main(int argc,char **argv){assert(argc==2);scenario=(UINTN)strtoul(argv[1],NULL,10);bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.FreePool=Free,.CreateEventEx=Event,.CloseEvent=Close};st.BootServices=&bs;runtime.Revision=1;runtime.BootServicesAlive=RuntimeLive;owners.Report.Initialized=TRUE;owners.Report.Phase=PianoProductOwnersReturnRequested;owners.Config.Runtime=&runtime;owners.Config.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK;block.Media=&media;media.MediaPresent=TRUE;sfs.OpenVolume=Open;path[0]=HARDWARE_DEVICE_PATH;path[1]=HW_VENDOR_DP;path[2]=36;EFI_GUID guid=PIANO_PRODUCT_STORAGE_TYPE_GUID;memcpy(path+4,&guid,16);memcpy(path+20,uuid,16);path[36]=END_DEVICE_PATH_TYPE;path[37]=END_ENTIRE_DEVICE_PATH_SUBTYPE;path[38]=4;if(scenario==3)path[20]^=1;
PIANO_PRODUCT_OS_PIN pin={.ImageBytes=40ULL<<20,.DtbBytes=2ULL<<20,.InitrdBytes=880ULL<<20};memset(pin.ImageSha256,1,32);memset(pin.DtbSha256,2,32);memset(pin.InitrdSha256,3,32);
PIANO_PRODUCT_OS_ENV e={.Revision=1,.Services=&bs,.SystemTable=&st,.ParentImage=(VOID*)2,.Runtime=&runtime,.Owners=&owners,.BootServicesAlive=Live,.ApprovedVolume=Volume,.ValidateSelection=Selection,.Cpu={NULL,Live,Slice,Memory,Validate,Buffer},.Linux={.Services=&bs,.SystemTable=&st,.ParentImage=(VOID*)2,.BootServicesAlive=Live,.ServiceSlice=Slice,.CheckMemory=LinuxMemory,.ValidateMemory=Validate,.FailStop=Fail,.HandoffMode=PianoHandoffNativeLate,.NativeLateArm=Arm,.NativeLateDisarm=Disarm},.Stable=pin,.Next=pin};
assert(PianoProductOsInitialize(&controller,&e)==EFI_SUCCESS&&events==1);EFI_STATUS status=PianoProductOsRun(&controller,PianoProductOsStable,11);
if(scenario==0){assert(status==EFI_NOT_READY&&!volume_calls&&!locate_calls&&!file_calls);}else if(scenario==1){assert(status==EFI_ACCESS_DENIED&&!volume_calls&&!file_calls);}else if(scenario==2){assert(status==EFI_NOT_FOUND&&volume_calls==1&&locate_calls==1&&!file_calls);}else if(scenario==3){assert(status==EFI_ACCESS_DENIED&&!file_calls);}else if(scenario==4){assert(status==EFI_COMPROMISED_DATA&&!file_calls);}else{assert(scenario==5&&status==EFI_NOT_FOUND&&file_calls==1);}
assert(!session_calls&&!controller.Report.StartCalled&&!controller.Report.OwnersRetired&&owners.Report.Phase==PianoProductOwnersReturnRequested&&!owners.Report.Retained);puts("product OS preflight preserves actual owners and rejects unknown readiness/volume");return 0;}
