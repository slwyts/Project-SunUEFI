// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual parser/coordinator/EFI API signatures, CPU-only PE API mocks.
#define main parser_source_suite
#include "test_fastboot_boot.c"
#undef main
#include <setjmp.h>
#include <sys/mman.h>
#include "../../uefi/core/PianoFastbootLaunch.c"
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid={.Data1=2},gEfiEventBeforeExitBootServicesGuid={.Data1=3};
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
static PIANO_FASTBOOT_LAUNCH state;
static EFI_BOOT_SERVICES *bs;
static EFI_LOADED_IMAGE_PROTOCOL *loaded;
static BOOLEAN alive,owner,loan,image_live,auto_unload,returns_after_exit,notify_exit,failstop_returns,reenter;
static UINTN take_calls,borrow_calls,unborrow_calls,restore_calls,zero_calls,shutdown_calls,load_calls,start_calls,unload_calls,close_calls,alloc_calls,free_calls,failstop_calls,dead_calls,handle_calls;
static EFI_STATUS take_status,borrow_status,shutdown_status,load_status,start_status,unload_status,close_status,restore_status,zero_status,unborrow_status,allocate_status;
static EFI_STATUS free_status;
static BOOLEAN null_owner,null_view,load_error_with_handle,fail_handle;
static BOOLEAN recycled_image;
static EFI_EVENT_NOTIFY notifier;static VOID *notify_context;
static VOID *options_alloc,*exit_alloc;static jmp_buf stop_return;
static PIANO_LAUNCH_ENV env;static PIANO_LAUNCH_BLOB blob;
static EFI_STATUS take(VOID *Context,VOID **Token) {
  assert(Context==(VOID *)123 && !owner);++take_calls;
  if(reenter){reenter=FALSE;assert(PianoFastbootLaunchRun(&state,&env,&blob,NULL,0)==EFI_ALREADY_STARTED);}
  if(take_status!=EFI_SUCCESS)return take_status;
  owner=TRUE;*Token=null_owner?NULL:(VOID *)456;return EFI_SUCCESS;
}
static EFI_STATUS read_blob(VOID *Context,VOID *Token,UINT64 Offset,UINTN Bytes,VOID *Buffer){assert(Context==(VOID *)123 && Token==(VOID *)456 && owner);return reader(file,Offset,Bytes,Buffer);}
static EFI_STATUS borrow_view(VOID *Context,VOID *Token,PIANO_BOOT_RANGE R,CONST VOID **View,VOID **Loan) {
  assert(Context==(VOID *)123 && Token==(VOID *)456 && owner && !loan && Range(R.Offset,R.Bytes,length));++borrow_calls;
  if(borrow_status!=EFI_SUCCESS)return borrow_status;
  loan=TRUE;*View=null_view?NULL:file+R.Offset;*Loan=(VOID *)789;return EFI_SUCCESS;
}
static EFI_STATUS unborrow(VOID *Context,VOID *Token,VOID *Loan) {
  assert(Context==(VOID *)123 && Token==(VOID *)456 && Loan==(VOID *)789 && owner && loan && !image_live);++unborrow_calls;
  if(unborrow_status!=EFI_SUCCESS)return unborrow_status;
  loan=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS restore(VOID *Context,VOID *Token){assert(Context==(VOID *)123 && Token==(VOID *)456 && owner && !loan && !image_live);++restore_calls;if(restore_status!=EFI_SUCCESS)return restore_status;owner=FALSE;return EFI_SUCCESS;}
static EFI_STATUS zero_release(VOID *Context,VOID *Token){assert(Context==(VOID *)123 && Token==(VOID *)456 && owner && !loan && !image_live);++zero_calls;if(zero_status!=EFI_SUCCESS)return zero_status;memset(file,0,sizeof(file));owner=FALSE;return EFI_SUCCESS;}
static EFI_STATUS shutdown(VOID *Context){assert(Context==(VOID *)321 && owner && loan && !load_calls);++shutdown_calls;return shutdown_status;}
static BOOLEAN services_alive(VOID *Context){assert(Context==(VOID *)321);return alive;}
static VOID fail_stop(VOID *Context,EFI_STATUS Status){assert(Context==(VOID *)321 && Status==EFI_ABORTED && owner && loan);++failstop_calls;if(!failstop_returns)longjmp(stop_return,1);}
VOID EFIAPI CpuDeadLoop(VOID){++dead_calls;longjmp(stop_return,1);}
static EFI_STATUS EFIAPI create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event) {
  assert(alive && Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Group==&gEfiEventExitBootServicesGuid && shutdown_calls==1);
  notifier=Notify;notify_context=(VOID *)Context;*Event=(VOID *)999;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event){assert(alive && Event==(VOID *)999 && !image_live);++close_calls;if(close_status!=EFI_SUCCESS)return close_status;notifier=NULL;notify_context=NULL;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Image) {
  assert(alive && !Policy && Parent==(VOID *)555 && !Path && owner && loan && shutdown_status==EFI_SUCCESS && shutdown_calls==1 && notifier);
  assert(Source==state.View && Bytes==state.Parsed.Kernel.Bytes && ((UINT8 *)Source)[0]=='M' && ((UINT8 *)Source)[1]=='Z');++load_calls;
  if(load_status!=EFI_SUCCESS && !load_error_with_handle)return load_status;
  assert(mprotect(loaded,4096,PROT_READ|PROT_WRITE)==0);memset(loaded,0,sizeof(*loaded));loaded->Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION;
  loaded->LoadOptions=(VOID *)111;loaded->LoadOptionsSize=7;loaded->ImageBase=(VOID *)0x10000000;loaded->ImageSize=8192;*Image=(VOID *)777;image_live=TRUE;return load_status;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Interface){assert(alive && Image==(VOID *)777 && Guid==&gEfiLoadedImageProtocolGuid);++handle_calls;
  *Interface=NULL;if(!image_live)return EFI_INVALID_PARAMETER;if(fail_handle)return EFI_DEVICE_ERROR;*Interface=loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI alloc_pool(EFI_MEMORY_TYPE Type,UINTN Bytes,VOID **Buffer){assert(alive && Type==EfiLoaderData && Bytes<=4096);++alloc_calls;if(allocate_status!=EFI_SUCCESS)return allocate_status;options_alloc=malloc(Bytes);assert(options_alloc);*Buffer=options_alloc;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI free_pool(VOID *Buffer){assert(alive && !image_live);++free_calls;
  if(free_status!=EFI_SUCCESS)return free_status;
  if(Buffer==options_alloc){for(UINTN I=0;I<state.OptionsBytes;++I)assert(!((UINT8 *)Buffer)[I]);free(Buffer);options_alloc=NULL;}
  else {assert(Buffer==exit_alloc);free(Buffer);exit_alloc=NULL;}return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI start(EFI_HANDLE Image,UINTN *ExitBytes,CHAR16 **ExitData) {
  assert(alive && Image==(VOID *)777 && image_live && owner && loan && shutdown_calls==1);++start_calls;
  if(state.OptionsBytes){assert(loaded->LoadOptions==options_alloc && loaded->LoadOptionsSize==state.OptionsBytes);}
  if(returns_after_exit) {
    if(notify_exit)notifier((VOID *)999,notify_context);
    alive=FALSE;assert(mprotect(bs,4096,PROT_NONE)==0);return EFI_SUCCESS;
  }
  exit_alloc=malloc(8);assert(exit_alloc);*ExitBytes=8;*ExitData=exit_alloc;
  if(recycled_image)loaded->ImageBase=(VOID *)0x20000000;
  if(auto_unload){image_live=FALSE;assert(mprotect(loaded,4096,PROT_NONE)==0);}return start_status;
}
static EFI_STATUS EFIAPI unload(EFI_HANDLE Image){assert(alive && Image==(VOID *)777 && image_live && owner && loan);++unload_calls;
  if(unload_status!=EFI_SUCCESS)return unload_status;
  if(state.OptionsInstalled)assert(FALSE);
  image_live=FALSE;assert(mprotect(loaded,4096,PROT_NONE)==0);return EFI_SUCCESS;
}
static VOID reset_model(BOOLEAN Wrapped) {
  if(bs)assert(mprotect(bs,4096,PROT_READ|PROT_WRITE)==0);
  if(loaded)assert(mprotect(loaded,4096,PROT_READ|PROT_WRITE)==0);
  free(options_alloc);free(exit_alloc);options_alloc=exit_alloc=NULL;
  memset(&state,0,sizeof(state));assert(PianoFastbootLaunchInit(&state)==EFI_SUCCESS);
  if(!bs){bs=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);loaded=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bs!=MAP_FAILED && loaded!=MAP_FAILED);}
  memset(bs,0,sizeof(*bs));bs->CreateEventEx=create;bs->CloseEvent=close_event;bs->LoadImage=load;bs->HandleProtocol=handle;bs->StartImage=start;bs->UnloadImage=unload;bs->AllocatePool=alloc_pool;bs->FreePool=free_pool;
  alive=auto_unload=TRUE;owner=loan=image_live=returns_after_exit=notify_exit=failstop_returns=reenter=null_owner=null_view=load_error_with_handle=fail_handle=recycled_image=FALSE;
  take_calls=borrow_calls=unborrow_calls=restore_calls=zero_calls=shutdown_calls=load_calls=start_calls=unload_calls=close_calls=alloc_calls=free_calls=failstop_calls=dead_calls=handle_calls=0;
  take_status=borrow_status=shutdown_status=load_status=start_status=unload_status=close_status=restore_status=zero_status=unborrow_status=allocate_status=EFI_SUCCESS;
  free_status=EFI_SUCCESS;
  notifier=NULL;notify_context=NULL;read_error=EFI_SUCCESS;
  memset(file,0,sizeof(file));length=1024;pe(0);
  if(Wrapped){memset(file,0,sizeof(file));memcpy(file,"ANDROID!",8);p32(8,1024);p32(36,2048);length=4096;pe(2048);}
  blob=(PIANO_LAUNCH_BLOB){(VOID *)123,length,take,read_blob,borrow_view,unborrow,restore,zero_release};
  env=(PIANO_LAUNCH_ENV){.Context=(VOID *)321,.Services=bs,.ParentImage=(VOID *)555,.MaxImageBytes=64*1024*1024,.MaxSourceBytes=64*1024*1024,.ShutdownAll=shutdown,.BootServicesAlive=services_alive,.FailStop=fail_stop,.RestoreOnFailure=TRUE};
}
static EFI_STATUS run_options(VOID){STATIC CONST UINT8 Options[]={1,2,3,4,5,6};return PianoFastbootLaunchRun(&state,&env,&blob,Options,sizeof(Options));}
int main(void) {
  for(UINTN Wrapped=0;Wrapped<2;++Wrapped) {
    reset_model(Wrapped);reenter=TRUE;assert(run_options()==EFI_SUCCESS && state.Result.ShutdownSucceeded && state.Result.AppReturned && state.Result.ImageUnloaded && state.Result.BlobZeroReleased && !owner && !loan);
    assert(handle_calls==2 && !unload_calls && !state.Busy && !state.Result.ResourcesRetained && !options_alloc && !exit_alloc); // Auto-unload old protocol page was PROT_NONE.
  }
  reset_model(FALSE);length=0x40000000;blob.Bytes=length;env.MaxSourceBytes=length;assert(PianoFastbootLaunchRun(&state,&env,&blob,NULL,0)==EFI_SUCCESS && !alloc_calls && max_read<=1660); // logical1GiB, no full-copy allocation.
  reset_model(FALSE);blob.Bytes=0x40000000;assert(run_options()==EFI_OUT_OF_RESOURCES && !take_calls && !shutdown_calls);
  reset_model(TRUE);p16(2048,0);assert(run_options()==EFI_UNSUPPORTED && restore_calls==1 && !shutdown_calls && !load_calls);
  reset_model(TRUE);p32(16,100);length=blob.Bytes=6144;assert(run_options()==EFI_UNSUPPORTED && !shutdown_calls && restore_calls==1);
  reset_model(FALSE);env.MaxImageBytes=4096;assert(run_options()==EFI_OUT_OF_RESOURCES && !shutdown_calls && restore_calls==1);
  reset_model(FALSE);shutdown_status=EFI_DEVICE_ERROR;assert(run_options()==EFI_DEVICE_ERROR && !load_calls && restore_calls==1 && unborrow_calls==1);
  reset_model(FALSE);shutdown_status=EFI_WARN_UNKNOWN_GLYPH;assert(run_options()==EFI_DEVICE_ERROR && !load_calls);
  reset_model(FALSE);load_status=EFI_SECURITY_VIOLATION;load_error_with_handle=TRUE;assert(run_options()==EFI_SECURITY_VIOLATION && unload_calls==1 && !start_calls && restore_calls==1);
  reset_model(FALSE);allocate_status=EFI_OUT_OF_RESOURCES;assert(run_options()==EFI_OUT_OF_RESOURCES && unload_calls==1 && restore_calls==1);
  reset_model(FALSE);auto_unload=FALSE;assert(run_options()==EFI_SUCCESS && unload_calls==1 && state.Result.OptionsRestored && zero_calls==1);
  reset_model(FALSE);auto_unload=FALSE;start_status=EFI_SECURITY_VIOLATION;assert(run_options()==EFI_SECURITY_VIOLATION && unload_calls==1 && restore_calls==1 && state.Result.OptionsRestored);
  reset_model(FALSE);auto_unload=FALSE;recycled_image=TRUE;assert(run_options()==EFI_COMPROMISED_DATA && !unload_calls && state.Result.ResourcesRetained && owner && loan && options_alloc);
  for(UINTN Failure=0;Failure<6;++Failure) {
    reset_model(FALSE);
    if(Failure==0){auto_unload=FALSE;unload_status=EFI_DEVICE_ERROR;}
    if(Failure==1)close_status=EFI_DEVICE_ERROR;
    if(Failure==2)unborrow_status=EFI_DEVICE_ERROR;
    if(Failure==3){shutdown_status=EFI_DEVICE_ERROR;restore_status=EFI_DEVICE_ERROR;}
    if(Failure==4)zero_status=EFI_DEVICE_ERROR;
    if(Failure==5)free_status=EFI_DEVICE_ERROR;
    assert(run_options()!=EFI_SUCCESS && state.Result.ResourcesRetained && state.Busy && owner && !state.Result.BlobRestored && !state.Result.BlobZeroReleased);
    assert(PianoFastbootLaunchInit(&state)==EFI_ACCESS_DENIED && PianoFastbootLaunchRun(&state,&env,&blob,NULL,0)==EFI_ALREADY_STARTED);
  }
  reset_model(FALSE);null_owner=TRUE;assert(run_options()==EFI_COMPROMISED_DATA && state.Result.ResourcesRetained && !restore_calls && !zero_calls);
  reset_model(FALSE);null_view=TRUE;assert(run_options()==EFI_COMPROMISED_DATA && state.Result.ResourcesRetained && !shutdown_calls);
  reset_model(FALSE);env.RestoreOnFailure=FALSE;load_status=EFI_LOAD_ERROR;assert(run_options()==EFI_LOAD_ERROR && zero_calls==1 && !restore_calls);
  for(UINTN Mode=0;Mode<3;++Mode) {
    reset_model(FALSE);returns_after_exit=TRUE;notify_exit=Mode!=1;failstop_returns=Mode==2;
    if(!setjmp(stop_return)){run_options();assert(FALSE);}
    assert(failstop_calls==1 && dead_calls==(Mode==2) && owner && loan && !close_calls && !unborrow_calls && !restore_calls && !zero_calls && state.Result.ResourcesRetained);
    assert(state.Result.ExitSignalSeen==(Mode!=1)); // Boot Services page was PROT_NONE after Start returned.
  }
  reset_model(FALSE);alive=FALSE;assert(mprotect(bs,4096,PROT_NONE)==0);assert(run_options()==EFI_NOT_READY && !take_calls);
  reset_model(FALSE);assert(PianoFastbootLaunchRun(&state,&env,&blob,NULL,4097)==EFI_INVALID_PARAMETER);
  reset_model(FALSE);free(options_alloc);free(exit_alloc);assert(munmap(bs,4096)==0 && munmap(loaded,4096)==0);bs=NULL;loaded=NULL;
  puts("Actual RAM launch core: raw/wrapped AA64 PE, exclusive blob views, typed shutdown-before-Load/Start, fresh auto-unload lifecycle, options/exitdata cleanup, restore-vs-zero, retained failures, logical1GiB no-copy, reentry and PROT_NONE post-EBS failstop passed.");return 0;
}
