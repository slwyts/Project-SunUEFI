// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductOsController.h"
#include "PianoUfsProductVolume.h"
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#define OS_SIG SIGNATURE_32('P','O','S','1')
STATIC EFI_GUID mRuntimeGuid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
STATIC EFI_GUID mVolumeGuid=PIANO_PRODUCT_STORAGE_TYPE_GUID;
STATIC BOOLEAN Live(PIANO_PRODUCT_OS_CONTROLLER *S){
  if(S->Report.ServicesLost||!S->Env.BootServicesAlive||S->Env.BootServicesAlive(S->Env.Context)!=TRUE){S->Report.ServicesLost=TRUE;return FALSE;}return TRUE;
}
STATIC BOOLEAN FileLive(VOID *Context){return Live((PIANO_PRODUCT_OS_CONTROLLER *)Context);}
STATIC EFI_STATUS Exact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;((PIANO_PRODUCT_OS_CONTROLLER *)Context)->Report.ServicesLost=TRUE;}
STATIC BOOLEAN Alias(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){UINTN X=(UINTN)A,Y=(UINTN)B;return An>MAX_UINTN-X||Bn>MAX_UINTN-Y||(X<Y+Bn&&Y<X+An);}
STATIC BOOLEAN CleanCpu(PIANO_PRODUCT_OS_CONTROLLER *S){
  if(S->Report.Retained||S->Report.ServicesLost||S->Session.Retained||S->Session.BeforeEbs||S->Session.Ebs||S->Session.LateArmed||S->Session.OwnersRetired||
     S->Session.Image||S->Session.InitrdHandle||S->Session.BeforeEvent||S->Session.ExitEvent||S->Session.FdtCopy||S->Session.OptionsCopy||S->Session.ExitData||
     S->Session.FdtInstalled||S->Session.InitrdInstalled||S->Session.OptionsInstalled)return FALSE;
  for(UINTN I=0;I<3;++I){PIANO_BOOT_FILE_SOURCE *F=&S->Files[I];if(F->Retained||F->ServicesLost||F->Busy||F->Taken||F->Owner||F->Loan||F->Data||F->Root||F->File||F->Exit||
    S->Session.Owners[I]||S->Session.Loans[I]||S->Session.Views[I])return FALSE;}return TRUE;
}
STATIC BOOLEAN PinValid(CONST PIANO_PRODUCT_OS_PIN *P){
  UINT8 A=0,B=0,C=0;for(UINTN I=0;I<32;++I){A|=P->ImageSha256[I];B|=P->DtbSha256[I];C|=P->InitrdSha256[I];}
  return A&&B&&C&&P->ImageBytes&&P->DtbBytes&&P->InitrdBytes&&P->ImageBytes<=PIANO_CPU_INPUT_MAX_BYTES&&
    P->DtbBytes<=PIANO_CPU_INPUT_MAX_BYTES-P->ImageBytes&&P->InitrdBytes<=PIANO_CPU_INPUT_MAX_BYTES-P->ImageBytes-P->DtbBytes;
}
STATIC EFI_STATUS CheckContext(PIANO_PRODUCT_OS_CONTROLLER *S,PIANO_PRODUCT_OS_FAMILY Family,UINT64 Sequence){
  if(!Live(S))return EFI_ABORTED;
  EFI_TPL T=S->Env.Services->RaiseTPL(TPL_HIGH_LEVEL);S->Env.Services->RestoreTPL(T);
  if(!Live(S))return EFI_ABORTED;if(T!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *R=NULL;EFI_STATUS E=S->Env.Services->LocateProtocol(&mRuntimeGuid,NULL,(VOID **)&R);
  if(!Live(S))return EFI_ABORTED;
  if(E!=EFI_SUCCESS||R!=S->Env.Runtime||!R||R->Revision!=PIANO_PRODUCT_RUNTIME_REVISION||!R->BootServicesAlive||R->BootServicesAlive(R)!=TRUE)return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
  PIANO_PRODUCT_OWNERS *O=S->Env.Owners;
  if(!O||!O->Report.Initialized||O->Report.Phase!=PianoProductOwnersReturnRequested||O->Report.Retained||O->Report.Clean||O->Report.Busy||O->Report.ServicesLost||
     O->Config.Runtime!=R||O->Config.ExpectedOwnerMask!=PIANO_OWNER_ALL_MASK)return EFI_NOT_READY;
  E=Exact(S->Env.ValidateSelection(S->Env.Context,Family,Sequence));return Live(S)?E:EFI_ABORTED;
}
STATIC EFI_STATUS FileSlice(VOID *Context,UINTN Budget){
  PIANO_PRODUCT_OS_CONTROLLER *S=Context;if(!S->FileIoActive)return Exact(S->Env.Cpu.ServiceSlice(S->Env.Cpu.Context,Budget));
  EFI_STATUS E=CheckContext(S,S->Report.Family,S->Report.Sequence);if(E!=EFI_SUCCESS)return E;
  EFI_BLOCK_IO_PROTOCOL *B=NULL;UINT8 Uuid[16]={0};E=Exact(S->Env.ApprovedVolume(S->Env.Context,&B,Uuid));if(!Live(S))return EFI_ABORTED;
  if(E!=EFI_SUCCESS||B!=S->Block||CompareMem(Uuid,S->VolumeUuid,16))return E==EFI_SUCCESS?EFI_MEDIA_CHANGED:E;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F=NULL;E=S->Env.Services->HandleProtocol(S->FileSystem,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&F);
  if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS||F!=S->Sfs)return E==EFI_SUCCESS?EFI_MEDIA_CHANGED:Exact(E);
  return Exact(S->Env.Cpu.ServiceSlice(S->Env.Cpu.Context,Budget));
}
STATIC EFI_STATUS FileMemory(VOID *Context,PIANO_LINUX_MEMORY_PROOF *Report){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Cpu.CheckMemory?S->Env.Cpu.CheckMemory(S->Env.Cpu.Context,Report):EFI_NOT_READY;}
STATIC EFI_STATUS FileValidate(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Report){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Cpu.ValidateMemory?S->Env.Cpu.ValidateMemory(S->Env.Cpu.Context,Report):EFI_NOT_READY;}
STATIC EFI_STATUS FileBuffer(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Report,CONST VOID *Producer,VOID *Owner,CONST VOID *Base,UINT64 Bytes){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Cpu.ValidateBuffer?S->Env.Cpu.ValidateBuffer(S->Env.Cpu.Context,Report,Producer,Owner,Base,Bytes):EFI_NOT_READY;}
STATIC EFI_STATUS LateArm(VOID *Context,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *Identity){
  PIANO_PRODUCT_OS_CONTROLLER *S=Context;EFI_STATUS E=CheckContext(S,S->Report.Family,S->Report.Sequence);if(E!=EFI_SUCCESS)return E;
  E=Exact(S->Env.Linux.NativeLateArm(S->Env.Linux.Context,Image,Identity));return Live(S)?E:EFI_ABORTED;
}
STATIC EFI_STATUS LateDisarm(VOID *Context,EFI_HANDLE Image){
  PIANO_PRODUCT_OS_CONTROLLER *S=Context;EFI_STATUS E=Exact(S->Env.Linux.NativeLateDisarm(S->Env.Linux.Context,Image));return Live(S)?E:EFI_ABORTED;
}
STATIC EFI_STATUS LinuxSlice(VOID *Context,UINTN Budget){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Linux.ServiceSlice(S->Env.Linux.Context,Budget);}
STATIC EFI_STATUS LinuxMemory(VOID *Context,CONST VOID *Dtb,UINTN Bytes,PIANO_LINUX_MEMORY_PROOF *Report){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Linux.CheckMemory(S->Env.Linux.Context,Dtb,Bytes,Report);}
STATIC EFI_STATUS LinuxValidate(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Report){PIANO_PRODUCT_OS_CONTROLLER *S=Context;return S->Env.Linux.ValidateMemory(S->Env.Linux.Context,Report);}
STATIC VOID LinuxFail(VOID *Context,EFI_STATUS Status){PIANO_PRODUCT_OS_CONTROLLER *S=Context;S->Env.Linux.FailStop(S->Env.Linux.Context,Status);}
STATIC EFI_STATUS Volume(PIANO_PRODUCT_OS_CONTROLLER *S){
  EFI_BLOCK_IO_PROTOCOL *Approved=NULL;UINT8 Uuid[16]={0};EFI_STATUS E=Exact(S->Env.ApprovedVolume(S->Env.Context,&Approved,Uuid));
  if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return E;
  UINT8 Any=0;for(UINTN I=0;I<16;++I)Any|=Uuid[I];if(!Approved||!Approved->Media||!Approved->Media->MediaPresent||!Any)return EFI_NOT_READY;
  EFI_HANDLE *Handles=NULL;UINTN Count=0;E=S->Env.Services->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if(!Live(S))return EFI_ABORTED;
  if(E==EFI_NOT_FOUND&&!Count&&!Handles)return EFI_NOT_FOUND;
  if(E!=EFI_SUCCESS||!Handles||!Count||Count>128){S->Report.Retained=Handles!=NULL;return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);}
  EFI_HANDLE Chosen=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs=NULL;
  for(UINTN I=0;I<Count;++I){
    EFI_BLOCK_IO_PROTOCOL *Block=NULL;EFI_DEVICE_PATH_PROTOCOL *Path=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F=NULL;
    E=S->Env.Services->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block);if(!Live(S))return EFI_ABORTED;
    if(E!=EFI_SUCCESS||Block!=Approved)continue;
    E=S->Env.Services->HandleProtocol(Handles[I],&gEfiDevicePathProtocolGuid,(VOID **)&Path);if(!Live(S))return EFI_ABORTED;
    if(E!=EFI_SUCCESS||!Path){E=EFI_COMPROMISED_DATA;goto Done;}
    UINT8 *P=(UINT8 *)Path;
    if(P[0]!=HARDWARE_DEVICE_PATH||P[1]!=HW_VENDOR_DP||P[2]!=36||P[3]||CompareMem(P+4,&mVolumeGuid,16)||CompareMem(P+20,Uuid,16)||
       P[36]!=END_DEVICE_PATH_TYPE||P[37]!=END_ENTIRE_DEVICE_PATH_SUBTYPE||P[38]!=4||P[39]){E=EFI_ACCESS_DENIED;goto Done;}
    E=S->Env.Services->HandleProtocol(Handles[I],&gEfiSimpleFileSystemProtocolGuid,(VOID **)&F);if(!Live(S))return EFI_ABORTED;
    if(E!=EFI_SUCCESS||!F||!F->OpenVolume||Chosen){E=EFI_COMPROMISED_DATA;goto Done;}
    Chosen=Handles[I];Sfs=F;S->Path=Path;
  }
  E=Chosen?EFI_SUCCESS:EFI_NOT_FOUND;
