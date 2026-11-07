// SPDX-License-Identifier: BSD-2-Clause-Patent
// The only publication point is SEC before the first MemoryPeim. The actual
// ABL handoff tree describes RAM; SMEM adds reservations, never a capacity cap.
#include "PianoColdMemory.h"
#include "PianoColdBootObjects.h"
#include "PianoPlatformMemoryContract.h"
#include "PianoEarlyMemory.h"
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>
STATIC PIANO_COLD_MEMORY_REPORT mMemory;
STATIC PIANO_PLATFORM_MEMORY_CONTRACT mContract;
STATIC PIANO_PLATFORM_MEMORY_INPUT mInput;
STATIC PIANO_PLATFORM_MEMORY_OWNER mOwners[PIANO_PLATFORM_MEMORY_MAX_OWNERS];
STATIC UINTN mOwnerCount;
STATIC BOOLEAN mAttempted,mPublished;
STATIC EFI_GUID mGuid=PIANO_COLD_MEMORY_HOB_GUID;
STATIC_ASSERT(sizeof(PIANO_COLD_MEMORY_REPORT)+sizeof(EFI_HOB_GUID_TYPE)<65536,"cold memory report HOB");
STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC EFI_STATUS Owner(UINT64 Base,UINT64 Bytes){
 if(!Bytes)return EFI_SUCCESS;if(Base>MAX_UINT64-Bytes||mOwnerCount>=ARRAY_SIZE(mOwners))return EFI_COMPROMISED_DATA;
 mOwners[mOwnerCount++]=(PIANO_PLATFORM_MEMORY_OWNER){Base,Bytes,-1};return EFI_SUCCESS;
}
STATIC EFI_STATUS Authorize(VOID *Context,CONST PIANO_PLATFORM_MEMORY_INPUT *Input,CONST PIANO_PLATFORM_MEMORY_CONTRACT *Contract){
 (VOID)Context;CONST PIANO_COLD_BOOT_OBJECT_REPORT *Objects=PianoColdBootObjectsReport();CONST PIANO_EARLY_MEMORY_REPORT *Early=PianoEarlyMemoryReport();
 if(Input!=&mInput||Contract!=&mContract||PianoColdBootObjectsValidate(Objects)!=EFI_SUCCESS||Objects->Published||Objects->Epoch!=mMemory.BootEpoch||
    !Early->Finished||Early->Status!=EFI_SUCCESS||!Early->ColdStateVerified||Early->Published||Early->EntryEl!=Objects->Cpu.El||Early->EntrySctlr!=Objects->Cpu.Sctlr||Early->EntryVbar!=Objects->Cpu.Vbar||Early->RecoveredFaults||Objects->RecoveredFaults)return EFI_NOT_READY;
 // Match the frozen dual-read semantic capture and revalidate the admitted
 // handoff and unchanged pre-PHIT CPU state, not a status boolean.
 PIANO_MEMORY_LAYOUT Fresh;EFI_STATUS S=PianoColdBootObjectsMemoryLayout(&Fresh);
 if(S!=EFI_SUCCESS)return S;if(CompareMem(&Fresh,&mMemory.Layout,sizeof(Fresh)))return EFI_MEDIA_CHANGED;
 return EFI_SUCCESS;
}
EFI_STATUS PianoColdMemoryPrepare(VOID){
 if(mAttempted)return EFI_ALREADY_STARTED;mAttempted=TRUE;ZeroMem(&mMemory,sizeof(mMemory));mMemory.Version=1;mMemory.Bytes=sizeof(mMemory);mMemory.Status=mMemory.PublishStatus=EFI_NOT_READY;
 CONST PIANO_COLD_BOOT_OBJECT_REPORT *Objects=PianoColdBootObjectsReport();CONST PIANO_EARLY_MEMORY_REPORT *Early=PianoEarlyMemoryReport();
 EFI_STATUS S=PianoColdBootObjectsValidate(Objects);if(S!=EFI_SUCCESS)return mMemory.Status=S;
 if(!Early->Finished||Early->Status!=EFI_SUCCESS||!Early->ColdStateVerified||!Early->RawPayloadCoherent||!Early->Smem.Parsed||Early->Smem.Status!=EFI_SUCCESS||Early->RawPayloadBytes!=Early->Smem.PayloadBytes||
    Early->RawPayloadCrc32!=PianoEarlyMemoryBytesCrc32(Early->RawPayload,Early->RawPayloadBytes)||!Early->Smem.RepeatedMetadataEqual||!Early->Smem.RepeatedPayloadEqual)return mMemory.Status=EFI_NOT_READY;
 mMemory.BootEpoch=Objects->Epoch;S=PianoColdBootObjectsMemoryLayout(&mMemory.Layout);if(S!=EFI_SUCCESS)return mMemory.Status=S;
 for(UINTN I=0;I<Objects->Count;++I){S=Owner(Objects->Objects[I].Base,Objects->Objects[I].Bytes);if(S!=EFI_SUCCESS)return mMemory.Status=S;}
 // Preserve every non-current native record, including categories unknown to
 // the RAM inventory parser. Their declared intervals are occupied, not banks.
 UINT32 Version=Early->Smem.RamVersion,Stride=Version==1?64:72,Count=Early->Smem.RawEntryCount;
 if(Early->RawPayloadBytes<24||(Version!=1&&Version!=2&&Version!=3)||Count>(Early->RawPayloadBytes-24)/Stride)return mMemory.Status=EFI_COMPROMISED_DATA;
 for(UINT32 I=0;I<Count;++I){CONST UINT8 *E=Early->RawPayload+24+(UINTN)I*Stride;UINT32 Category=Le32(E+0x24),Type=Le32(E+0x2c);
  if(Category==14&&Type==1)continue;S=Owner(Le64(E+0x10),Le64(E+0x18));if(S!=EFI_SUCCESS)return mMemory.Status=S;
 }
 EFI_MEMORY_REGION_DESCRIPTOR *Native=NULL;UINT8 NativeCount=0;GetMemoryMap(&Native,&NativeCount);mMemory.NativeCount=NativeCount;
 mInput=(PIANO_PLATFORM_MEMORY_INPUT){.Native=Native,.NativeCount=NativeCount,.KnownOwners=mOwners,.OwnerCount=mOwnerCount,.Layout=&mMemory.Layout,.ColdInventory=&Early->Smem};
 S=PianoPlatformMemoryCompose(&mInput,&mContract);if(S==EFI_SUCCESS)S=PianoPlatformMemoryAuthorizeCold(&mInput,&mContract,Authorize,NULL);
 EFI_MEMORY_REGION_DESCRIPTOR *Rows=NULL;UINT8 CountRows=0;if(S==EFI_SUCCESS)S=PianoPlatformMemoryAcquireForMemoryPeim(&mContract,&Rows,&CountRows);
 if(S==EFI_SUCCESS){mMemory.Count=CountRows;mMemory.InputFingerprint=mContract.InputFingerprint;CopyMem(mMemory.Rows,Rows,CountRows*sizeof(*Rows));S=PianoProductSelectMemoryMap(Rows,CountRows);}
 mMemory.Status=S;mMemory.Crc32=PianoColdMemoryCrc(&mMemory);
 DEBUG((DEBUG_WARN,"PIANO_COLD_MEMORY status=%r epoch=%lx rows=%u native=%u factory_ram=%lx occupied=%lx retained=%lx allocator_expansion=0\n",S,mMemory.BootEpoch,mMemory.Count,mMemory.NativeCount,mMemory.Layout.DramBytes,mContract.AddedOccupiedBytes,mContract.RetainedReservedBytes));
 return S;
}
EFI_STATUS PianoColdMemoryPublishHob(VOID){
 if(!mAttempted||mPublished)return EFI_NOT_READY;VOID *Data=BuildGuidHob(&mGuid,sizeof(mMemory));if(!Data)return mMemory.PublishStatus=EFI_OUT_OF_RESOURCES;
 mPublished=TRUE;mMemory.PublishStatus=EFI_SUCCESS;mMemory.Crc32=PianoColdMemoryCrc(&mMemory);CopyMem(Data,&mMemory,sizeof(mMemory));return EFI_SUCCESS;
}
