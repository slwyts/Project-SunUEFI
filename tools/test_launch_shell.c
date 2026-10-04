// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host loader/result-state tests; do not claim to execute Shell commands here.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoLaunchShell.c"
EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiFirmwareVolume2ProtocolGuid,gEfiLoadedImageProtocolGuid;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE st;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL console;
static EFI_FIRMWARE_VOLUME2_PROTOCOL fv;
static EFI_LOADED_IMAGE_PROTOCOL loaded;
static EFI_DEVICE_PATH_PROTOCOL base_path[]={{4,7,{4,0}},{0x7f,0xff,{4,0}}};
static unsigned allocations,loads,starts,unloads,output_calls,report_lines;
static unsigned fail_load_call,fail_start_call;
static BOOLEAN no_volumes,null_loaded,report_during_start;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){++allocations;return malloc(N);}
VOID EFIAPI FreePool(VOID *P){assert(P && allocations);--allocations;free(P);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return TRUE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  if(strncmp(Format,"SUNUEFI_SHELL_REPORT command=",28)==0)++report_lines;
}
UINTN EFIAPI StrSize(CONST CHAR16 *S){UINTN N=0;while(S[N])++N;return (N+1)*sizeof(CHAR16);}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(EFI_HANDLE H){assert(H==(void *)2);return base_path;}
UINT16 EFIAPI SetDevicePathNodeLength(VOID *Node,UINTN N){EFI_DEVICE_PATH_PROTOCOL *P=Node;P->Length[0]=(UINT8)N;P->Length[1]=(UINT8)(N>>8);return (UINT16)N;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(CONST EFI_DEVICE_PATH_PROTOCOL *Base,CONST EFI_DEVICE_PATH_PROTOCOL *Node){
  assert(Base==base_path && Node->Type==MEDIA_DEVICE_PATH && Node->SubType==MEDIA_PIWG_FW_FILE_DP);
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *F=(void *)Node;assert(!memcmp(&F->FvFileName,&mShellFile,sizeof(EFI_GUID)));
  UINTN N=sizeof(*F);UINT8 *P=AllocatePool(4+N+4);memcpy(P,Base,4);memcpy(P+4,Node,N);memcpy(P+4+N,Base+1,4);return (void *)P;
}
static EFI_STATUS EFIAPI volumes(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles){
  assert(Type==ByProtocol && Guid==&gEfiFirmwareVolume2ProtocolGuid);if(no_volumes)return EFI_NOT_FOUND;
  *Count=1;*Handles=AllocatePool(sizeof(EFI_HANDLE));(*Handles)[0]=(void *)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocol(EFI_HANDLE H,EFI_GUID *Guid,VOID **Interface){
  if(Guid==&gEfiFirmwareVolume2ProtocolGuid){assert(H==(void *)2);*Interface=&fv;return EFI_SUCCESS;}
  assert(H==(void *)3 && Guid==&gEfiLoadedImageProtocolGuid);*Interface=null_loaded?NULL:&loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI section(CONST EFI_FIRMWARE_VOLUME2_PROTOCOL *This,CONST EFI_GUID *Guid,EFI_SECTION_TYPE Type,UINTN Instance,VOID **Source,UINTN *Bytes,UINT32 *Auth){
  assert(This==&fv && !memcmp(Guid,&mShellFile,sizeof(EFI_GUID)) && Type==EFI_SECTION_PE32 && Instance==0);
  *Source=AllocatePool(64);*Bytes=64;*Auth=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Image){
  assert(!Policy && Parent==(void *)1 && Path && Source && Bytes==64);++loads;*Image=(void *)3;
  return loads==fail_load_call?EFI_LOAD_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *String){assert(This==&console && String);++output_calls;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI start(EFI_HANDLE Image,UINTN *Size,CHAR16 **Data){
  assert(Image==(void *)3 && loaded.LoadOptions && loaded.LoadOptionsSize==StrSize(loaded.LoadOptions));++starts;
  if(report_during_start){
    assert(mShellReports[starts-1].StartCalled && !mShellReports[starts-1].StartReturned);
    PianoReportShellDiagnostics();
  }
  console.OutputString(&console,L"mock image output\r\n");
  return starts==fail_start_call?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unload(EFI_HANDLE Image){assert(Image==(void *)3);++unloads;return EFI_INVALID_PARAMETER;}
static VOID initialize(VOID){
  assert(!allocations);loads=starts=unloads=output_calls=report_lines=0;fail_load_call=fail_start_call=0;
  no_volumes=null_loaded=report_during_start=FALSE;console.OutputString=output;loaded.ImageCodeType=EfiLoaderCode;
  mShellReportsInitialized=FALSE;mOriginalOutput=NULL;
}
int main(void){
  gBS=&bs;gST=&st;st.ConOut=&console;bs.LocateHandleBuffer=volumes;bs.HandleProtocol=protocol;
  bs.LoadImage=load;bs.StartImage=start;bs.UnloadImage=unload;fv.ReadSection=section;
  initialize();PianoReportShellDiagnostics();assert(!report_lines && !loads);
  no_volumes=TRUE;assert(PianoRunShellDiagnostics((void *)1,FALSE)==EFI_NOT_FOUND && report_lines==3);
  for(UINTN I=0;I<3;++I)assert(mShellReports[I].Requested && mShellReports[I].Load==EFI_NOT_FOUND && !mShellReports[I].StartCalled && mShellReports[I].Start==EFI_NOT_STARTED && !mShellReports[I].UnloadCalled);
  initialize();fail_load_call=2;assert(PianoRunShellDiagnostics((void *)1,FALSE)==EFI_LOAD_ERROR && loads==3 && starts==2 && unloads==2 && !allocations);
  assert(mShellReports[1].Load==EFI_LOAD_ERROR && !mShellReports[1].StartCalled && mShellReports[2].StartReturned);
  initialize();null_loaded=TRUE;assert(PianoRunShellDiagnostics((void *)1,FALSE)==EFI_DEVICE_ERROR && !starts && unloads==3 && !allocations);
  initialize();fail_start_call=2;assert(PianoRunShellDiagnostics((void *)1,FALSE)==EFI_DEVICE_ERROR && starts==3 && !allocations);
  assert(mShellReports[1].Start==EFI_DEVICE_ERROR && mShellReports[2].Start==EFI_SUCCESS);
  initialize();report_during_start=TRUE;assert(PianoRunShellDiagnostics((void *)1,FALSE)==EFI_SUCCESS && starts==3 && unloads==3 && output_calls==3 && !allocations);
  assert(console.OutputString==output && mOriginalOutput==NULL);
  for(UINTN I=0;I<3;++I)assert(mShellReports[I].StartReturned && mShellReports[I].UnloadCalled && mShellReports[I].Unload==EFI_INVALID_PARAMETER && mShellReports[I].Application);
  unsigned old=report_lines;PianoReportShellDiagnostics();assert(report_lines==old+3);
  puts("Shell final RAM state: honest not-started/in-progress, independent Load/Start/Unload results, partial failures, final re-report and console restoration passed.");
}