Done:
  {EFI_STATUS Free=S->Env.Services->FreePool(Handles);if(!Live(S))return EFI_ABORTED;if(Free!=EFI_SUCCESS){S->Report.Retained=TRUE;return Exact(Free);}}
  if(E!=EFI_SUCCESS)return E;
  S->FileSystem=Chosen;S->Sfs=Sfs;S->Block=Approved;CopyMem(S->VolumeUuid,Uuid,16);return EFI_SUCCESS;
}
EFI_STATUS PianoProductOsInitialize(PIANO_PRODUCT_OS_CONTROLLER *S,CONST PIANO_PRODUCT_OS_ENV *E){
  if(!S||!E||S->Signature||E->Revision!=PIANO_PRODUCT_OS_REVISION||Alias(S,sizeof(*S),E,sizeof(*E))||
     (E->Context&&Alias(E->Context,1,S,sizeof(*S)))||(E->Cpu.Context&&Alias(E->Cpu.Context,1,S,sizeof(*S)))||
     (E->Linux.Context&&Alias(E->Linux.Context,1,S,sizeof(*S))))return EFI_INVALID_PARAMETER;
  if(!E->Services||!E->SystemTable||E->SystemTable->BootServices!=E->Services||!E->ParentImage||!E->Runtime||!E->Owners||!E->BootServicesAlive||!E->ApprovedVolume||!E->ValidateSelection||
     !E->Services->RaiseTPL||!E->Services->RestoreTPL||!E->Services->LocateProtocol||!E->Services->LocateHandleBuffer||!E->Services->HandleProtocol||!E->Services->FreePool||
     !E->Services->CreateEventEx||!E->Services->CloseEvent||E->Linux.HandoffMode!=PianoHandoffNativeLate||!E->Linux.NativeLateArm||!E->Linux.NativeLateDisarm||!E->Cpu.BootServicesAlive||!E->Cpu.ServiceSlice||
     E->Linux.Services!=E->Services||E->Linux.SystemTable!=E->SystemTable||E->Linux.ParentImage!=E->ParentImage||!E->Linux.BootServicesAlive||!E->Linux.ServiceSlice||!E->Linux.CheckMemory||!E->Linux.ValidateMemory||!E->Linux.FailStop||
     !PinValid(&E->Stable)||!PinValid(&E->Next))return EFI_NOT_READY;
  S->Signature=OS_SIG;S->Env=*E;S->Cpu=E->Cpu;S->FileCpu=(PIANO_CPU_INPUT_ENV){S,FileLive,FileSlice,FileMemory,FileValidate,FileBuffer};S->Linux=E->Linux;S->Linux.Context=S;S->Linux.BootServicesAlive=FileLive;S->Linux.ServiceSlice=LinuxSlice;
  S->Linux.CheckMemory=LinuxMemory;S->Linux.ValidateMemory=LinuxValidate;S->Linux.FailStop=LinuxFail;
  S->Linux.NativeLateArm=LateArm;S->Linux.NativeLateDisarm=LateDisarm;S->Linux.Cpu=&S->Cpu;S->Report.Revision=1;
  if(!Live(S)){S->Report.Retained=TRUE;return EFI_NOT_READY;}
  EFI_STATUS Status=E->Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,S,&gEfiEventExitBootServicesGuid,&S->Exit);
  if(Status!=EFI_SUCCESS||!S->Exit||!Live(S)){S->Report.Retained=TRUE;return S->Report.Status=Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(Status);}
  S->Signature=OS_SIG;return S->Report.Status=EFI_SUCCESS;
}
EFI_STATUS PianoProductOsRun(PIANO_PRODUCT_OS_CONTROLLER *S,PIANO_PRODUCT_OS_FAMILY Family,UINT64 Sequence){
  if(!S||S->Signature!=OS_SIG||Family<PianoProductOsStable||Family>PianoProductOsNext||!Sequence)return EFI_INVALID_PARAMETER;
  if(S->Report.Busy||S->Report.Retained||!S->Exit)return EFI_ACCESS_DENIED;
  if(S->Session.Signature||S->Files[0].Signature||S->Files[1].Signature||S->Files[2].Signature){
    if(!CleanCpu(S)){S->Report.Retained=TRUE;return EFI_ACCESS_DENIED;}
    ZeroMem(&S->Session,sizeof(S->Session));ZeroMem(S->Files,sizeof(S->Files));ZeroMem(S->Readers,sizeof(S->Readers));ZeroMem(S->Blobs,sizeof(S->Blobs));
  }
  S->Report.Memory=S->Report.Volume=S->Report.Files=S->Report.Session=EFI_NOT_STARTED;S->Report.Cleanup=EFI_SUCCESS;
  S->Report.FilesLoaded=S->Report.StartCalled=S->Report.OwnersRetired=FALSE;
  if(S->Report.Attempts==MAX_UINT64)return EFI_OUT_OF_RESOURCES;
  S->Report.Family=Family;S->Report.Sequence=Sequence;++S->Report.Attempts;
  EFI_STATUS E=CheckContext(S,Family,Sequence);if(E!=EFI_SUCCESS)return S->Report.Status=E;
  CONST PIANO_PRODUCT_OS_PIN *Pin=Family==PianoProductOsStable?&S->Env.Stable:&S->Env.Next;
  UINT64 Budget=Pin->ImageBytes+Pin->DtbBytes+Pin->InitrdBytes;PIANO_LINUX_MEMORY_PROOF Memory;
  S->Report.Memory=E=PianoCpuInputAuthorize(&S->Env.Cpu,Budget,&Memory);if(E!=EFI_SUCCESS)return S->Report.Status=E;
  S->Report.Volume=E=Volume(S);if(E!=EFI_SUCCESS)return S->Report.Status=E;
  S->Report.Busy=TRUE;
  CONST CHAR16 *Paths[2][3]={{L"\\EFI\\Piano\\stable\\Image.efi",L"\\EFI\\Piano\\stable\\piano.dtb",L"\\EFI\\Piano\\shared\\initramfs.cpio"},
    {L"\\EFI\\Piano\\next\\Image.efi",L"\\EFI\\Piano\\next\\piano.dtb",L"\\EFI\\Piano\\shared\\initramfs.cpio"}};
  PIANO_BOOT_FILE_SPEC Specs[3]={{S->FileSystem,Paths[Family-1][0],Pin->ImageBytes,Pin->ImageSha256},
    {S->FileSystem,Paths[Family-1][1],Pin->DtbBytes,Pin->DtbSha256},{S->FileSystem,Paths[Family-1][2],Pin->InitrdBytes,Pin->InitrdSha256}};
  PIANO_BOOT_FILE_ENV FileEnv={S,S->Env.Services,FileLive,&S->FileCpu};
  S->FileIoActive=TRUE;S->Report.Files=E=PianoBootFileLoadBundle(S->Files,3,&FileEnv,Specs,Budget,S->Readers,S->Blobs);S->FileIoActive=FALSE;
  if(E!=EFI_SUCCESS)goto Done;S->Report.FilesLoaded=TRUE;
  E=CheckContext(S,Family,Sequence);if(E!=EFI_SUCCESS)goto Dispose;
  S->Linux.Services=S->Env.Services;S->Linux.SystemTable=S->Env.SystemTable;S->Linux.ParentImage=S->Env.ParentImage;
  S->Linux.MaxSourceBytes=Budget;S->Linux.MaxKernelBytes=Pin->ImageBytes;S->Linux.MaxDtbBytes=Pin->DtbBytes;S->Linux.MaxInitrdBytes=Pin->InitrdBytes;
  S->Report.Session=E=PianoLinuxEfiSessionRun(&S->Session,&S->Linux,&S->Blobs[0],&S->Blobs[1],&S->Blobs[2]);
  S->Report.StartCalled=S->Session.StartCalled;S->Report.OwnersRetired=S->Session.OwnersRetired;
Dispose:
  if(!S->Session.Retained&&!S->Session.BeforeEbs&&!S->Session.Ebs){for(UINTN I=0;I<3;++I)if(S->Files[I].Signature&&!S->Files[I].Consumed){EFI_STATUS F=PianoBootFileDispose(&S->Files[I]);if(F!=EFI_SUCCESS){S->Report.Cleanup=F;E=F;}}}
Done:
  for(UINTN I=0;I<3;++I)if(S->Files[I].Retained)S->Report.Retained=TRUE;
  if(S->Session.Retained||S->Session.BeforeEbs||S->Session.Ebs||S->Report.Cleanup!=EFI_SUCCESS)S->Report.Retained=TRUE;
  S->Report.Busy=FALSE;return S->Report.Status=E;
}
