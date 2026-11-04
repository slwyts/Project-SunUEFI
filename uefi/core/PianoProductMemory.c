// SPDX-License-Identifier: BSD-2-Clause-Patent
// Validate the real SEC-published map. Nothing here expands an allocator,
// interprets ESP /memory as physical RAM, or changes live descriptors/PTEs.
#include "PianoProductMemory.h"
#include "PianoColdMemory.h"
#include "PianoColdBootObjects.h"
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/HobLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
STATIC PIANO_COLD_MEMORY_REPORT mCold;
STATIC EFI_GUID mGuid=PIANO_COLD_MEMORY_HOB_GUID;
STATIC EFI_STATUS mCapture=EFI_NOT_STARTED;
STATIC CONST CHAR8 *mCaptureStage="not-started",*mDtbStage="not-started";
STATIC EFI_STATUS mDtbStatus=EFI_NOT_STARTED;
STATIC UINTN mCaptureIndex=MAX_UINTN,mDtbIndex=MAX_UINTN;
STATIC BOOLEAN mBusy;
STATIC UINT64 mMapBuffer[8192];
STATIC UINTN mMapBytes,mMapStride;
STATIC BOOLEAN Span(UINT64 Base,UINT64 Bytes){return Bytes&&Base<=MAX_UINT64-Bytes;}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 N,UINT64 B,UINT64 Z){return A<B+Z&&B<A+N;}
STATIC BOOLEAN Reclaimable(UINT32 Type){return Type==EfiLoaderCode||Type==EfiLoaderData||Type==EfiBootServicesCode||Type==EfiBootServicesData||Type==EfiConventionalMemory||Type==EfiACPIReclaimMemory;}
STATIC BOOLEAN Live(VOID){return gBS&&gST&&gST->BootServices==gBS&&gDS&&gBS->GetMemoryMap&&gDS->GetMemorySpaceDescriptor;}
STATIC BOOLEAN LayoutCovers(CONST PIANO_MEMORY_LAYOUT *L,UINT64 Base,UINT64 Bytes){
 if(!Span(Base,Bytes))return FALSE;UINT64 End=Base+Bytes;
 while(Base<End){UINT64 Next=Base;for(UINTN I=0;I<L->RamCount;++I){CONST PIANO_MEMORY_SPAN *R=&L->Ram[I];if(Span(R->Base,R->Bytes)&&Base>=R->Base&&Base-R->Base<R->Bytes&&R->Base+R->Bytes>Next)Next=R->Base+R->Bytes;}if(Next==Base)return FALSE;Base=MIN(Next,End);}return TRUE;
}
STATIC EFI_STATUS CaptureResult(CONST CHAR8 *Stage,EFI_STATUS Status){
 mCaptureStage=Stage;
 mCapture=Status;
 return Status;
}

