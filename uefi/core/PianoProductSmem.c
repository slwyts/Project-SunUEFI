// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductSmem.h"
#include "PianoSmemRam.h"
#include "PianoGuardedRead.h"
#include "PianoEarlyMemory.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>

STATIC PIANO_SMEM_RAM_WORK mWork;
STATIC PIANO_SMEM_RAM_REPORT mSmem={.Status=EFI_NOT_STARTED,.CookieStatus=EFI_NOT_READY};
STATIC PIANO_GUARDED_REPORT mGuardSnapshot;
STATIC EFI_STATUS mObservationStatus=EFI_NOT_STARTED;
STATIC EFI_BOOT_SERVICES *mBoot;
STATIC BOOLEAN mPhase, mAttempted, mRetained;
STATIC PIANO_EARLY_MEMORY_REPORT mEarly,mEarlyScratch;
STATIC PIANO_SMEM_RAM_REPORT mV3Check;
STATIC EFI_STATUS mEarlyStatus=EFI_NOT_STARTED;
STATIC EFI_GUID mEarlyGuid=PIANO_EARLY_MEMORY_HOB_GUID;

STATIC BOOLEAN EarlySpan(UINT64 Base,UINT64 Bytes){return Bytes&&Base<=MAX_UINT64-Bytes;}
STATIC BOOLEAN EarlyBoolean(BOOLEAN B){return B==FALSE||B==TRUE;}
STATIC BOOLEAN EarlyZero(CONST VOID *Bytes,UINTN Count){
  CONST UINT8 *P=Bytes;for(UINTN I=0;I<Count;++I)if(P[I])return FALSE;return TRUE;
}
STATIC BOOLEAN EarlyWindow(UINT64 Address,UINT32 Bytes){
  return Bytes&&!(Address&3)&&!(Bytes&3)&&Address>=PIANO_SMEM_BASE&&
    Address-PIANO_SMEM_BASE<PIANO_SMEM_BYTES&&Bytes<=PIANO_SMEM_BYTES-(Address-PIANO_SMEM_BASE);
}
STATIC UINT16 Early16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC UINT32 Early32(CONST UINT8 *P){return Early16(P)|((UINT32)Early16(P+2)<<16);}
STATIC UINT64 Early64(CONST UINT8 *P){return Early32(P)|((UINT64)Early32(P+4)<<32);}
STATIC BOOLEAN EarlyRawValid(CONST PIANO_EARLY_MEMORY_REPORT *R){
  CONST PIANO_SMEM_RAM_REPORT *S=&R->Smem;CONST PIANO_EARLY_SIII_DIAGNOSTIC *D=&R->Descriptor;
  if(!EarlyBoolean(R->RawPayloadCoherent)||!EarlyBoolean(R->RawDescriptorCoherent)||
     !EarlyZero(R->RawReserved,sizeof(R->RawReserved))||D->Reserved||
     !EarlyZero(D->ReservedFlags,sizeof(D->ReservedFlags))||
     !EarlyBoolean(D->RepeatedEqual)||!EarlyBoolean(D->RegionMatchesKnownWindow)||
     (D->Status!=EFI_SUCCESS&&!EFI_ERROR(D->Status))||
     R->RawPayloadBytes>sizeof(R->RawPayload)||R->RawDescriptorBytes>sizeof(R->RawDescriptor))return FALSE;
  if(!EarlyZero(R->RawPayload+R->RawPayloadBytes,sizeof(R->RawPayload)-R->RawPayloadBytes)||
     !EarlyZero(R->RawDescriptor+R->RawDescriptorBytes,sizeof(R->RawDescriptor)-R->RawDescriptorBytes))return FALSE;
  if(R->RawPayloadCoherent){
    if(!R->ColdStateVerified||!S->RepeatedMetadataEqual||!S->RepeatedPayloadEqual||
       R->RawPayloadBytes<24||R->RawPayloadBytes!=S->PayloadBytes||
       R->RawPayloadCrc32!=S->PayloadCrc32||!EarlyWindow(S->PayloadAddress,R->RawPayloadBytes)||
       PianoEarlyMemoryBytesCrc32(R->RawPayload,R->RawPayloadBytes)!=R->RawPayloadCrc32)return FALSE;
  }else if(R->RawPayloadBytes||R->RawPayloadCrc32||S->RepeatedPayloadEqual)return FALSE;
  if(R->RawDescriptorCoherent){
    UINT32 Bytes=R->RawDescriptorBytes;
    if(!R->ColdStateVerified||!D->RepeatedEqual||S->CookieStatus!=EFI_SUCCESS||!S->CookieRepeatedEqual||
       D->Address!=S->CookieValue||Bytes<20||Bytes!=D->SnapshotBytes||
       R->RawDescriptorCrc32!=D->Crc32||!EarlyWindow(D->Address,Bytes)||
       PianoEarlyMemoryBytesCrc32(R->RawDescriptor,Bytes)!=D->Crc32||
       D->TlvCount>64||Early32(R->RawDescriptor)!=0x49494953U||
       Early32(R->RawDescriptor+4)!=D->SmemBytes||Early64(R->RawDescriptor+8)!=D->SmemBase||
       Early16(R->RawDescriptor+16)!=D->ItemCount||Early16(R->RawDescriptor+18)!=D->TlvCount||
       D->RegionMatchesKnownWindow!=(D->SmemBase==PIANO_SMEM_BASE&&D->SmemBytes==PIANO_SMEM_BYTES)||
       CompareMem(D->Prefix,R->RawDescriptor,MIN((UINT32)sizeof(D->Prefix),Bytes))||
       !EarlyZero(D->Prefix+MIN((UINT32)sizeof(D->Prefix),Bytes),sizeof(D->Prefix)-MIN((UINT32)sizeof(D->Prefix),Bytes)))return FALSE;
    // Validate serialized boundaries and report linkage only. The SIII region
    // contents never replace the fixed address permission above.
    UINT32 At=20;UINT16 HostBytes=0,Hosts=0;
    for(UINT16 I=0;I<D->TlvCount;++I){
      if(At>Bytes||Bytes-At<4)return FALSE;
      UINT16 Length=Early16(R->RawDescriptor+At+2);
      if(Length<4||(Length&3)||Length>Bytes-At)return FALSE;
      if(Early16(R->RawDescriptor+At)==0x4853){if(!Hosts)HostBytes=Length;++Hosts;}
      At+=Length;
    }
    if(At!=Bytes||HostBytes!=D->HostInfoBytes||
       D->Status!=(Hosts>1?EFI_COMPROMISED_DATA:EFI_SUCCESS))return FALSE;
  }else if(R->RawDescriptorBytes||R->RawDescriptorCrc32||D->RepeatedEqual||D->SnapshotBytes||D->Crc32||
    D->SmemBase||D->SmemBytes||D->ItemCount||D->TlvCount||D->HostInfoBytes||
    D->RegionMatchesKnownWindow||!EarlyZero(D->Prefix,sizeof(D->Prefix)))return FALSE;
  return TRUE;
}
STATIC BOOLEAN EarlyValid(CONST PIANO_EARLY_MEMORY_REPORT *R){
  if(R->Version!=PIANO_EARLY_MEMORY_VERSION||R->Bytes!=sizeof(*R)||R->Reserved||
     R->ReportCrc32!=PianoEarlyMemoryReportCrc32(R)||!R->Attempted||!R->Finished||!R->Published||
     R->PublishStatus!=EFI_SUCCESS||(R->Status!=EFI_SUCCESS&&!EFI_ERROR(R->Status))||
     !EarlyBoolean(R->Attempted)||!EarlyBoolean(R->Finished)||!EarlyBoolean(R->Published)||
     !EarlyBoolean(R->ColdStateVerified)||R->MemoryOwnershipGranted||R->HighDdrPublished||
     R->LoadCount>PIANO_SMEM_TOTAL_MAX/4||R->RecoveredFaults>R->LoadCount||!EarlyRawValid(R))return FALSE;
  if(R->ColdStateVerified&&(R->EntryEl!=4||R->EntrySpSel!=1||(R->EntrySctlr&(BIT0|BIT2))||
     !R->EntryVbar||(R->EntryVbar&2047)))return FALSE;
  if(!R->ColdStateVerified&&(R->LoadCount||R->RecoveredFaults||R->Status==EFI_SUCCESS))return FALSE;
  if(R->RecoveredFaults){CONST PIANO_SEC_READ_STATE *F=&R->LastFault;
    if(F->Faulted!=1||F->Fatal||!F->Elr||F->Elr>MAX_UINT64-4||F->Resume!=F->Elr+4||
       F->Address!=F->Far||((F->Esr>>26)&63)!=0x25||!(F->Esr&BIT25)||
       (F->Esr&(BIT6|BIT10))||(F->Spsr&15)!=5||
       !((F->Far>=PIANO_SMEM_BASE&&F->Far<PIANO_SMEM_BASE+PIANO_SMEM_BYTES)||
         F->Far==PIANO_SMEM_COOKIE_LOW||F->Far==PIANO_SMEM_COOKIE_HIGH))return FALSE;
  }
  CONST PIANO_SMEM_RAM_REPORT *S=&R->Smem;
  if(S->BankCount>PIANO_SMEM_RAM_MAX||S->PreloadedCount>PIANO_SMEM_RAM_MAX||
     S->RawEntryCount>PIANO_SMEM_RAM_MAX||S->ReadCalls>PIANO_SMEM_CALLS_MAX||
     S->ReadBytes>PIANO_SMEM_TOTAL_MAX||S->PayloadBytes>PIANO_SMEM_PAYLOAD_MAX||
     S->MetadataBytes>PIANO_SMEM_TRACE_MAX||!EarlyBoolean(S->Parsed)||
     (UINT32)S->Reason>PianoSmemReasonRamOverlap||
     !EarlyBoolean(S->RepeatedMetadataEqual)||!EarlyBoolean(S->RepeatedPayloadEqual)||
     !EarlyBoolean(S->CookieRepeatedEqual))return FALSE;
  if(!S->Parsed)return R->Status!=EFI_SUCCESS&&S->Status!=EFI_SUCCESS&&!S->BankCount&&!S->PreloadedCount&&!S->OtherCategoryCount;
  if(R->Status!=EFI_SUCCESS||!R->ColdStateVerified||S->Status!=EFI_SUCCESS||
     S->Reason!=PianoSmemReasonNone||(S->RamVersion!=1&&S->RamVersion!=2&&S->RamVersion!=3)||
     (!S->BankCount&&S->RamVersion!=3)||!S->RepeatedMetadataEqual||!S->RepeatedPayloadEqual||
     S->BankCount+S->PreloadedCount+S->OtherCategoryCount!=S->RawEntryCount||
     (S->SmemVersion>>16!=11&&S->SmemVersion>>16!=12)||
     S->PayloadAddress<PIANO_SMEM_BASE||S->PayloadAddress-PIANO_SMEM_BASE>=PIANO_SMEM_BYTES||
     !S->PayloadBytes||S->PayloadBytes>PIANO_SMEM_BYTES-(S->PayloadAddress-PIANO_SMEM_BASE))return FALSE;
  if(S->RamVersion==3){
    // Reproduce the exact pure observation model from the already CRC-checked
    // local bytes. This validates current slices, not DDR/map/allocation rights.
    EFI_STATUS Parsed=PianoSmemRamParse(R->RawPayload,R->RawPayloadBytes,&mV3Check);
    BOOLEAN Equal=Parsed==EFI_SUCCESS&&mV3Check.RamVersion==3&&mV3Check.RawEntryCount==S->RawEntryCount&&
      mV3Check.BankCount==S->BankCount&&mV3Check.PreloadedCount==S->PreloadedCount&&
      mV3Check.OtherCategoryCount==S->OtherCategoryCount&&
      !CompareMem(mV3Check.Banks,S->Banks,sizeof(S->Banks))&&
      !CompareMem(mV3Check.Preloaded,S->Preloaded,sizeof(S->Preloaded));
    ZeroMem(&mV3Check,sizeof(mV3Check));return Equal;
  }
  for(UINT32 I=0;I<S->BankCount;++I){CONST PIANO_SMEM_RAM_ENTRY *E=&S->Banks[I];
    if(!EarlySpan(E->Base,E->RawSize)||!E->AvailableLength||E->AvailableLength>E->RawSize||
       E->RawType!=1||E->SourceIndex>=S->RawEntryCount)return FALSE;
    for(UINT32 J=0;J<I;++J)if(E->Base<S->Banks[J].Base+S->Banks[J].RawSize&&
       S->Banks[J].Base<E->Base+E->RawSize)return FALSE;
  }
  for(UINT32 I=0;I<S->PreloadedCount;++I){CONST PIANO_SMEM_RAM_ENTRY *E=&S->Preloaded[I];
    if(!EarlySpan(E->Base,E->RawSize)||E->AvailableLength||E->RawType<5||
       E->RawType>(S->RamVersion==1?8U:9U)||E->SourceIndex>=S->RawEntryCount)return FALSE;
  }
  return TRUE;
}
STATIC VOID CaptureEarly(VOID){
  VOID *Hob=GetFirstGuidHob(&mEarlyGuid);mEarlyStatus=EFI_NOT_FOUND;
  if(!Hob)return;
  EFI_HOB_GUID_TYPE *G=Hob;
  if(G->Header.HobType!=EFI_HOB_TYPE_GUID_EXTENSION||G->Header.Reserved||
     G->Header.HobLength!=sizeof(*G)+sizeof(mEarly)||
     CompareMem(&G->Name,&mEarlyGuid,sizeof(mEarlyGuid))){mEarlyStatus=EFI_COMPROMISED_DATA;return;}
  if(GetNextGuidHob(&mEarlyGuid,(VOID *)((UINTN)G+G->Header.HobLength))){mEarlyStatus=EFI_COMPROMISED_DATA;return;}
  CopyMem(&mEarly,GET_GUID_HOB_DATA(G),sizeof(mEarly));
  CopyMem(&mEarlyScratch,GET_GUID_HOB_DATA(G),sizeof(mEarlyScratch));
  if(CompareMem(&mEarly,&mEarlyScratch,sizeof(mEarly))||!EarlyValid(&mEarly)){
    ZeroMem(&mEarly,sizeof(mEarly));mEarlyStatus=EFI_COMPROMISED_DATA;
  }else mEarlyStatus=EFI_SUCCESS;
  ZeroMem(&mEarlyScratch,sizeof(mEarlyScratch));
}
STATIC VOID ReemitEarly(VOID){
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_HOB status=%r version=%u bytes=%u crc32=%08x ownership=0 high_ddr=0\n",
    mEarlyStatus,mEarly.Version,mEarly.Bytes,mEarly.ReportCrc32));
  if(mEarlyStatus!=EFI_SUCCESS)return;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_STATE status=%r el=%lu sctlr=%lx mmu=%u dcache=%u spsel=%lu vbar=%lx daif=%lx cold=%u loads=%u recovered=%u\n",
    mEarly.Status,mEarly.EntryEl>>2,mEarly.EntrySctlr,(UINT32)(mEarly.EntrySctlr&1),
    (UINT32)((mEarly.EntrySctlr>>2)&1),mEarly.EntrySpSel,mEarly.EntryVbar,mEarly.EntryDaif,
    mEarly.ColdStateVerified,mEarly.LoadCount,mEarly.RecoveredFaults));
  CONST PIANO_SMEM_RAM_REPORT *S=&mEarly.Smem;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_SMEM status=%r parsed=%u major=%u ramver=%u reason=%u payload=%lx bytes=%u crc32=%08x\n",
    S->Status,S->Parsed,S->SmemVersion>>16,S->RamVersion,S->Reason,S->PayloadAddress,S->PayloadBytes,S->PayloadCrc32));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_COUNTS raw=%u banks=%u preloaded=%u other_category=%u reads=%u bytes=%u\n",
    S->RawEntryCount,S->BankCount,S->PreloadedCount,S->OtherCategoryCount,S->ReadCalls,S->ReadBytes));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_COOKIE status=%r value=%lx stable=%u metadata_equal=%u payload_equal=%u\n",
    S->CookieStatus,S->CookieValue,S->CookieRepeatedEqual,S->RepeatedMetadataEqual,S->RepeatedPayloadEqual));
  for(UINT32 I=0;I<S->BankCount&&S->Parsed;++I){CONST PIANO_SMEM_RAM_ENTRY *E=&S->Banks[I];
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_BANK index=%u category=14 type=%u base=%lx raw_size=%lx available=%lx view=%a authority=0\n",
      E->SourceIndex,E->RawType,E->Base,E->RawSize,E->AvailableLength,S->RamVersion==3?"native-current":"strict-legacy"));}
  for(UINT32 I=0;I<S->PreloadedCount&&S->Parsed;++I){CONST PIANO_SMEM_RAM_ENTRY *E=&S->Preloaded[I];
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_PRELOADED index=%u category=14 type=%u base=%lx size=%lx\n",
      E->SourceIndex,E->RawType,E->Base,E->RawSize));}
  if(mEarly.RecoveredFaults)DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM_FAULT pc=%lx esr=%lx far=%lx spsr=%lx resume=%lx\n",
    mEarly.LastFault.Elr,mEarly.LastFault.Esr,mEarly.LastFault.Far,mEarly.LastFault.Spsr,mEarly.LastFault.Resume));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAW ram_bytes=%u ram_crc32=%08x ram_equal=%u siii_bytes=%u siii_crc32=%08x siii_equal=%u ownership=0 high_ddr=0\n",
    mEarly.RawPayloadBytes,mEarly.RawPayloadCrc32,mEarly.RawPayloadCoherent,
    mEarly.RawDescriptorBytes,mEarly.RawDescriptorCrc32,mEarly.RawDescriptorCoherent));
  if(mEarly.RawPayloadCoherent)for(UINT32 At=0;At<mEarly.RawPayloadBytes;At+=24){
    UINT32 Words[6]={0};UINT32 Bytes=MIN(24U,mEarly.RawPayloadBytes-At);CopyMem(Words,mEarly.RawPayload+At,Bytes);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_RAM402_RAW off=%x bytes=%u words=%08x,%08x,%08x,%08x,%08x,%08x\n",
      At,Bytes,Words[0],Words[1],Words[2],Words[3],Words[4],Words[5]));
  }
  CONST PIANO_EARLY_SIII_DIAGNOSTIC *D=&mEarly.Descriptor;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_SIII status=%r address=%lx base=%lx bytes=%x items=%u tlvs=%u hostlen=%u snapshot=%u crc32=%08x equal=%u window_match=%u ownership=0\n",
    D->Status,D->Address,D->SmemBase,D->SmemBytes,D->ItemCount,D->TlvCount,D->HostInfoBytes,
    D->SnapshotBytes,D->Crc32,D->RepeatedEqual,D->RegionMatchesKnownWindow));
  if(mEarly.RawDescriptorCoherent)for(UINT32 At=0;At<mEarly.RawDescriptorBytes;At+=24){
    UINT32 Words[6]={0};UINT32 Bytes=MIN(24U,mEarly.RawDescriptorBytes-At);CopyMem(Words,mEarly.RawDescriptor+At,Bytes);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_EARLY_SIII_RAW off=%x bytes=%u words=%08x,%08x,%08x,%08x,%08x,%08x\n",
      At,Bytes,Words[0],Words[1],Words[2],Words[3],Words[4],Words[5]));
  }
}
EFI_STATUS PianoProductEarlySmemStatus(VOID){return mEarlyStatus;}

