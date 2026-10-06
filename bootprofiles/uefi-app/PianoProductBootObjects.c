// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductBootObjects.h"
#include <Library/HobLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
STATIC PIANO_COLD_BOOT_OBJECT_REPORT mObjects,mObjectsScratch;
STATIC EFI_GUID mObjectsGuid=PIANO_COLD_OBJECT_HOB_GUID;
STATIC EFI_STATUS mObjectsStatus=EFI_NOT_STARTED,mObjectsValidation=EFI_NOT_STARTED;
STATIC BOOLEAN mObjectsAttempted,mObjectsLost;
STATIC BOOLEAN ObjectsAlive(BOOLEAN(*Alive)(VOID)){if(mObjectsLost||!Alive||Alive()!=TRUE){mObjectsLost=TRUE;return FALSE;}return TRUE;}
STATIC VOID ObjectsCapture(BOOLEAN(*Alive)(VOID)){
 mObjectsAttempted=TRUE;mObjectsStatus=EFI_NOT_FOUND;
 VOID *Hob=GetFirstGuidHob(&mObjectsGuid);if(!ObjectsAlive(Alive)){mObjectsStatus=EFI_ABORTED;return;}if(!Hob)return;
 EFI_HOB_GUID_TYPE *G=Hob;
 if(G->Header.HobType!=EFI_HOB_TYPE_GUID_EXTENSION||G->Header.Reserved||G->Header.HobLength!=sizeof(*G)+sizeof(mObjects)||CompareMem(&G->Name,&mObjectsGuid,sizeof(mObjectsGuid))){mObjectsStatus=EFI_COMPROMISED_DATA;return;}
 if(GetNextGuidHob(&mObjectsGuid,(UINT8*)G+G->Header.HobLength)){mObjectsStatus=EFI_COMPROMISED_DATA;return;}
 if(!ObjectsAlive(Alive)){mObjectsStatus=EFI_ABORTED;return;}
 CopyMem(&mObjects,GET_GUID_HOB_DATA(G),sizeof(mObjects));if(!ObjectsAlive(Alive)){ZeroMem(&mObjects,sizeof(mObjects));mObjectsStatus=EFI_ABORTED;return;}
 CopyMem(&mObjectsScratch,GET_GUID_HOB_DATA(G),sizeof(mObjectsScratch));if(!ObjectsAlive(Alive)){ZeroMem(&mObjects,sizeof(mObjects));ZeroMem(&mObjectsScratch,sizeof(mObjectsScratch));mObjectsStatus=EFI_ABORTED;return;}
 mObjectsValidation=PianoColdBootObjectsValidate(&mObjects);
 if(CompareMem(&mObjects,&mObjectsScratch,sizeof(mObjects))||!mObjects.Published||mObjects.PublishStatus!=EFI_SUCCESS||
    (mObjectsValidation!=EFI_SUCCESS&&mObjectsValidation!=EFI_NOT_READY)){ZeroMem(&mObjects,sizeof(mObjects));mObjectsStatus=EFI_COMPROMISED_DATA;}else mObjectsStatus=EFI_SUCCESS;
 ZeroMem(&mObjectsScratch,sizeof(mObjectsScratch));
}
EFI_STATUS PianoProductBootObjectsStatus(VOID){return mObjectsStatus;}
CONST PIANO_COLD_BOOT_OBJECT_REPORT *PianoProductBootObjectsSnapshot(VOID){return mObjectsStatus==EFI_SUCCESS?&mObjects:NULL;}
EFI_STATUS PianoProductBootObjectsReemit(BOOLEAN(*Alive)(VOID)){
 if(!Alive)return EFI_NOT_READY;if(!ObjectsAlive(Alive))return EFI_ABORTED;
 if(!mObjectsAttempted)ObjectsCapture(Alive);if(!ObjectsAlive(Alive))return EFI_ABORTED;
 DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_HOB capture=%r validation=%r version=%u bytes=%u crc=%08x ownership=0 high_ddr=0 authority=0\n",mObjectsStatus,mObjectsValidation,mObjects.Version,mObjects.Bytes,mObjects.ReportCrc32));
 if(mObjectsStatus!=EFI_SUCCESS)return mObjectsStatus;
 CONST PIANO_COLD_BOOT_OBJECT_REPORT *R=&mObjects;
 DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_STATUS status=%r reason=%u handoff=%r dtb=%r coherent=%u epoch=%lx count=%u\n",R->Status,R->Reason,R->HandoffStatus,R->DtbStatus,R->Coherent,R->Epoch,R->Count));
 DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_CPU pc=%lx sp=%lx el=%lx sctlr=%lx vbar=%lx daif=%lx spsel=%lu\n",R->Cpu.Pc,R->Cpu.Sp,R->Cpu.El,R->Cpu.Sctlr,R->Cpu.Vbar,R->Cpu.Daif,R->Cpu.SpSel));
 DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_TABLES ttbr0=%lx ttbr1=%lx counter=%lx frequency=%lu after_el=%lx after_sctlr=%lx after_vbar=%lx\n",R->Cpu.Ttbr0,R->Cpu.Ttbr1,R->Cpu.Counter,R->Cpu.Frequency,R->After.El,R->After.Sctlr,R->After.Vbar));
 DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_READ loads=%u recovered=%u ticks=%lu usecs=%lu last=%lx bytes=%u completed=%lx\n",R->Loads,R->RecoveredFaults,R->ElapsedTicks,R->ElapsedUsecs,R->LastReadStart,R->LastReadBytes,R->LastCompletedAddress));
 DEBUG((DEBUG_WARN,"PIANO_COLD_HANDOFF version=%u bytes=%u flags=%lx crc=%08x dtb=%lx entry_pc=%lx entry_sp=%lx entry_el=%lx\n",R->Handoff.Version,R->Handoff.Bytes,R->Handoff.Flags,R->Handoff.Crc32,R->Handoff.Dtb,R->Handoff.EntryPc,R->Handoff.EntrySp,R->Handoff.EntryEl));
 DEBUG((DEBUG_WARN,"PIANO_COLD_HANDOFF_FD shim=%lx/%lx source=%lx destination=%lx bytes=%lx counter=%lx frequency=%lu\n",R->Handoff.ShimBase,R->Handoff.ShimBytes,R->Handoff.FdSource,R->Handoff.FdBase,R->Handoff.FdBytes,R->Handoff.Counter,R->Handoff.Frequency));
 DEBUG((DEBUG_WARN,"PIANO_COLD_DTB bytes=%u header_crc=%08x initrd=%lx..%lx whole_dtb_identity=0\n",R->DtbBytes,R->DtbHeaderCrc32,R->InitrdStart,R->InitrdEnd));
 for(UINT32 I=0;I<R->Count;++I){if(!ObjectsAlive(Alive))return EFI_ABORTED;CONST PIANO_COLD_BOOT_OBJECT *O=&R->Objects[I];DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT i=%u role=%u base=%lx bytes=%lx occupied_input=1 allocation_permission=0\n",I,O->Role,O->Base,O->Bytes));}
 if(R->RecoveredFaults)DEBUG((DEBUG_WARN,"PIANO_COLD_OBJECT_FAULT address=%lx elr=%lx esr=%lx far=%lx spsr=%lx resume=%lx\n",R->LastFault.Address,R->LastFault.Elr,R->LastFault.Esr,R->LastFault.Far,R->LastFault.Spsr,R->LastFault.Resume));
 return ObjectsAlive(Alive)?R->Status:EFI_ABORTED;
}