STATIC EFI_STATUS Capture(VOID){
 if(mCapture!=EFI_NOT_STARTED)return mCapture;
 mCaptureIndex=MAX_UINTN;
 mCapture=EFI_NOT_FOUND;
 mCaptureStage="cold-hob";

 VOID *Hob=GetFirstGuidHob(&mGuid);
 if(!Hob)return mCapture;
 EFI_HOB_GUID_TYPE *G=Hob;
 if(G->Header.HobLength!=sizeof(*G)+sizeof(mCold)||G->Header.Reserved||
    GetNextGuidHob(&mGuid,(UINT8 *)G+G->Header.HobLength))
  return CaptureResult("cold-hob-shape",EFI_COMPROMISED_DATA);

 CopyMem(&mCold,GET_GUID_HOB_DATA(G),sizeof(mCold));
 // Preserve the existing rejection and its order; retain the first failing
 // condition so status works even when the RAM console has no valid header.
 if(mCold.Version!=1)return CaptureResult("cold-version",EFI_NOT_READY);
 if(mCold.Bytes!=sizeof(mCold))return CaptureResult("cold-bytes",EFI_NOT_READY);
 if(mCold.Reserved)return CaptureResult("cold-reserved",EFI_NOT_READY);
 if(mCold.Crc32!=PianoColdMemoryCrc(&mCold))return CaptureResult("cold-crc",EFI_NOT_READY);
 if(mCold.PublishStatus!=EFI_SUCCESS)return CaptureResult("cold-publish",EFI_NOT_READY);
 if(mCold.Status!=EFI_SUCCESS)return CaptureResult("cold-status",EFI_NOT_READY);
 if(!mCold.BootEpoch)return CaptureResult("cold-epoch",EFI_NOT_READY);
 if(!mCold.InputFingerprint)return CaptureResult("cold-fingerprint",EFI_NOT_READY);
 if(!mCold.NativeCount||mCold.NativeCount>mCold.Count||mCold.Count>=MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT)
  return CaptureResult("cold-counts",EFI_NOT_READY);
 if(!mCold.Layout.RamCount||mCold.Layout.RamCount>PIANO_MEMORY_LAYOUT_MAX_RAM||
    mCold.Layout.ReservedCount>PIANO_MEMORY_LAYOUT_MAX_RESERVED)
  return CaptureResult("cold-layout-counts",EFI_NOT_READY);
 if(mCold.Layout.Reserved)return CaptureResult("cold-layout-reserved",EFI_NOT_READY);
 if(!mCold.Layout.DramBytes)return CaptureResult("cold-ram-bytes",EFI_NOT_READY);

 // The independent cold object HOB identifies the very same BootShim entry.
 EFI_GUID ObjectsGuid=PIANO_COLD_OBJECT_HOB_GUID;
 VOID *ObjectHob=GetFirstGuidHob(&ObjectsGuid);
 if(!ObjectHob)return CaptureResult("objects-hob",EFI_NOT_READY);
 EFI_HOB_GUID_TYPE *O=ObjectHob;
 if(O->Header.HobLength!=sizeof(*O)+sizeof(PIANO_COLD_BOOT_OBJECT_REPORT)||
    GetNextGuidHob(&ObjectsGuid,(UINT8 *)O+O->Header.HobLength))
  return CaptureResult("objects-hob-shape",EFI_COMPROMISED_DATA);

 CONST PIANO_COLD_BOOT_OBJECT_REPORT *Objects=GET_GUID_HOB_DATA(O);
 if(PianoColdBootObjectsValidate(Objects)!=EFI_SUCCESS)return CaptureResult("objects-validation",EFI_NOT_READY);
 if(!Objects->Published)return CaptureResult("objects-published",EFI_NOT_READY);
 if(Objects->PublishStatus!=EFI_SUCCESS)return CaptureResult("objects-publish",EFI_NOT_READY);
 if(Objects->Epoch!=mCold.BootEpoch)return CaptureResult("objects-epoch",EFI_NOT_READY);

 for(UINTN I=0;I<mCold.Count;++I){
  CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];
  if(!Span(R->Address,R->Length)||(R->Address|R->Length)&4095){
   mCaptureIndex=I;
   return CaptureResult("cold-row",EFI_COMPROMISED_DATA);
  }
 }
 return CaptureResult("ready",EFI_SUCCESS);
}

STATIC EFI_STATUS DiagnosticHob(CONST EFI_GUID *Guid,UINTN Bytes,CONST VOID **Data){
 *Data=NULL;
 EFI_HOB_GUID_TYPE *Hob=GetFirstGuidHob(Guid);
 if(!Hob)return EFI_NOT_FOUND;
 if(Hob->Header.HobLength!=sizeof(*Hob)+Bytes||Hob->Header.Reserved||
    GetNextGuidHob(Guid,(UINT8 *)Hob+Hob->Header.HobLength))return EFI_COMPROMISED_DATA;
 *Data=GET_GUID_HOB_DATA(Hob);
 return EFI_SUCCESS;
}

