// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoLinuxEfiSession.h"
#include "../../core/PianoPanelSelection.h"
#include <Guid/Fdt.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#define SIG SIGNATURE_32('P','L','E','S')
STATIC BOOLEAN Overlap(CONST VOID*,UINTN,CONST VOID*,UINTN);
STATIC EFI_STATUS Exact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC VOID Halt(PIANO_LINUX_EFI_SESSION*S,EFI_STATUS E){S->Retained=TRUE;S->Status=E;
#ifdef __aarch64__
 __asm__ volatile("msr daifset, #15":::"memory");
#endif
 S->Env.FailStop(S->Env.Context,E);CpuDeadLoop();}
STATIC VOID Alive(PIANO_LINUX_EFI_SESSION*S){if(S->BeforeEbs||S->Ebs||!S->Env.BootServicesAlive(S->Env.Context))Halt(S,EFI_ABORTED);}
STATIC VOID EFIAPI Before(EFI_EVENT E,VOID*C){(VOID)E;((PIANO_LINUX_EFI_SESSION*)C)->BeforeEbs=TRUE;}
STATIC VOID EFIAPI Exit(EFI_EVENT E,VOID*C){(VOID)E;((PIANO_LINUX_EFI_SESSION*)C)->Ebs=TRUE;}
STATIC EFI_STATUS Table(PIANO_LINUX_EFI_SESSION*S,VOID **Value){
 Alive(S);EFI_SYSTEM_TABLE*T=S->Env.SystemTable;*Value=NULL;
 if(T->NumberOfTableEntries>1024||(T->NumberOfTableEntries&&!T->ConfigurationTable))return EFI_COMPROMISED_DATA;
 BOOLEAN Found=FALSE;for(UINTN I=0;I<T->NumberOfTableEntries;++I)if(CompareMem(&T->ConfigurationTable[I].VendorGuid,&gFdtTableGuid,sizeof(EFI_GUID))==0){if(Found)return EFI_COMPROMISED_DATA;Found=TRUE;*Value=T->ConfigurationTable[I].VendorTable;}
 return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI LoadInitrd(EFI_LOAD_FILE2_PROTOCOL*This,EFI_DEVICE_PATH_PROTOCOL*Path,BOOLEAN Policy,UINTN*Bytes,VOID*Buffer){
 if(!This)return EFI_INVALID_PARAMETER;PIANO_LINUX_EFI_SESSION*S=BASE_CR(This,PIANO_LINUX_EFI_SESSION,Load);
 if(S->Signature!=SIG||!S->Busy)return EFI_NOT_READY;
 if(S->BeforeEbs||S->Ebs||!S->Env.BootServicesAlive(S->Env.Context))return EFI_ABORTED;
 if(Policy)return EFI_UNSUPPORTED;
 if(!Bytes||!Path||Path->Type!=END_DEVICE_PATH_TYPE||Path->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE||Path->Length[0]!=4||Path->Length[1])return EFI_INVALID_PARAMETER;
 if(Overlap(Bytes,sizeof(*Bytes),S,sizeof(*S)))return EFI_INVALID_PARAMETER;
 for(UINTN N=0;N<3;++N)if(S->Views[N]&&Overlap(Bytes,sizeof(*Bytes),S->Views[N],(UINTN)S->Sources[N].Bytes))return EFI_INVALID_PARAMETER;
 UINTN Required=(UINTN)S->Sources[2].Bytes;
 if(!Buffer||*Bytes<Required){*Bytes=Required;return EFI_BUFFER_TOO_SMALL;}
 if(!S->Views[2]||!S->Owners[2]||!S->Loans[2])return EFI_NOT_READY;
 if(Overlap(Buffer,Required,S,sizeof(*S)))return EFI_INVALID_PARAMETER;
 for(UINTN N=0;N<3;++N)if(S->Views[N]&&Overlap(Buffer,Required,S->Views[N],(UINTN)S->Sources[N].Bytes))return EFI_INVALID_PARAMETER;
 EFI_STATUS Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->SourceMemory,
   S->Sources[2].Context,S->Owners[2],S->Views[2],S->Sources[2].Bytes);
 if(Status==EFI_SUCCESS)Status=PianoCpuInputCopy(S->HasCpu?&S->Cpu:NULL,Buffer,S->Views[2],Required);
 if(S->BeforeEbs||S->Ebs||!S->Env.BootServicesAlive(S->Env.Context))return EFI_ABORTED;
 if(Status==EFI_SUCCESS)*Bytes=Required;return Status;
}
STATIC EFI_STATUS KernelRead(VOID*C,UINT64 At,UINTN Bytes,VOID*Buffer){PIANO_LINUX_EFI_SESSION*S=C;Alive(S);EFI_STATUS E=S->Sources[0].Read(S->Sources[0].Context,S->Owners[0],At,Bytes,Buffer);Alive(S);return E;}
STATIC BOOLEAN Missing(EFI_STATUS E){return E==EFI_NOT_FOUND||E==EFI_UNSUPPORTED||E==EFI_INVALID_PARAMETER;}
STATIC EFI_STATUS RetireImage(PIANO_LINUX_EFI_SESSION*S){
 if(!S->Image)return EFI_SUCCESS;Alive(S);EFI_LOADED_IMAGE_PROTOCOL*L=NULL;EFI_STATUS E=S->Env.Services->HandleProtocol(S->Image,&gEfiLoadedImageProtocolGuid,(VOID**)&L);Alive(S);
 if(S->StartReturned&&Missing(E)){S->AutoUnloaded=TRUE;S->Image=NULL;S->OptionsInstalled=FALSE;return EFI_SUCCESS;}
 if(E!=EFI_SUCCESS||!L)return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
 if(S->LoadedIdentity&&(L!=S->LoadedIdentity||L->ImageBase!=S->ImageBaseIdentity||L->ImageSize!=S->ImageSizeIdentity))return EFI_COMPROMISED_DATA;
 if(S->OptionsInstalled){if(L->LoadOptions!=S->OptionsCopy||L->LoadOptionsSize!=S->OptionsBytes)return EFI_COMPROMISED_DATA;L->LoadOptions=S->OldOptions;L->LoadOptionsSize=S->OldOptionsBytes;S->OptionsInstalled=FALSE;}
 E=Exact(S->Env.Services->UnloadImage(S->Image));Alive(S);if(E==EFI_SUCCESS)S->Image=NULL;return E;
}
STATIC EFI_STATUS Free(PIANO_LINUX_EFI_SESSION*S,VOID**P){if(!*P)return EFI_SUCCESS;Alive(S);EFI_STATUS E=Exact(S->Env.Services->FreePool(*P));Alive(S);if(E==EFI_SUCCESS)*P=NULL;return E;}
STATIC EFI_STATUS Cleanup(PIANO_LINUX_EFI_SESSION*S){
 Alive(S);EFI_STATUS E;
 if(S->LateArmed){E=Exact(S->Env.NativeLateDisarm(S->Env.Context,S->Image));Alive(S);if(E!=EFI_SUCCESS)Halt(S,E);S->LateArmed=FALSE;}
 E=RetireImage(S);if(E!=EFI_SUCCESS)return E;
 if(S->InitrdInstalled){E=Exact(S->Env.Services->UninstallMultipleProtocolInterfaces(S->InitrdHandle,&gEfiLoadFile2ProtocolGuid,&S->Load,&gEfiDevicePathProtocolGuid,&S->Path,NULL));Alive(S);if(E!=EFI_SUCCESS)return E;S->InitrdInstalled=FALSE;S->InitrdHandle=NULL;}
 if(S->FdtInstalled){VOID*Current=NULL;E=Table(S,&Current);if(E!=EFI_SUCCESS||Current!=S->FdtCopy)return EFI_COMPROMISED_DATA;
 E=Exact(S->Env.Services->InstallConfigurationTable(&gFdtTableGuid,S->OldFdt));Alive(S);if(E!=EFI_SUCCESS)return E;S->FdtInstalled=FALSE;}
 E=Free(S,&S->ExitData);if(E!=EFI_SUCCESS)return E;E=Free(S,&S->OptionsCopy);if(E!=EFI_SUCCESS)return E;E=Free(S,&S->FdtCopy);if(E!=EFI_SUCCESS)return E;
 EFI_EVENT*Events[]={&S->BeforeEvent,&S->ExitEvent};for(UINTN I=0;I<2;++I)if(*Events[I]){E=Exact(S->Env.Services->CloseEvent(*Events[I]));Alive(S);if(E!=EFI_SUCCESS)return E;*Events[I]=NULL;}
 for(UINTN I=3;I>0;--I){UINTN K=I-1;if(S->Loans[K]){E=Exact(S->Sources[K].Unborrow(S->Sources[K].Context,S->Owners[K],S->Loans[K]));Alive(S);if(E!=EFI_SUCCESS)return E;S->Loans[K]=NULL;S->Views[K]=NULL;}
 if(S->Owners[K]){E=Exact(S->Sources[K].ZeroRelease(S->Sources[K].Context,S->Owners[K]));Alive(S);if(E!=EFI_SUCCESS)return E;S->Owners[K]=NULL;}}
 return EFI_SUCCESS;
}
STATIC BOOLEAN ValidBlob(CONST PIANO_LAUNCH_BLOB*B,UINT64 Max){return B&&B->Context&&B->Bytes&&B->Bytes<=Max&&B->Bytes<=MAX_UINTN&&B->Take&&B->Read&&B->BorrowView&&B->Unborrow&&B->ZeroRelease;}
STATIC BOOLEAN Overlap(CONST VOID*A,UINTN An,CONST VOID*B,UINTN Bn){
 UINTN X=(UINTN)A,Y=(UINTN)B;if(X>MAX_UINTN-An||Y>MAX_UINTN-Bn)return TRUE;
 return X<Y+Bn&&Y<X+An;
}
EFI_STATUS PianoLinuxEfiSessionRun(PIANO_LINUX_EFI_SESSION*S,CONST PIANO_LINUX_EFI_ENV*E,CONST PIANO_LAUNCH_BLOB*K,CONST PIANO_LAUNCH_BLOB*D,CONST PIANO_LAUNCH_BLOB*I){
 if(!S||!E||!E->Services||!E->SystemTable||!E->ParentImage||!E->BootServicesAlive||!E->FailStop||!E->ServiceSlice||!E->CommandLine||!E->ExpectedOwners||!E->ExpectedDramBytes||!E->MaxLoadedBytes)return EFI_INVALID_PARAMETER;
 if(E->HandoffMode!=PianoHandoffLegacyPreStart&&E->HandoffMode!=PianoHandoffNativeLate)return EFI_INVALID_PARAMETER;
 if(PIANO_PRODUCT_NATIVE_LATE && E->HandoffMode!=PianoHandoffNativeLate)return EFI_UNSUPPORTED;
 if(!E->CheckMemory||!E->ValidateMemory)return EFI_NOT_READY;
 if(E->HandoffMode==PianoHandoffNativeLate){if(!E->NativeLateArm||!E->NativeLateDisarm)return EFI_NOT_READY;}
 else if(!E->PrepareHandoff||!E->ValidateRetired)return EFI_NOT_READY;
 if(Overlap(E,sizeof(*E),S,sizeof(*S))||Overlap(K,sizeof(*K),S,sizeof(*S))||Overlap(D,sizeof(*D),S,sizeof(*S))||Overlap(I,sizeof(*I),S,sizeof(*S)))return EFI_INVALID_PARAMETER;
 if(!ValidBlob(K,E->MaxKernelBytes)||!ValidBlob(D,E->MaxDtbBytes)||!ValidBlob(I,E->MaxInitrdBytes)||D->Bytes<40||D->Bytes>0x200000)return EFI_BAD_BUFFER_SIZE;
 CONST PIANO_LAUNCH_BLOB*Input[]={K,D,I};UINT64 Total=0;
 UINT64 SourceLimit=E->MaxSourceBytes?E->MaxSourceBytes:PIANO_LINUX_LOW_SOURCE_BUDGET;
 if(!SourceLimit||SourceLimit>PIANO_CPU_INPUT_MAX_BYTES)return EFI_BAD_BUFFER_SIZE;
 if(E->Cpu&&Overlap(E->Cpu,sizeof(*E->Cpu),S,sizeof(*S)))return EFI_INVALID_PARAMETER;
 for(UINTN N=0;N<3;++N){
   if(Input[N]->Bytes>SourceLimit||Total>SourceLimit-Input[N]->Bytes)return EFI_BAD_BUFFER_SIZE;
   Total+=Input[N]->Bytes;
   if(Overlap(Input[N]->Context,1,S,sizeof(*S)))return EFI_INVALID_PARAMETER;
   for(UINTN P=0;P<N;++P)if(Input[N]->Context==Input[P]->Context)return EFI_INVALID_PARAMETER;
 }
 if(S->Signature==SIG&&(S->Busy||S->Retained||S->OwnersRetired))return EFI_ALREADY_STARTED;
 EFI_BOOT_SERVICES*B=E->Services;
 if(!B->RaiseTPL||!B->RestoreTPL||!B->LoadImage||!B->StartImage||!B->HandleProtocol||!B->UnloadImage||!B->AllocatePool||!B->FreePool||!B->InstallConfigurationTable||!B->InstallMultipleProtocolInterfaces||!B->UninstallMultipleProtocolInterfaces||!B->CreateEventEx||!B->CloseEvent||!B->LocateDevicePath)return EFI_UNSUPPORTED;
 if(!E->BootServicesAlive(E->Context))return EFI_ABORTED;
 EFI_TPL T=B->RaiseTPL(TPL_HIGH_LEVEL);B->RestoreTPL(T);if(T!=TPL_APPLICATION)return EFI_UNSUPPORTED;
 PIANO_LINUX_MEMORY_PROOF SourceMemory;EFI_STATUS Authorized=PianoCpuInputAuthorize(E->Cpu,SourceLimit,&SourceMemory);
 if(Authorized!=EFI_SUCCESS)return Authorized;
 ZeroMem(S,sizeof(*S));S->Signature=SIG;S->Busy=TRUE;S->Env=*E;S->Sources[0]=*K;S->Sources[1]=*D;S->Sources[2]=*I;
 S->SourceMemory=SourceMemory;if(E->Cpu){S->Cpu=*E->Cpu;S->HasCpu=TRUE;S->Env.Cpu=&S->Cpu;}
 EFI_STATUS Status=EFI_SUCCESS;
 // Three independent CPU leases remain until all image/table/protocol users end.
 for(UINTN N=0;N<3;++N){Status=Exact(S->Sources[N].Take(S->Sources[N].Context,&S->Owners[N]));Alive(S);
 if(Status!=EFI_SUCCESS){if(S->Owners[N])S->Retained=TRUE;goto Done;}if(!S->Owners[N]){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 PIANO_BOOT_RANGE R={0,S->Sources[N].Bytes};Status=Exact(S->Sources[N].BorrowView(S->Sources[N].Context,S->Owners[N],R,&S->Views[N],&S->Loans[N]));Alive(S);
 if((S->Views[N]!=NULL)!=(S->Loans[N]!=NULL)){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 if(Status!=EFI_SUCCESS){if(S->Views[N]||S->Loans[N])S->Retained=TRUE;goto Done;}if(!S->Views[N]||!S->Loans[N]){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 // Returned views are known only after Borrow. Refuse at the first overlap
 // without touching/releasing any possibly shared source storage.
 if(Overlap(S->Views[N],(UINTN)S->Sources[N].Bytes,S,sizeof(*S))){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 for(UINTN P=0;P<N;++P)if(Overlap(S->Views[N],(UINTN)S->Sources[N].Bytes,S->Views[P],(UINTN)S->Sources[P].Bytes)){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 Status=PianoCpuInputValidateBuffer(S->HasCpu?&S->Cpu:NULL,&S->SourceMemory,
   S->Sources[N].Context,S->Owners[N],S->Views[N],S->Sources[N].Bytes);
 Alive(S);if(Status!=EFI_SUCCESS){S->Retained=TRUE;goto Done;}
 }
 PIANO_BOOT_SOURCE Src={S,KernelRead,K->Bytes};Status=PianoFastbootBootParse(&Src,&S->Kernel);if(Status!=EFI_SUCCESS)goto Done;
 if(S->Kernel.Kind!=PianoBootArm64Pe||S->Kernel.Pe.ImageBytes>E->MaxLoadedBytes){Status=EFI_UNSUPPORTED;goto Done;}
 // libfdt needs aligned data. Copy into an owned LoaderData allocation before
 // checking an immutable DTB which may originally follow an unaligned file.
 S->FdtCapacity=(UINTN)D->Bytes+65536;Status=Exact(B->AllocatePool(EfiLoaderData,S->FdtCapacity,&S->FdtCopy));Alive(S);if(Status!=EFI_SUCCESS)goto Done;if(!S->FdtCopy){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 Status=PianoCpuInputCopy(S->HasCpu?&S->Cpu:NULL,S->FdtCopy,S->Views[1],(UINTN)D->Bytes);
 Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 if(FdtCheckHeader(S->FdtCopy)||FdtTotalSize(S->FdtCopy)!=D->Bytes||FdtOpenInto(S->FdtCopy,S->FdtCopy,(INT32)S->FdtCapacity)){Status=EFI_COMPROMISED_DATA;goto Done;}
 Status=PianoPanelSelectDtb(S->FdtCopy,S->FdtCapacity);if(Status!=EFI_SUCCESS)goto Done;
 INT32 Chosen=FdtPathOffset(S->FdtCopy,"/chosen");if(Chosen<0){Status=EFI_NOT_FOUND;goto Done;}
 CONST CHAR8*Remove[]={"linux,initrd-start","linux,initrd-end","kaslr-seed","rng-seed"};for(UINTN N=0;N<4;++N){INT32 R=FdtDelProp(S->FdtCopy,Chosen,Remove[N]);if(R!=0&&R!=-1){Status=EFI_COMPROMISED_DATA;goto Done;}}
 CHAR8 Ascii[2048];UINTN Chars=0;while(Chars<ARRAY_SIZE(Ascii)-1&&E->CommandLine[Chars]){if(E->CommandLine[Chars]>127){Status=EFI_UNSUPPORTED;goto Done;}Ascii[Chars]=(CHAR8)E->CommandLine[Chars];++Chars;}if(E->CommandLine[Chars]){Status=EFI_BAD_BUFFER_SIZE;goto Done;}Ascii[Chars]=0;
 if(FdtSetProp(S->FdtCopy,Chosen,"bootargs",Ascii,(INT32)Chars+1)){Status=EFI_COMPROMISED_DATA;goto Done;}
 S->OptionsBytes=(UINT32)((Chars+1)*sizeof(CHAR16));Status=Exact(B->AllocatePool(EfiLoaderData,S->OptionsBytes,&S->OptionsCopy));Alive(S);if(Status!=EFI_SUCCESS)goto Done;if(!S->OptionsCopy){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}CopyMem(S->OptionsCopy,E->CommandLine,S->OptionsBytes);
 Status=Exact(E->CheckMemory(E->Context,S->FdtCopy,FdtTotalSize(S->FdtCopy),&S->Memory));Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 if(S->Memory.Revision!=1||S->Memory.Status!=EFI_SUCCESS||!S->Memory.BootEpoch||S->Memory.DramBytes!=E->ExpectedDramBytes||!S->Memory.NormalBytes||S->Memory.UnresolvedReservations||!S->Memory.FullDdr||!S->Memory.FixedReservations||!S->Memory.DynamicReservations||!S->Memory.RuntimeRegions||!S->Memory.CacheVerified||!S->Memory.OwnershipVerified){Status=EFI_NOT_READY;goto Done;}
 if(S->SourceMemory.BootEpoch&&(S->SourceMemory.BootEpoch!=S->Memory.BootEpoch||
    S->SourceMemory.DramBytes!=S->Memory.DramBytes)){Status=EFI_NOT_READY;goto Done;}
 Status=Exact(E->ValidateMemory(E->Context,&S->Memory));Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 S->Path=(PIANO_LINUX_INITRD_PATH){{{MEDIA_DEVICE_PATH,MEDIA_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},LINUX_EFI_INITRD_MEDIA_GUID},{END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}};S->Load.LoadFile=LoadInitrd;
 EFI_DEVICE_PATH_PROTOCOL *ExistingPath=&S->Path.Vendor.Header;EFI_HANDLE ExistingInitrd=NULL;
 Status=B->LocateDevicePath(&gEfiLoadFile2ProtocolGuid,&ExistingPath,&ExistingInitrd);Alive(S);
 if(Status!=EFI_NOT_FOUND){Status=Status==EFI_SUCCESS?EFI_ALREADY_STARTED:Exact(Status);goto Done;}
 Status=Table(S,&S->OldFdt);if(Status!=EFI_SUCCESS&&Status!=EFI_NOT_FOUND)goto Done;
 Status=Exact(B->InstallConfigurationTable(&gFdtTableGuid,S->FdtCopy));Alive(S);if(Status!=EFI_SUCCESS){S->Retained=TRUE;goto Done;}S->FdtInstalled=TRUE;
 Status=Exact(B->InstallMultipleProtocolInterfaces(&S->InitrdHandle,&gEfiLoadFile2ProtocolGuid,&S->Load,&gEfiDevicePathProtocolGuid,&S->Path,NULL));Alive(S);if(Status!=EFI_SUCCESS||!S->InitrdHandle){S->Retained=TRUE;if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;goto Done;}S->InitrdInstalled=TRUE;
 Status=Exact(B->LoadImage(FALSE,E->ParentImage,NULL,(VOID*)S->Views[0],(UINTN)K->Bytes,&S->Image));Alive(S);if(Status!=EFI_SUCCESS)goto Done;if(!S->Image){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
 EFI_LOADED_IMAGE_PROTOCOL*L=NULL;Status=Exact(B->HandleProtocol(S->Image,&gEfiLoadedImageProtocolGuid,(VOID**)&L));Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 if(!L||!L->ImageBase||L->ImageSize<S->Kernel.Pe.ImageBytes||L->ImageSize>E->MaxLoadedBytes||L->ImageCodeType!=EfiLoaderCode||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION){Status=EFI_COMPROMISED_DATA;goto Done;}
 S->LoadedIdentity=L;S->ImageBaseIdentity=L->ImageBase;S->ImageSizeIdentity=L->ImageSize;S->OldOptions=L->LoadOptions;S->OldOptionsBytes=L->LoadOptionsSize;L->LoadOptions=S->OptionsCopy;L->LoadOptionsSize=S->OptionsBytes;S->OptionsInstalled=TRUE;
 Status=Exact(B->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Before,S,&gEfiEventBeforeExitBootServicesGuid,&S->BeforeEvent));Alive(S);if(Status!=EFI_SUCCESS||!S->BeforeEvent){S->Retained=TRUE;if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;goto Done;}
 Status=Exact(B->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,S,&gEfiEventExitBootServicesGuid,&S->ExitEvent));Alive(S);if(Status!=EFI_SUCCESS||!S->ExitEvent){S->Retained=TRUE;if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;goto Done;}
 Status=Exact(E->ServiceSlice(E->Context,1000));Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 if(E->HandoffMode==PianoHandoffNativeLate){
   Status=E->NativeLateArm(E->Context,S->Image,L);Alive(S);
   if(Status!=EFI_SUCCESS){if(!EFI_ERROR(Status)){S->Retained=TRUE;Status=EFI_DEVICE_ERROR;}goto Done;}
   S->LateArmed=TRUE;
 }else{
 Status=Exact(E->PrepareHandoff(E->Context,&S->Memory,&S->Retire));Alive(S);if(Status!=EFI_SUCCESS){S->Retained=TRUE;goto Done;}
 if(S->Retire.Revision!=1||S->Retire.Status!=EFI_SUCCESS||!S->Retire.Clean||S->Retire.Retained||!S->Retire.NoDma||!S->Retire.AtApplication||S->Retire.ExpectedOwners!=E->ExpectedOwners||
    (S->Retire.RetiredOwners&S->Retire.AbsentOwners)||((S->Retire.RetiredOwners|S->Retire.AbsentOwners)!=E->ExpectedOwners)){S->Retained=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}S->OwnersRetired=TRUE;
 Status=Exact(E->ValidateRetired(E->Context,&S->Retire));Alive(S);if(Status!=EFI_SUCCESS){S->Retained=TRUE;goto Done;}
 // Owner retirement can change the live map. Recollect and revalidate against
 // the same boot epoch before starting the stub; old proof is insufficient.
 PIANO_LINUX_MEMORY_PROOF Fresh={0};Status=Exact(E->CheckMemory(E->Context,S->FdtCopy,FdtTotalSize(S->FdtCopy),&Fresh));Alive(S);if(Status!=EFI_SUCCESS)goto Done;
 if(Fresh.Revision!=1||Fresh.Status!=EFI_SUCCESS||Fresh.BootEpoch!=S->Memory.BootEpoch||Fresh.DramBytes!=S->Memory.DramBytes||!Fresh.NormalBytes||Fresh.UnresolvedReservations||!Fresh.FullDdr||!Fresh.FixedReservations||!Fresh.DynamicReservations||!Fresh.RuntimeRegions||!Fresh.CacheVerified||!Fresh.OwnershipVerified){Status=EFI_NOT_READY;goto Done;}
 Status=Exact(E->ValidateMemory(E->Context,&Fresh));Alive(S);if(Status!=EFI_SUCCESS)goto Done;S->Memory=Fresh;
 }
 S->StartCalled=TRUE;Status=Exact(B->StartImage(S->Image,&S->ExitBytes,(CHAR16**)&S->ExitData));S->StartReturned=TRUE;S->ImageExitStatus=Status;Alive(S);
 // Returning without an EBS handoff is not Linux boot success.
 if(Status==EFI_SUCCESS)Status=EFI_ABORTED;
Done:
 S->Status=Status;if(S->Retained)Halt(S,Status);
 S->CleanupStatus=Cleanup(S);if(S->CleanupStatus!=EFI_SUCCESS){S->Retained=TRUE;return S->Status=Status==EFI_SUCCESS?S->CleanupStatus:Status;}
 S->Busy=FALSE;return Status;
}
