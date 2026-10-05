// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real SEC observer: no BS/DXE service, target write, MMU or DDR publication.
#include "PianoEarlyMemory.h"
#include <PiPei.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>

STATIC_ASSERT(OFFSET_OF(PIANO_SEC_READ_STATE,Address)==24,"SEC ASM address");
STATIC_ASSERT(OFFSET_OF(PIANO_SEC_READ_STATE,OldDaif)==80,"SEC ASM DAIF");
STATIC_ASSERT(sizeof(PIANO_SEC_READ_STATE)==88,"SEC ASM state size");
STATIC PIANO_SMEM_RAM_WORK mWork;
STATIC PIANO_EARLY_MEMORY_REPORT mReport;
STATIC UINT32 mScratch[PIANO_SMEM_READ_MAX/4];
STATIC UINT64 mStart,mFrequency;
STATIC EFI_GUID mHobGuid=PIANO_EARLY_MEMORY_HOB_GUID;

STATIC EFI_STATUS CpuState(UINT64 *El,UINT64 *Sctlr,UINT64 *Vbar,UINT64 *Daif,
  UINT64 *Counter,UINT64 *Frequency,UINT64 *SpSel){
#ifdef PIANO_EARLY_HOST_TEST
  extern EFI_STATUS PianoEarlyHostCpu(UINT64 *,UINT64 *,UINT64 *,UINT64 *,UINT64 *,UINT64 *,UINT64 *);
  return PianoEarlyHostCpu(El,Sctlr,Vbar,Daif,Counter,Frequency,SpSel);
#elif defined(__aarch64__)
  __asm__ volatile("mrs %0, CurrentEL":"=r"(*El));
  if(*El!=4)return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, vbar_el1\n mrs %2, daif\n mrs %3, cntvct_el0\n mrs %4, cntfrq_el0\n mrs %5, SPSel"
    :"=r"(*Sctlr),"=r"(*Vbar),"=r"(*Daif),"=r"(*Counter),"=r"(*Frequency),"=r"(*SpSel)::"memory");
  return EFI_SUCCESS;
#else
  (VOID)El;(VOID)Sctlr;(VOID)Vbar;(VOID)Daif;(VOID)Counter;(VOID)Frequency;(VOID)SpSel;
  return EFI_UNSUPPORTED;