VOID PianoProductMemoryGetDiagnostic(PIANO_PRODUCT_MEMORY_DIAGNOSTIC *Report){
 if(!Report)return;
 ZeroMem(Report,sizeof(*Report));
 Report->CaptureStage=mCaptureStage;
 Report->CaptureStatus=mCapture;
 Report->CaptureIndex=mCaptureIndex;
 Report->DtbStage=mDtbStage;
 Report->DtbStatus=mDtbStatus;
 Report->DtbIndex=mDtbIndex;

 // Inspect only existing HOB payloads. Values remain visible when their CRC
 // or producer status is bad; the companion status never grants authority.
 CONST VOID *Data;
 Report->ColdStatus=Report->ColdPublishStatus=EFI_NOT_FOUND;
 Report->ColdHobStatus=DiagnosticHob(&mGuid,sizeof(PIANO_COLD_MEMORY_REPORT),&Data);
 if(Report->ColdHobStatus==EFI_SUCCESS){
  CONST PIANO_COLD_MEMORY_REPORT *Cold=Data;
  if(Cold->Version!=1||Cold->Bytes!=sizeof(*Cold)||Cold->Reserved||
     Cold->Crc32!=PianoColdMemoryCrc(Cold))Report->ColdHobStatus=EFI_COMPROMISED_DATA;
  Report->ColdStatus=Cold->Status;
  Report->ColdPublishStatus=Cold->PublishStatus;
  Report->ColdNativeCount=Cold->NativeCount;
  Report->ColdCount=Cold->Count;
  Report->ColdRamCount=Cold->Layout.RamCount;
  Report->ColdReservedCount=Cold->Layout.ReservedCount;
  Report->ColdEpoch=Cold->BootEpoch;
  Report->ColdDramBytes=Cold->Layout.DramBytes;
  Report->ColdInputFingerprint=Cold->InputFingerprint;
 }

 EFI_GUID ObjectsGuid=PIANO_COLD_OBJECT_HOB_GUID;
 Report->ObjectsValidateStatus=Report->ObjectsStatus=Report->ObjectsPublishStatus=EFI_NOT_FOUND;
 Report->ObjectsHandoffStatus=Report->ObjectsDtbStatus=EFI_NOT_FOUND;
 Report->ObjectsHobStatus=DiagnosticHob(&ObjectsGuid,sizeof(PIANO_COLD_BOOT_OBJECT_REPORT),&Data);
 if(Report->ObjectsHobStatus==EFI_SUCCESS){
  CONST PIANO_COLD_BOOT_OBJECT_REPORT *Objects=Data;
  Report->ObjectsValidateStatus=PianoColdBootObjectsValidate(Objects);
  Report->ObjectsStatus=Objects->Status;
  Report->ObjectsPublishStatus=Objects->PublishStatus;
  Report->ObjectsHandoffStatus=Objects->HandoffStatus;
  Report->ObjectsDtbStatus=Objects->DtbStatus;
  Report->ObjectsReason=Objects->Reason;
  Report->ObjectsCount=Objects->Count;
  Report->ObjectsLoads=Objects->Loads;
  Report->ObjectsEpoch=Objects->Epoch;
  Report->ObjectsElapsedUsecs=Objects->ElapsedUsecs;
  Report->ObjectsPublished=Objects->Published;
  Report->ObjectsCoherent=Objects->Coherent;
 }

 EFI_GUID EarlyGuid=PIANO_EARLY_MEMORY_HOB_GUID;
 Report->EarlyStatus=Report->EarlyPublishStatus=Report->SmemStatus=EFI_NOT_FOUND;
 Report->EarlyHobStatus=DiagnosticHob(&EarlyGuid,sizeof(PIANO_EARLY_MEMORY_REPORT),&Data);
 if(Report->EarlyHobStatus==EFI_SUCCESS){
  CONST PIANO_EARLY_MEMORY_REPORT *Early=Data;
  if(Early->Version!=PIANO_EARLY_MEMORY_VERSION||Early->Bytes!=sizeof(*Early)||Early->Reserved||
     Early->ReportCrc32!=PianoEarlyMemoryReportCrc32(Early))Report->EarlyHobStatus=EFI_COMPROMISED_DATA;
  Report->EarlyStatus=Early->Status;
  Report->EarlyPublishStatus=Early->PublishStatus;
  Report->EarlyFinished=Early->Finished;
  Report->EarlyPublished=Early->Published;
  Report->EarlyColdStateVerified=Early->ColdStateVerified;
  Report->EarlyRecoveredFaults=Early->RecoveredFaults;
  Report->EarlyRawPayloadBytes=Early->RawPayloadBytes;
  Report->SmemStatus=Early->Smem.Status;
  Report->SmemReason=Early->Smem.Reason;
  Report->SmemRamVersion=Early->Smem.RamVersion;
  Report->SmemBankCount=Early->Smem.BankCount;
  Report->SmemRawEntryCount=Early->Smem.RawEntryCount;
  Report->SmemParsed=Early->Smem.Parsed;
  Report->RawPayloadCoherent=Early->RawPayloadCoherent;
  Report->SmemRepeatedMetadataEqual=Early->Smem.RepeatedMetadataEqual;
  Report->SmemRepeatedPayloadEqual=Early->Smem.RepeatedPayloadEqual;
 }
}
STATIC EFI_MEMORY_DESCRIPTOR *Descriptor(UINTN I){return (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mMapBuffer+I*mMapStride);}
STATIC EFI_STATUS Snapshot(VOID){
 if(!Live())return EFI_NOT_READY;UINTN Key;UINT32 Version;mMapBytes=sizeof(mMapBuffer);mMapStride=0;
 EFI_STATUS S=gBS->GetMemoryMap(&mMapBytes,(EFI_MEMORY_DESCRIPTOR *)mMapBuffer,&Key,&mMapStride,&Version);
 if(S!=EFI_SUCCESS)return S;if(!Live()||Version!=EFI_MEMORY_DESCRIPTOR_VERSION||mMapStride<sizeof(EFI_MEMORY_DESCRIPTOR)||mMapStride>256||mMapBytes%mMapStride)return EFI_COMPROMISED_DATA;
 UINTN Count=mMapBytes/mMapStride;if(!Count)return EFI_NOT_READY;
 for(UINTN I=0;I<Count;++I){EFI_MEMORY_DESCRIPTOR *A=Descriptor(I);if(!A->NumberOfPages||A->NumberOfPages>MAX_UINT64/4096||!Span(A->PhysicalStart,A->NumberOfPages*4096)||(A->PhysicalStart&4095)||A->Type>=EfiMaxMemoryType)return EFI_COMPROMISED_DATA;
  for(UINTN J=0;J<I;++J){EFI_MEMORY_DESCRIPTOR *B=Descriptor(J);if(Overlap(A->PhysicalStart,A->NumberOfPages*4096,B->PhysicalStart,B->NumberOfPages*4096))return EFI_COMPROMISED_DATA;}
 }return EFI_SUCCESS;
}
STATIC EFI_MEMORY_DESCRIPTOR *At(UINT64 Address){for(UINTN I=0;I<mMapBytes/mMapStride;++I){EFI_MEMORY_DESCRIPTOR *D=Descriptor(I);if(Address>=D->PhysicalStart&&Address-D->PhysicalStart<D->NumberOfPages*4096)return D;}return NULL;}
STATIC BOOLEAN HoldingCovers(UINT64 Base,UINT64 Bytes){
 if(!Span(Base,Bytes))return FALSE;UINT64 End=Base+Bytes;
 while(Base<End){UINT64 Next=Base;
  for(UINTN I=mCold.NativeCount;I<mCold.Count;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];
   if(R->MemoryType==EfiLoaderData&&Base>=R->Address&&Base-R->Address<R->Length)Next=MAX(Next,R->Address+R->Length);
  }if(Next==Base)return FALSE;Base=MIN(Next,End);
 }return TRUE;
}
STATIC EFI_STATUS Reserved(UINT64 Base,UINT64 Bytes,BOOLEAN Cma,BOOLEAN FinalTree){
 if(!Bytes)return EFI_SUCCESS;if(!Span(Base,Bytes))return EFI_COMPROMISED_DATA;
 for(UINTN I=0;I<mMapBytes/mMapStride;++I){EFI_MEMORY_DESCRIPTOR *D=Descriptor(I);if(!Overlap(Base,Bytes,D->PhysicalStart,D->NumberOfPages*4096))continue;
  if(FinalTree&&Reclaimable(D->Type)){
   UINT64 Begin=MAX(Base,D->PhysicalStart),End=MIN(Base+Bytes,D->PhysicalStart+D->NumberOfPages*4096);
   // An arbitrary active low-heap allocation is not ours to give to a DT
   // reservation. Only the cold producer's still-exclusive holding span may
   // supply a new reservation; the standard allocator cannot have reused it.
   if(D->Type!=EfiLoaderData||!HoldingCovers(Begin,End-Begin))return EFI_ACCESS_DENIED;
  }
  else if(Cma){if(D->Type!=EfiLoaderData&&D->Type!=EfiReservedMemoryType)return EFI_ACCESS_DENIED;}
  else if(Reclaimable(D->Type))return EFI_ACCESS_DENIED;
 }return EFI_SUCCESS;
}
typedef struct {UINT64 El,Sctlr,Tcr,Ttbr0,Ttbr1,Mair;} CPU_MAP;
STATIC EFI_STATUS CpuMap(CPU_MAP *C){
#ifdef PIANO_PRODUCT_MEMORY_HOST_TEST
 extern EFI_STATUS PianoMemoryHostCpu(UINT64 *);return PianoMemoryHostCpu(&C->El);
#elif defined(__aarch64__)
 __asm__ volatile("mrs %0, CurrentEL":"=r"(C->El));if(C->El!=4)return EFI_UNSUPPORTED;
 __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, ttbr1_el1\n mrs %4, mair_el1"
  :"=r"(C->Sctlr),"=r"(C->Tcr),"=r"(C->Ttbr0),"=r"(C->Ttbr1),"=r"(C->Mair)::"memory");return EFI_SUCCESS;
