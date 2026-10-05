// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductSmem.h"
#include "PianoSmemRam.h"
#include "PianoGuardedRead.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>

STATIC PIANO_SMEM_RAM_WORK mWork;
STATIC PIANO_SMEM_RAM_REPORT mSmem={.Status=EFI_NOT_STARTED,.CookieStatus=EFI_NOT_READY};
STATIC PIANO_GUARDED_REPORT mGuardSnapshot;
STATIC EFI_STATUS mObservationStatus=EFI_NOT_STARTED;
STATIC EFI_BOOT_SERVICES *mBoot;
STATIC BOOLEAN mPhase, mAttempted, mRetained;

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
        DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_BANK i=%u base=%lx size=%lx available=%lx\n",
          I,mSmem.Banks[I].Base,mSmem.Banks[I].RawSize,mSmem.Banks[I].AvailableLength));
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
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_BANK i=%u base=%lx size=%lx available=%lx\n",
      I,mSmem.Banks[I].Base,mSmem.Banks[I].RawSize,mSmem.Banks[I].AvailableLength));
  for(UINT32 I=0;I<mSmem.PreloadedCount && mSmem.Parsed;++I)
    DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_PRELOADED i=%u base=%lx size=%lx type=%u\n",
      I,mSmem.Preloaded[I].Base,mSmem.Preloaded[I].RawSize,mSmem.Preloaded[I].RawType));
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_SMEM_FAULT pc=%lx esr=%lx far=%lx recovered=%u\n",
    mGuardSnapshot.Elr,mGuardSnapshot.Esr,mGuardSnapshot.Far,mGuardSnapshot.RecoveredFaults));
  return EFI_SUCCESS;
}