#endif
}
STATIC EFI_STATUS Fresh(VOID){
  UINT64 El=0,Sctlr=0,Vbar=0,Daif=0,Now=0,Frequency=0,SpSel=0;
  EFI_STATUS S=CpuState(&El,&Sctlr,&Vbar,&Daif,&Now,&Frequency,&SpSel);
  if(S!=EFI_SUCCESS)return S;
  // This adapter is usable only before the first MMU setup. Neither a late
  // DXE caller nor a second cold initialization can acquire read permission.
  if(El!=4||SpSel!=1||(Sctlr&(BIT0|BIT2))||Sctlr!=mReport.EntrySctlr||
     Vbar!=mReport.EntryVbar||Daif!=mReport.EntryDaif||Frequency!=mFrequency)
    return EFI_NOT_READY;
  if(!Frequency||Frequency>1000000000ULL||Now<mStart||
     Now-mStart>=Frequency/10||mReport.LoadCount>=PIANO_SMEM_TOTAL_MAX/4)
    return EFI_TIMEOUT;
  return EFI_SUCCESS;
}
STATIC BOOLEAN Allowed(UINT64 Address,UINTN Bytes){
  return (Address>=PIANO_SMEM_BASE&&Address-PIANO_SMEM_BASE<PIANO_SMEM_BYTES&&
     Bytes<=PIANO_SMEM_BYTES-(Address-PIANO_SMEM_BASE))||
    ((Address==PIANO_SMEM_COOKIE_LOW||Address==PIANO_SMEM_COOKIE_HIGH)&&Bytes==4);
}
STATIC BOOLEAN Alias(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return An>MAX_UINTN-X||Bn>MAX_UINTN-Y||(X<Y+Bn&&Y<X+An);
}
STATIC EFI_STATUS EFIAPI TryRead(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Destination){
  if(Context!=&mReport||!mReport.Attempted||mReport.Finished||
     !Destination||!Bytes||Bytes>sizeof(mScratch)||(Address&3)||(Bytes&3))return EFI_INVALID_PARAMETER;
  if(!Allowed(Address,Bytes)||Alias(Destination,Bytes,&mReport,sizeof(mReport))||
     Alias(Destination,Bytes,mScratch,sizeof(mScratch))||
     Alias(Destination,Bytes,(VOID *)(UINTN)PIANO_SMEM_BASE,PIANO_SMEM_BYTES)||
     Alias(Destination,Bytes,(VOID *)(UINTN)PIANO_SMEM_COOKIE_LOW,8))return EFI_ACCESS_DENIED;
  EFI_STATUS S=Fresh();
  for(UINTN I=0;S==EFI_SUCCESS&&I<Bytes/4;++I){
    ZeroMem(&mReport.LastRead,sizeof(mReport.LastRead));
    UINTN Result=PianoSecRead32(Address+I*4,&mScratch[I],&mReport.LastRead);
    ++mReport.LoadCount;
    if(Result==1){
      ++mReport.RecoveredFaults;CopyMem(&mReport.LastFault,&mReport.LastRead,sizeof(mReport.LastFault));
      DEBUG((DEBUG_WARN,"SUNUEFI_EARLY_SMEM_READ_ABORT addr=%lx elr=%lx esr=%lx far=%lx spsr=%lx resume=%lx\n",
        Address+I*4,mReport.LastFault.Elr,mReport.LastFault.Esr,mReport.LastFault.Far,
        mReport.LastFault.Spsr,mReport.LastFault.Resume));
      S=EFI_NOT_READY;
    }
    else if(Result!=0)S=EFI_UNSUPPORTED;
    if(S==EFI_SUCCESS)S=Fresh();
  }
  if(S==EFI_SUCCESS)CopyMem(Destination,mScratch,Bytes);
  ZeroMem(mScratch,sizeof(mScratch));return S;
}
EFI_STATUS PianoEarlyMemoryObserveCold(VOID){
  if(mReport.Attempted)return EFI_ALREADY_STARTED;
  ZeroMem(&mReport,sizeof(mReport));mReport.Version=PIANO_EARLY_MEMORY_VERSION;
  mReport.Bytes=sizeof(mReport);mReport.Attempted=TRUE;mReport.PublishStatus=EFI_NOT_READY;
  mReport.Smem.Status=EFI_NOT_STARTED;mReport.Smem.CookieStatus=EFI_NOT_READY;
  EFI_STATUS S=CpuState(&mReport.EntryEl,&mReport.EntrySctlr,&mReport.EntryVbar,
    &mReport.EntryDaif,&mStart,&mFrequency,&mReport.EntrySpSel);
  if(S==EFI_SUCCESS){
    if(mReport.EntryEl!=4||mReport.EntrySpSel!=1||(mReport.EntrySctlr&(BIT0|BIT2))||
       !mReport.EntryVbar||(mReport.EntryVbar&2047))S=EFI_UNSUPPORTED;
    else S=Fresh();
  }
  if(S==EFI_SUCCESS){
    mReport.ColdStateVerified=TRUE;
    CONST PIANO_SMEM_READER Reader={&mReport,TryRead,PIANO_SMEM_CALLS_MAX,PIANO_SMEM_TOTAL_MAX};
    S=PianoSmemRamCollect(&Reader,&mWork,&mReport.Smem);
  }
  mReport.Status=S;mReport.Finished=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_EARLY_SMEM status=%r cold=%u loads=%u recovered=%u smem=%x ram=%u banks=%u preloaded=%u cookie=%lx ownership=0 high_ddr=0\n",
    S,mReport.ColdStateVerified,mReport.LoadCount,mReport.RecoveredFaults,
    mReport.Smem.SmemVersion,mReport.Smem.RamVersion,mReport.Smem.BankCount,
    mReport.Smem.PreloadedCount,mReport.Smem.CookieValue));
  DEBUG((DEBUG_WARN,"SUNUEFI_EARLY_SMEM_SNAPSHOT payload=%lx bytes=%u crc32=%x metadata=%u metadata_equal=%u payload_equal=%u cookie_status=%r cookie_equal=%u\n",
    mReport.Smem.PayloadAddress,mReport.Smem.PayloadBytes,mReport.Smem.PayloadCrc32,
    mReport.Smem.MetadataBytes,mReport.Smem.RepeatedMetadataEqual,mReport.Smem.RepeatedPayloadEqual,
    mReport.Smem.CookieStatus,mReport.Smem.CookieRepeatedEqual));
  for(UINT32 I=0;I<mReport.Smem.BankCount;++I){
    CONST PIANO_SMEM_RAM_ENTRY *E=&mReport.Smem.Banks[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_EARLY_SMEM_BANK index=%u base=%lx raw_size=%lx available=%lx type=%u\n",
      E->SourceIndex,E->Base,E->RawSize,E->AvailableLength,E->RawType));
  }
  for(UINT32 I=0;I<mReport.Smem.PreloadedCount;++I){
    CONST PIANO_SMEM_RAM_ENTRY *E=&mReport.Smem.Preloaded[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_EARLY_SMEM_PRELOADED index=%u base=%lx size=%lx type=%u\n",
      E->SourceIndex,E->Base,E->RawSize,E->RawType));
  }
  return S;
}
EFI_STATUS PianoEarlyMemoryPublishHob(VOID){
  if(!mReport.Attempted||!mReport.Finished||mReport.Published)return EFI_NOT_READY;
  // HOB creation is the only allocation here and happens AFTER the genuine
  // SEC PHIT constructor. Inaccessible SMEM is valid diagnostic failure data.
  VOID *Data=BuildGuidHob(&mHobGuid,sizeof(mReport));
  if(!Data)return mReport.PublishStatus=EFI_OUT_OF_RESOURCES;
  mReport.Published=TRUE;mReport.PublishStatus=EFI_SUCCESS;
  mReport.ReportCrc32=PianoEarlyMemoryReportCrc32(&mReport);
  CopyMem(Data,&mReport,sizeof(mReport));
  return EFI_SUCCESS;
}
CONST PIANO_EARLY_MEMORY_REPORT *PianoEarlyMemoryReport(VOID){return &mReport;}
VOID EFIAPI PianoSecReadFatal(PIANO_SEC_READ_STATE *State){
  State->Fatal=1;
  DEBUG((DEBUG_ERROR,"SUNUEFI_EARLY_SMEM_FATAL elr=%lx esr=%lx far=%lx spsr=%lx resume=%lx\n",
    State->Elr,State->Esr,State->Far,State->Spsr,State->Resume));
#ifdef PIANO_EARLY_HOST_TEST
  extern VOID PianoEarlyHostFatal(VOID);PianoEarlyHostFatal();
#elif defined(__aarch64__)
  __asm__ volatile("msr daifset, #15\n movz x0, #9\n movk x0, #0x8400, lsl #16\n smc #0":::"x0","x1","x2","x3","memory");
#endif
  CpuDeadLoop();while(TRUE){}
}