#else
 (VOID)C;return EFI_UNSUPPORTED;
#endif
}
STATIC BOOLEAN CpuGeometry(CONST CPU_MAP *C){
 UINTN T0=C->Tcr&63,Ips=(C->Tcr>>32)&7;
 // Only the existing 4KiB, <=48-bit, software-managed AF/dirty geometry.
 // Do not change controls or silently accept LPA2/tagged-address formats.
 return C->El==4&&(C->Sctlr&BIT0)&&!(C->Sctlr&BIT25)&&T0>=16&&T0<=39&&Ips<=5&&
  !(C->Tcr&(BIT7|BIT37|BIT38|BIT39|BIT40|BIT59))&&((C->Tcr>>14)&3)==0&&!(C->Ttbr0&0xfff);
}
STATIC EFI_STATUS TablePage(UINT64 Page){
 BOOLEAN Known=FALSE;
 for(UINTN I=0;I<mCold.NativeCount;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];
  if((!AsciiStrCmp(R->Name,"MMU_PageTables")||!AsciiStrCmp(R->Name,"DXE_Heap")||!AsciiStrCmp(R->Name,"DXE_Heap_Upper"))&&
     Page>=R->Address&&Page-R->Address<=R->Length&&4096<=R->Length-(Page-R->Address))Known=TRUE;
 }
 EFI_MEMORY_DESCRIPTOR *D=At(Page);
 if(!Known||!D||D->PhysicalStart+D->NumberOfPages*4096-Page<4096||
    (D->Type!=EfiReservedMemoryType&&D->Type!=EfiBootServicesData&&D->Type!=EfiRuntimeServicesData))return EFI_ACCESS_DENIED;