STATIC BOOLEAN ServicesAlive(VOID *Context) {
  // The guard owns its actual EBS event. This additional CPU-only check is
  // independent of BootPolicy/GUI runtime, which is not initialized yet.
  return Context==mBoot && mPhase && mBoot && gBS==mBoot && gST &&
    gST->BootServices==mBoot && mBoot->Hdr.Signature==EFI_BOOT_SERVICES_SIGNATURE &&
    mBoot->Hdr.HeaderSize>=sizeof(EFI_BOOT_SERVICES);
}
BOOLEAN PianoProductSmemRetained(VOID) { return mRetained; }

EFI_STATUS PianoProductObserveSmem(VOID) {
  if(mAttempted)return EFI_ALREADY_STARTED;
  mAttempted=TRUE;mPhase=TRUE;mBoot=gBS;
  CaptureEarly();ReemitEarly();
  PIANO_GUARDED_CONFIG Config={
    .Context=mBoot,.Services=gBS,.DxeServices=gDS,.BootServicesAlive=ServicesAlive,
    // Exact product MemoryMapLib sources: SMEM is reserved/UC and normal NC;
    // the two TCSR cookies lie in the named NS_DEVICE MMIO region. Neither
    // a cookie nor an item value can extend these trusted read ranges.
    .Ranges={{PIANO_SMEM_BASE,PIANO_SMEM_BYTES,EfiGcdMemoryTypeReserved,EFI_MEMORY_UC,0x44},
      {PIANO_SMEM_COOKIE_LOW,8,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},
    .RangeCount=2,.MaxReads=PIANO_GUARDED_READ_MAX,.MaxUsecs=PIANO_GUARDED_USECS_MAX};
  VOID *Context=NULL;EFI_STATUS Status=PianoGuardedReadBegin(&Config,&Context);
  CONST PIANO_GUARDED_REPORT *Guard=PianoGuardedReadReport();
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_GUARD status=%r pages=%u reads=%u retained=%u\n",
    Status,Guard->PagesValidated,Guard->Reads,Guard->Retained));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_MAP page=%lx par=%lx status=%r\n",
    Guard->LastMappingPage,Guard->LastPar,Guard->MappingStatus));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_GCD type=%u attrs=%lx\n",
    Guard->LastGcdType,Guard->LastGcdAttributes));
  if(Status==EFI_SUCCESS) {
    CONST PIANO_SMEM_READER Reader={Context,PianoGuardedTryRead,
      PIANO_SMEM_CALLS_MAX,PIANO_SMEM_TOTAL_MAX};
    Status=PianoSmemRamCollect(&Reader,&mWork,&mSmem);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_PAYLOAD status=%r parsed=%u major=%u ramver=%u reason=%u\n",
      Status,mSmem.Parsed,mSmem.SmemVersion>>16,mSmem.RamVersion,mSmem.Reason));
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_COUNTS banks=%u preloaded=%u bytes=%u reads=%u\n",
      mSmem.BankCount,mSmem.PreloadedCount,mSmem.PayloadBytes,mSmem.ReadCalls));
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_COHERENCE metadata=%u payload=%u crc=%08x map_authorized=0\n",
      mSmem.RepeatedMetadataEqual,mSmem.RepeatedPayloadEqual,mSmem.PayloadCrc32));
    if(Status==EFI_SUCCESS && mSmem.Parsed) {
      for(UINT32 I=0;I<mSmem.BankCount;++I)
        DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_BANK i=%u base=%lx size=%lx available=%lx view=%a authority=0\n",
          I,mSmem.Banks[I].Base,mSmem.Banks[I].RawSize,mSmem.Banks[I].AvailableLength,mSmem.RamVersion==3?"native-current":"strict-legacy"));
      for(UINT32 I=0;I<mSmem.PreloadedCount;++I)
        DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_PRELOADED i=%u base=%lx size=%lx type=%u\n",
          I,mSmem.Preloaded[I].Base,mSmem.Preloaded[I].RawSize,mSmem.Preloaded[I].RawType));
    }
    EFI_STATUS Close=PianoGuardedReadEnd(Context);
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_CLOSE status=%r recovered=%u retained=%u handlers=%u/%u\n",
      Close,Guard->RecoveredFaults,Guard->Retained,Guard->SyncOwned,Guard->SErrorOwned));
    if(Close!=EFI_SUCCESS)Status=Close;
  }
  mRetained=Guard->Retained || Guard->ServicesLost || Guard->Fatal ||
    Guard->SyncOwned || Guard->SErrorOwned;
  if(Guard->RecoveredFaults || Guard->Retained)
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_FAULT pc=%lx esr=%lx far=%lx retained=%u\n",
      Guard->Elr,Guard->Esr,Guard->Far,Guard->Retained));
  CopyMem(&mGuardSnapshot,Guard,sizeof(mGuardSnapshot));mObservationStatus=Status;
  mPhase=FALSE;
  return Status;
}

