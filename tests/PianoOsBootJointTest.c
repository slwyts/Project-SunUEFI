// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual file reader -> immutable blobs -> actual Linux EFI session. The only
// boundaries mocked are SFS, platform/owner validation and EFI Boot Services.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/os-boot/PianoBootFileSource.h"
#include "../bootprofiles/os-boot/PianoLinuxEfiSession.h"
#include <Guid/FileInfo.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Protocol/LoadedImage.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
EFI_GUID gEfiSimpleFileSystemProtocolGuid={.Data1=1},gEfiFileInfoGuid={.Data1=2},gEfiEventExitBootServicesGuid={.Data1=3},gEfiEventBeforeExitBootServicesGuid={.Data1=4},gFdtTableGuid={.Data1=5},gEfiLoadedImageProtocolGuid={.Data1=6},gEfiLoadFile2ProtocolGuid={.Data1=7},gEfiDevicePathProtocolGuid={.Data1=8};
VOID *EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID *EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}BOOLEAN EFIAPI Sha256Init(VOID*c){return SHA256_Init(c)==1;}BOOLEAN EFIAPI Sha256Update(VOID*c,CONST VOID*p,UINTN n){return SHA256_Update(c,p,n)==1;}BOOLEAN EFIAPI Sha256Final(VOID*c,UINT8*d){return SHA256_Final(d,c)==1;}
static UINTN scenario,sfs_calls,file_closes,root_closes,allocs,frees,event_closes,loads,starts,retire_calls,pumps,memory_calls;
static BOOLEAN bs_alive=TRUE,ui_alive=TRUE,sfs_sealed,image_live;
static EFI_BOOT_SERVICES *bs;static EFI_SYSTEM_TABLE st;static EFI_CONFIGURATION_TABLE table;static EFI_LOADED_IMAGE_PROTOCOL *loaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;static EFI_FILE_PROTOCOL roots[3],files[3];
static UINT8 kernel[1024],dtb[4096],initrd[4097],hashes[3][32];static UINT8 *contents[3];static UINTN sizes[3];static UINT64 position[3];
static CONST CHAR16 *paths[3]={L"\\EFI\\Piano\\Image.efi",L"\\EFI\\Piano\\piano.dtb",L"\\EFI\\Piano\\initrd.img"};
static CONST CHAR16 *names[3]={L"Image.efi",L"piano.dtb",L"initrd.img"};
static PIANO_BOOT_FILE_SOURCE file_sources[3];static PIANO_BOOT_SOURCE readers[3];static PIANO_LAUNCH_BLOB blobs[3];static PIANO_LINUX_EFI_SESSION session;
static struct {VOID*ptr;UINTN bytes;BOOLEAN source;}pools[12];
static struct {EFI_EVENT_NOTIFY fn;VOID*ctx;CONST EFI_GUID*group;BOOLEAN live;}events[8];static jmp_buf halt;
static VOID CheckBs(VOID){assert(bs_alive);}static VOID CheckSfs(VOID){CheckBs();assert(!sfs_sealed&&ui_alive);++sfs_calls;}
static BOOLEAN BsAlive(VOID*c){assert(c==(VOID*)1);return bs_alive;}static BOOLEAN BadUiAlive(VOID*c){assert(c==(VOID*)1);return bs_alive&&ui_alive;}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(halt,1);}static VOID Fail(VOID*c,EFI_STATUS s){assert(c==(VOID*)1&&s!=EFI_SUCCESS);longjmp(halt,1);}
static UINTN Index(EFI_FILE_PROTOCOL *p,BOOLEAN root){for(UINTN i=0;i<3;i++)if(p==(root?&roots[i]:&files[i]))return i;abort();}
static EFI_TPL tpl=TPL_APPLICATION;static EFI_TPL EFIAPI Raise(EFI_TPL n){CheckBs();EFI_TPL o=tpl;tpl=n;return o;}static VOID EFIAPI Restore(EFI_TPL o){CheckBs();tpl=o;}
static EFI_STATUS EFIAPI Allocate(EFI_MEMORY_TYPE t,UINTN n,VOID**out){CheckBs();assert(t==EfiLoaderData&&allocs<12);*out=calloc(1,n);assert(*out);pools[allocs]=(typeof(pools[0])){*out,n,!sfs_sealed};++allocs;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID*p){CheckBs();assert(!image_live);for(UINTN i=0;i<allocs;i++)if(pools[i].ptr==p){if(pools[i].source)for(UINTN q=0;q<pools[i].bytes;q++)assert(((UINT8*)p)[q]==0);free(p);pools[i].ptr=NULL;++frees;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI Create(UINT32 type,EFI_TPL t,EFI_EVENT_NOTIFY fn,CONST VOID*c,CONST EFI_GUID*g,EFI_EVENT*out){CheckBs();assert(type==EVT_NOTIFY_SIGNAL&&t==TPL_NOTIFY);for(UINTN i=0;i<8;i++)if(!events[i].live){events[i]=(typeof(events[0])){fn,(VOID*)c,g,TRUE};*out=&events[i];return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT e){CheckBs();for(UINTN i=0;i<8;i++)if(e==&events[i]){assert(events[i].live);events[i].live=FALSE;++event_closes;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI FileClose(EFI_FILE_PROTOCOL*p){CheckSfs();Index(p,FALSE);++file_closes;return EFI_SUCCESS;}static EFI_STATUS EFIAPI RootClose(EFI_FILE_PROTOCOL*p){CheckSfs();Index(p,TRUE);++root_closes;return EFI_SUCCESS;}
static UINTN root_open_index;
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*p,EFI_FILE_PROTOCOL**out){CheckSfs();assert(p==&sfs&&root_open_index<3);*out=&roots[root_open_index++];return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL*p,EFI_FILE_PROTOCOL**out,CHAR16*path,UINT64 mode,UINT64 a){CheckSfs();UINTN i=Index(p,TRUE);assert(mode==EFI_FILE_MODE_READ&&!a);for(UINTN q=0;paths[i][q]||path[q];q++)assert(paths[i][q]==path[q]);*out=&files[i];position[i]=0;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Seek(EFI_FILE_PROTOCOL*p,UINT64 at){CheckSfs();UINTN i=Index(p,FALSE);assert(at<=sizes[i]);position[i]=at;return EFI_SUCCESS;}static EFI_STATUS EFIAPI Pos(EFI_FILE_PROTOCOL*p,UINT64*out){CheckSfs();*out=position[Index(p,FALSE)];return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL*p,UINTN*n,VOID*b){CheckSfs();UINTN i=Index(p,FALSE),bytes=MIN(*n,sizes[i]-(UINTN)position[i]);memcpy(b,contents[i]+position[i],bytes);position[i]+=bytes;*n=bytes;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL*p,EFI_GUID*g,UINTN*n,VOID*b){CheckSfs();assert(g==&gEfiFileInfoGuid);UINTN i=Index(p,FALSE),chars=0;while(names[i][chars])++chars;UINTN need=SIZE_OF_EFI_FILE_INFO+(chars+1)*2;if(!b){*n=need;return EFI_BUFFER_TOO_SMALL;}assert(*n>=need);memset(b,0,*n);EFI_FILE_INFO*f=b;f->Size=need;f->FileSize=sizes[i];f->PhysicalSize=(sizes[i]+4095)&~4095ULL;f->CreateTime.Year=f->ModificationTime.Year=2026;memcpy(f->FileName,names[i],(chars+1)*2);*n=need;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID*g,VOID**out){CheckBs();if(g==&gEfiSimpleFileSystemProtocolGuid){CheckSfs();assert(h==(VOID*)7);*out=&sfs;return EFI_SUCCESS;}assert(g==&gEfiLoadedImageProtocolGuid&&h==(VOID*)9);*out=image_live?loaded:NULL;return image_live?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
static EFI_STATUS EFIAPI Config(EFI_GUID*g,VOID*p){CheckBs();assert(g==&gFdtTableGuid);table=(EFI_CONFIGURATION_TABLE){*g,p};st.NumberOfTableEntries=p?1:0;st.ConfigurationTable=&table;return EFI_SUCCESS;}
static BOOLEAN initrd_installed;static EFI_STATUS EFIAPI Install(EFI_HANDLE*h,...){CheckBs();assert(!initrd_installed);initrd_installed=TRUE;*h=(VOID*)8;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE h,...){CheckBs();assert(h==(VOID*)8&&initrd_installed&&!image_live);initrd_installed=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LocatePath(EFI_GUID*g,EFI_DEVICE_PATH_PROTOCOL**p,EFI_HANDLE*h){CheckBs();assert(g==&gEfiLoadFile2ProtocolGuid);return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI Load(BOOLEAN b,EFI_HANDLE p,EFI_DEVICE_PATH_PROTOCOL*path,VOID*data,UINTN bytes,EFI_HANDLE*h){CheckBs();assert(!b&&p==(VOID*)2&&!path&&data==file_sources[0].Data&&bytes==sizes[0]&&!retire_calls&&ui_alive);++loads;image_live=TRUE;*loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=file_sources[0].Data,.ImageSize=8192,.ImageCodeType=EfiLoaderCode};*h=(VOID*)9;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE h){CheckBs();assert(h==(VOID*)9&&image_live);image_live=FALSE;return EFI_SUCCESS;}
static EFI_STATUS Pump(VOID*c,UINTN b){assert(c==(VOID*)1&&b==1000&&ui_alive&&!retire_calls&&image_live);++pumps;return EFI_SUCCESS;}
static EFI_STATUS Memory(VOID*c,CONST VOID*d,UINTN n,PIANO_LINUX_MEMORY_PROOF*out){CheckBs();assert(c==(VOID*)1&&fdt_check_header(d)==0&&n==(UINTN)fdt_totalsize(d));++memory_calls;if(scenario==1)return EFI_NOT_READY;*out=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=3,.DramBytes=16470685696ULL,.NormalBytes=4ULL<<30,.FullDdr=TRUE,.FixedReservations=TRUE,.DynamicReservations=TRUE,.RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};return EFI_SUCCESS;}
static EFI_STATUS ValidateMemory(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*p){CheckBs();assert(p->BootEpoch==3&&c==(VOID*)1);return EFI_SUCCESS;}
static EFI_STATUS Retire(VOID*c,CONST PIANO_LINUX_MEMORY_PROOF*p,PIANO_LINUX_RETIRE_PROOF*out){CheckBs();assert(pumps==1&&loads==1&&!starts&&initrd_installed&&sfs_sealed);++retire_calls;ui_alive=FALSE;*out=(PIANO_LINUX_RETIRE_PROOF){.Revision=1,.ExpectedOwners=31,.RetiredOwners=31,.Status=EFI_SUCCESS,.Clean=TRUE,.NoDma=TRUE,.AtApplication=TRUE};return EFI_SUCCESS;}
static EFI_STATUS ValidateRetired(VOID*c,CONST PIANO_LINUX_RETIRE_PROOF*p){CheckBs();assert(retire_calls==1&&!ui_alive&&p->RetiredOwners==31);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Start(EFI_HANDLE h,UINTN*n,CHAR16**d){CheckBs();assert(h==(VOID*)9&&retire_calls==1&&!ui_alive&&memory_calls==2&&bs_alive);++starts;UINTN before=sfs_calls;UINT8 out[4097];EFI_DEVICE_PATH_PROTOCOL end={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};UINTN bytes=0;assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,NULL)==EFI_BUFFER_TOO_SMALL&&bytes==4097);bytes=sizeof(out);assert(session.Load.LoadFile(&session.Load,&end,FALSE,&bytes,out)==EFI_SUCCESS&&!memcmp(out,initrd,sizeof(out))&&sfs_calls==before);
 // Real reader offset callback works after UI/storage retirement, with no SFS.
 UINT8 pe[16];assert(readers[0].Read(readers[0].Context,0,sizeof(pe),pe)==EFI_SUCCESS&&!memcmp(pe,kernel,sizeof(pe))&&sfs_calls==before);
 if(scenario==2||scenario==3){for(UINTN i=0;i<8;i++)if(events[i].live&&events[i].group==(scenario==2?&gEfiEventBeforeExitBootServicesGuid:&gEfiEventExitBootServicesGuid))events[i].fn(&events[i],events[i].ctx);if(scenario==3)bs_alive=FALSE;assert(mprotect(bs,4096,PROT_NONE)==0);return EFI_SUCCESS;}
 image_live=FALSE;assert(mprotect(loaded,4096,PROT_NONE)==0);*n=0;*d=NULL;return EFI_SUCCESS;}
static VOID p16(UINTN o,UINT16 v){memcpy(kernel+o,&v,2);}static VOID p32(UINTN o,UINT32 v){memcpy(kernel+o,&v,4);}static VOID p64(UINTN o,UINT64 v){memcpy(kernel+o,&v,8);}
int main(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);assert(scenario<5);p16(0,0x5a4d);p32(60,128);p32(128,0x4550);p16(132,0xaa64);p16(134,1);p16(148,240);p16(150,2);p16(152,0x20b);p32(168,4096);p64(176,0x10000000);p32(184,4096);p32(188,512);p32(208,8192);p32(212,512);p16(220,10);p32(260,16);memcpy(kernel+392,".text",5);p32(400,512);p32(404,4096);p32(408,512);p32(412,512);p32(428,0x60000020);
 assert(fdt_create_empty_tree(dtb,sizeof(dtb))==0);assert(fdt_add_subnode(dtb,0,"chosen")>=0);assert(fdt_pack(dtb)==0);for(UINTN i=0;i<sizeof(initrd);i++)initrd[i]=(UINT8)(i*11);
 contents[0]=kernel;contents[1]=dtb;contents[2]=initrd;sizes[0]=sizeof(kernel);sizes[1]=fdt_totalsize(dtb);sizes[2]=sizeof(initrd);for(UINTN i=0;i<3;i++)SHA256(contents[i],sizes[i],hashes[i]);
 bs=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);loaded=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bs!=MAP_FAILED&&loaded!=MAP_FAILED);
 *bs=(EFI_BOOT_SERVICES){.Hdr={.Signature=EFI_BOOT_SERVICES_SIGNATURE},.RaiseTPL=Raise,.RestoreTPL=Restore,.AllocatePool=Allocate,.FreePool=Free,.HandleProtocol=Handle,.CreateEventEx=Create,.CloseEvent=CloseEvent,.InstallConfigurationTable=Config,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall,.LocateDevicePath=LocatePath,.LoadImage=Load,.UnloadImage=Unload,.StartImage=Start};st.BootServices=bs;sfs=(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL){EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION,OpenVolume};for(UINTN i=0;i<3;i++){roots[i]=(EFI_FILE_PROTOCOL){.Revision=EFI_FILE_PROTOCOL_REVISION,.Open=Open,.Close=RootClose};files[i]=(EFI_FILE_PROTOCOL){.Revision=EFI_FILE_PROTOCOL_REVISION,.Close=FileClose,.Read=Read,.GetPosition=Pos,.SetPosition=Seek,.GetInfo=Info};}
 PIANO_BOOT_FILE_ENV source_env={.Context=(VOID*)1,.Services=bs,.BootServicesAlive=BsAlive};PIANO_BOOT_FILE_SPEC specs[3];for(UINTN i=0;i<3;i++)specs[i]=(PIANO_BOOT_FILE_SPEC){(VOID*)7,paths[i],8192,hashes[i]};assert(PianoBootFileLoadBundle(file_sources,3,&source_env,specs,24576,readers,blobs)==EFI_SUCCESS&&file_closes==3&&root_closes==3&&allocs==3);
 for(UINTN i=0;i<3;i++)assert(file_sources[i].Ready&&!file_sources[i].File&&!file_sources[i].Root&&!memcmp(file_sources[i].Sha256,hashes[i],32));sfs_sealed=TRUE;UINTN sealed_count=sfs_calls;
 PIANO_LINUX_EFI_ENV env={.Context=(VOID*)1,.Services=bs,.SystemTable=&st,.ParentImage=(VOID*)2,.MaxKernelBytes=8192,.MaxLoadedBytes=8192,.MaxDtbBytes=8192,.MaxInitrdBytes=8192,.ExpectedDramBytes=16470685696ULL,.ExpectedOwners=31,.CommandLine=L"rdinit=/init ro",.BootServicesAlive=scenario==4?BadUiAlive:BsAlive,.ServiceSlice=Pump,.CheckMemory=Memory,.ValidateMemory=ValidateMemory,.PrepareHandoff=Retire,.ValidateRetired=ValidateRetired,.FailStop=Fail};
 BOOLEAN fatal=FALSE;EFI_STATUS result=EFI_SUCCESS;if(!setjmp(halt)){result=PianoLinuxEfiSessionRun(&session,&env,&blobs[0],&blobs[1],&blobs[2]);}else fatal=TRUE;
 assert(sfs_calls==sealed_count);
 if(scenario==0){assert(!fatal&&result==EFI_ABORTED&&starts==1&&retire_calls==1&&!ui_alive&&bs_alive&&allocs==frees&&event_closes==5&&!initrd_installed);for(UINTN i=0;i<3;i++)assert(file_sources[i].Consumed&&!file_sources[i].Data&&!file_sources[i].Loan&&!file_sources[i].Owner&&!file_sources[i].Exit);}
 else if(scenario==1){assert(!fatal&&result==EFI_NOT_READY&&!starts&&!retire_calls&&ui_alive&&bs_alive&&allocs==frees&&event_closes==3);for(UINTN i=0;i<3;i++)assert(file_sources[i].Consumed);}
 else {assert(fatal&&session.Retained&&retire_calls==1&&!frees&&!event_closes);if(scenario==4)assert(!starts&&bs_alive&&!ui_alive);else assert(starts==1);for(UINTN i=0;i<3;i++)assert(!file_sources[i].Consumed&&file_sources[i].Data&&file_sources[i].Owner&&file_sources[i].Loan);}
 assert(mprotect(bs,4096,PROT_READ|PROT_WRITE)==0);for(UINTN i=0;i<allocs;i++)free(pools[i].ptr);munmap(bs,4096);munmap(loaded,4096);printf("Actual BootFileSource->LinuxEfiSession joint case%lu passed; SFS sealed after snapshot, no device/OS execution\n",(unsigned long)scenario);return 0;}