#if defined(__aarch64__) && !defined(PIANO_PRODUCT_MEMORY_HOST_TEST)
 // Prove the low table page itself readable and identity mapped before its
 // first software load. No high-DDR data is accessed by this verifier.
 UINT64 Mask,Old,Par;__asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1r, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
  :"=&r"(Mask),"=&r"(Old),"=&r"(Par):"r"(Page):"memory");
 if((Par&1)||(Par&0x000ffffffffff000ULL)!=Page||(UINT8)(Par>>56)!=0xff)return EFI_NO_MAPPING;
#endif
 return EFI_SUCCESS;
}
STATIC EFI_STATUS NormalMapping(CONST CPU_MAP *C,UINT64 Base,UINT64 Bytes){
 if(!Span(Base,Bytes))return EFI_COMPROMISED_DATA;UINTN Bits=64-(C->Tcr&63);
 STATIC CONST UINT8 PhysicalBits[]={32,36,40,42,44,48};
 UINTN StartLevel=4-((Bits-12+8)/9);UINT64 End=Base+Bytes;
 if(End>(1ULL<<Bits)||End>(1ULL<<PhysicalBits[(C->Tcr>>32)&7]))return EFI_UNSUPPORTED;
 while(Base<End){UINT64 Table=C->Ttbr0&0x0000fffffffff000ULL;BOOLEAN Leaf=FALSE;
  for(UINTN Level=StartLevel;Level<4;++Level){EFI_STATUS S=TablePage(Table);if(S!=EFI_SUCCESS)return S;
   UINTN Shift=12+9*(3-Level),Index=(Base>>Shift)&511;UINT64 Value,Again;
#ifdef PIANO_PRODUCT_MEMORY_HOST_TEST
   extern EFI_STATUS PianoMemoryHostTableEntry(UINT64,UINTN,UINT64 *);
   S=PianoMemoryHostTableEntry(Table,Index,&Value);if(S!=EFI_SUCCESS)return S;
   S=PianoMemoryHostTableEntry(Table,Index,&Again);if(S!=EFI_SUCCESS)return S;
#else
   volatile UINT64 *Entry=(volatile UINT64 *)(UINTN)(Table+Index*8);Value=*Entry;Again=*Entry;
#endif
   if(Value!=Again)return EFI_NOT_READY;UINTN Kind=Value&3;
   if(Level<3&&Kind==3){Table=Value&0x0000fffffffff000ULL;continue;}
   if((Level==0)||(Level==3?Kind!=3:Kind!=1)||!(Value&BIT10))return EFI_NO_MAPPING;
   UINT64 Block=1ULL<<Shift,Mask=Block-1,Physical=(Value&0x0000fffffffff000ULL&~Mask)|(Base&Mask);
   if(Physical!=Base||((C->Mair>>(((Value>>2)&7)*8))&255)!=0xff)return EFI_NO_MAPPING;
   Base=MIN(End,(Base&~Mask)+Block);Leaf=TRUE;break;
  }if(!Leaf)return EFI_NO_MAPPING;
 }return EFI_SUCCESS;
}
STATIC EFI_STATUS FullMap(UINT64 *Normal){
 EFI_STATUS S;*Normal=0;
 CPU_MAP Cpu,After;S=CpuMap(&Cpu);if(S!=EFI_SUCCESS)return S;if(!CpuGeometry(&Cpu))return EFI_UNSUPPORTED;
 // Newly described normal DDR remains the producer's LoaderData allocation.
 // It is not silently made Conventional, even when absent from legacy SMEM.
 for(UINTN I=mCold.NativeCount;I<mCold.Count;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];UINT64 Position=R->Address,End=Position+R->Length;
  while(Position<End){EFI_MEMORY_DESCRIPTOR *D=At(Position);if(!D||D->Type!=(UINT32)R->MemoryType)return EFI_NOT_READY;Position=MIN(End,D->PhysicalStart+D->NumberOfPages*4096);}
 }
 for(UINTN I=0;I<mCold.NativeCount;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];
  if(R->MemoryType==EfiRuntimeServicesCode||R->MemoryType==EfiRuntimeServicesData){
   for(UINT64 Position=R->Address,End=Position+R->Length;Position<End;){EFI_MEMORY_DESCRIPTOR *D=At(Position);
    if(!D||(D->Type!=EfiRuntimeServicesCode&&D->Type!=EfiRuntimeServicesData))return EFI_NOT_READY;
    Position=MIN(End,D->PhysicalStart+D->NumberOfPages*4096);
   }
  }
 }
 // Account for every complete factory RAM page. Only native explicitly
 // excluded owners may be absent from GetMemoryMap; an unexplained hole is
 // not silently treated as Reserved or as a successful full-DDR description.
 for(UINTN I=0;I<mCold.Layout.RamCount;++I){CONST PIANO_MEMORY_SPAN *R=&mCold.Layout.Ram[I];
  if(R->Base>MAX_UINT64-4095)return EFI_COMPROMISED_DATA;
  UINT64 Position=(R->Base+4095)&~4095ULL,End=(R->Base+R->Bytes)&~4095ULL;
  while(Position<End){EFI_MEMORY_DESCRIPTOR *D=At(Position);UINT64 Next=Position;
   if(D)Next=D->PhysicalStart+D->NumberOfPages*4096;
   else for(UINTN J=0;J<mCold.NativeCount;++J){CONST EFI_MEMORY_REGION_DESCRIPTOR *N=&mCold.Rows[J];
    if((N->MemoryType==EfiReservedMemoryType||N->HobOption==NoHob||N->ResourceType!=SYS_MEM)&&Position>=N->Address&&Position-N->Address<N->Length)Next=MAX(Next,N->Address+N->Length);
   }
   if(Next==Position)return EFI_NOT_READY;Position=MIN(Next,End);
  }
 }
 for(UINTN I=0;I<mCold.Layout.ReservedCount;++I){CONST PIANO_MEMORY_SPAN *R=&mCold.Layout.Fixed[I];S=Reserved(R->Base,R->Bytes,(R->Flags&PIANO_MEMORY_LAYOUT_CMA)!=0,FALSE);if(S!=EFI_SUCCESS)return S;}
 // Every fixed/unknown/cold owner represented as Reserved stays excluded.
 for(UINTN I=0;I<mCold.Count;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mCold.Rows[I];if(R->MemoryType==EfiReservedMemoryType){S=Reserved(R->Address,R->Length,FALSE,FALSE);if(S!=EFI_SUCCESS)return S;}}
 for(UINTN I=0;I<mMapBytes/mMapStride;++I){EFI_MEMORY_DESCRIPTOR *D=Descriptor(I);UINT64 Bytes=D->NumberOfPages*4096;
  BOOLEAN Runtime=D->Type==EfiRuntimeServicesCode||D->Type==EfiRuntimeServicesData;
  if(((D->Attribute&EFI_MEMORY_RUNTIME)!=0)!=Runtime)return EFI_COMPROMISED_DATA;
  if(!Reclaimable(D->Type)&&!Runtime)continue;
  if(!LayoutCovers(&mCold.Layout,D->PhysicalStart,Bytes)||(D->Attribute&EFI_MEMORY_RP)||(D->Attribute&(EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB))!=EFI_MEMORY_WB)return EFI_NOT_READY;
  S=NormalMapping(&Cpu,D->PhysicalStart,Bytes);if(S!=EFI_SUCCESS)return S;
  for(UINT64 Position=D->PhysicalStart,End=Position+Bytes;Position<End;){EFI_GCD_MEMORY_SPACE_DESCRIPTOR G={0};S=gDS->GetMemorySpaceDescriptor(Position,&G);
   if(S!=EFI_SUCCESS)return S;if(!Live()||G.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory||!Span(G.BaseAddress,G.Length)||Position<G.BaseAddress||Position-G.BaseAddress>=G.Length||(G.Attributes&EFI_MEMORY_RP)||(G.Attributes&(EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB))!=EFI_MEMORY_WB)return EFI_NOT_READY;
   Position=MIN(End,G.BaseAddress+G.Length);
  }
  if(Reclaimable(D->Type)){if(*Normal>MAX_UINT64-Bytes)return EFI_COMPROMISED_DATA;*Normal+=Bytes;}
 }S=CpuMap(&After);if(S!=EFI_SUCCESS)return S;if(CompareMem(&Cpu,&After,sizeof(Cpu)))return EFI_NOT_READY;
 return *Normal?EFI_SUCCESS:EFI_NOT_READY;
}
EFI_STATUS PianoProductMemoryCheck(VOID *Context,PIANO_LINUX_MEMORY_PROOF *Proof){
 (VOID)Context;if(!Proof)return EFI_INVALID_PARAMETER;ZeroMem(Proof,sizeof(*Proof));Proof->Revision=1;Proof->Status=EFI_NOT_READY;
 if(mBusy)return Proof->Status=EFI_ALREADY_STARTED;mBusy=TRUE;EFI_STATUS S=Capture();UINT64 Normal=0;
 if(S==EFI_SUCCESS)S=Snapshot();if(S==EFI_SUCCESS)S=FullMap(&Normal);
 if(S==EFI_SUCCESS){Proof->BootEpoch=mCold.BootEpoch;Proof->DramBytes=mCold.Layout.DramBytes;Proof->NormalBytes=Normal;
  // These follow the independent range, reservation, allocation and live
  // permission checks above. Dynamic DT requests are future memblock work;
  // there is no fabricated UEFI placement or high-memory allocator transfer.
  Proof->FullDdr=Proof->FixedReservations=Proof->DynamicReservations=Proof->RuntimeRegions=Proof->CacheVerified=Proof->OwnershipVerified=TRUE;
 }Proof->Status=S;mBusy=FALSE;return S;
}
EFI_STATUS PianoProductMemoryValidate(VOID *Context,CONST PIANO_LINUX_MEMORY_PROOF *Proof){
 if(!Proof)return EFI_INVALID_PARAMETER;PIANO_LINUX_MEMORY_PROOF Fresh;EFI_STATUS S=PianoProductMemoryCheck(Context,&Fresh);if(S!=EFI_SUCCESS)return S;
 if(Proof->Revision!=1||Proof->Status!=EFI_SUCCESS||Proof->BootEpoch!=Fresh.BootEpoch||Proof->DramBytes!=Fresh.DramBytes||!Proof->NormalBytes||Proof->UnresolvedReservations||!Proof->FullDdr||!Proof->FixedReservations||!Proof->DynamicReservations||!Proof->RuntimeRegions||!Proof->CacheVerified||!Proof->OwnershipVerified)return EFI_NOT_READY;
 return EFI_SUCCESS;
}
STATIC EFI_STATUS TreeRead(VOID *Context,UINT32 At,UINTN Bytes,VOID *Out){CopyMem(Out,(CONST UINT8 *)Context+At,Bytes);return EFI_SUCCESS;}