EFI_STATUS PianoProductSmemReemit(VOID *Context) {
  if(Context!=NULL)return EFI_INVALID_PARAMETER;
  if(!mAttempted || mPhase)return EFI_NOT_READY;
  if(mRetained)return EFI_ABORTED;
  ReemitEarly();
  // This is our saved report, not the mutable singleton guard report which a
  // later safe-read session may replace. No native/BS/device callback occurs.
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_SNAPSHOT status=%r parsed=%u major=%u ramver=%u reason=%u\n",
    mObservationStatus,mSmem.Parsed,mSmem.SmemVersion>>16,mSmem.RamVersion,mSmem.Reason));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_MAP page=%lx par=%lx status=%r\n",
    mGuardSnapshot.LastMappingPage,mGuardSnapshot.LastPar,mGuardSnapshot.MappingStatus));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_GCD type=%u attrs=%lx\n",
    mGuardSnapshot.LastGcdType,mGuardSnapshot.LastGcdAttributes));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_COUNTS banks=%u preloaded=%u bytes=%u reads=%u\n",
    mSmem.BankCount,mSmem.PreloadedCount,mSmem.PayloadBytes,mSmem.ReadCalls));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_COHERENCE metadata=%u payload=%u crc=%08x map_authorized=0\n",
    mSmem.RepeatedMetadataEqual,mSmem.RepeatedPayloadEqual,mSmem.PayloadCrc32));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_COOKIE status=%r value=%lx stable=%u\n",
    mSmem.CookieStatus,mSmem.CookieValue,mSmem.CookieRepeatedEqual));
  for(UINT32 I=0;I<mSmem.BankCount && mSmem.Parsed;++I)
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_BANK i=%u base=%lx size=%lx available=%lx view=%a authority=0\n",
      I,mSmem.Banks[I].Base,mSmem.Banks[I].RawSize,mSmem.Banks[I].AvailableLength,mSmem.RamVersion==3?"native-current":"strict-legacy"));
  for(UINT32 I=0;I<mSmem.PreloadedCount && mSmem.Parsed;++I)
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_PRELOADED i=%u base=%lx size=%lx type=%u\n",
      I,mSmem.Preloaded[I].Base,mSmem.Preloaded[I].RawSize,mSmem.Preloaded[I].RawType));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_FAULT pc=%lx esr=%lx far=%lx recovered=%u\n",
    mGuardSnapshot.Elr,mGuardSnapshot.Esr,mGuardSnapshot.Far,mGuardSnapshot.RecoveredFaults));
  return EFI_SUCCESS;
}