STATIC EFI_STATUS DtbResult(CONST CHAR8 *Stage,EFI_STATUS Status){
 mDtbStage=Stage;
 mDtbStatus=Status;
 return Status;
}

EFI_STATUS PianoProductMemoryValidateDtb(CONST VOID *Tree,UINTN Bytes){
 mDtbIndex=MAX_UINTN;
 if(!Tree||Bytes<40||Bytes>0x200000||FdtCheckHeader(Tree)||FdtTotalSize(Tree)>Bytes)
  return DtbResult("header",EFI_INVALID_PARAMETER);
 if(mBusy)return DtbResult("busy",EFI_ALREADY_STARTED);

 mBusy=TRUE;
 mDtbStage="capture";
 EFI_STATUS S=Capture();
 PIANO_MEMORY_LAYOUT Linux;
 if(S==EFI_SUCCESS){
  mDtbStage="layout";
  S=PianoMemoryLayoutRead(TreeRead,(VOID *)Tree,FdtTotalSize(Tree),&Linux);
 }
 if(S==EFI_SUCCESS&&Linux.DramBytes!=mCold.Layout.DramBytes){
  mDtbStage="ram-size";
  S=EFI_ACCESS_DENIED;
 }
 if(S==EFI_SUCCESS){
  for(UINTN I=0;I<Linux.RamCount;++I){
   if(!LayoutCovers(&mCold.Layout,Linux.Ram[I].Base,Linux.Ram[I].Bytes)){
    mDtbIndex=I;
    mDtbStage="ram-range";
    S=EFI_ACCESS_DENIED;
    break;
   }
  }
 }
 if(S==EFI_SUCCESS){
  mDtbStage="snapshot";
  S=Snapshot();
 }
 if(S==EFI_SUCCESS){
  for(UINTN I=0;I<Linux.ReservedCount;++I){
   CONST PIANO_MEMORY_SPAN *R=&Linux.Fixed[I];
   S=Reserved(R->Base,R->Bytes,(R->Flags&PIANO_MEMORY_LAYOUT_CMA)!=0,TRUE);
   if(S!=EFI_SUCCESS){
    mDtbIndex=I;
    mDtbStage="reservation";
    break;
   }
  }
 }
 mBusy=FALSE;
 return DtbResult(S==EFI_SUCCESS?"ready":mDtbStage,S);
}
