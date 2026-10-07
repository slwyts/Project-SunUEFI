// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cold occupied-object producer. No BS, target write, MMU/cache setup, resource
// HOB, high-DDR read, allocation permission, or SMEM/cookie whitelist change.
#include "PianoColdBootObjects.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>
STATIC_ASSERT(sizeof(PIANO_COLD_BOOT_HANDOFF)==144,"cold handoff bytes");
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BOOT_HANDOFF,Dtb)==8,"legacy DTB");
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BOOT_HANDOFF,EntryEl)==16,"legacy EL");
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BOOT_HANDOFF,ExtensionMagic)==24,"extension magic");
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BOOT_HANDOFF,Crc32)==128,"shim CRC");
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BOOT_HANDOFF,EntryPc)==136,"shim PC");
STATIC_ASSERT(sizeof(PIANO_COLD_BOOT_OBJECT_REPORT)+sizeof(EFI_HOB_GUID_TYPE)<65536,"typed HOB size");
STATIC PIANO_COLD_BOOT_OBJECT_REPORT mCold;
STATIC UINT32 mColdScratch[PIANO_COLD_READ_MAX/4];
STATIC PIANO_COLD_BATCH_STATE mColdBatch;
typedef struct {UINT32 Start,Bytes;BOOLEAN Valid;UINT8 Data[256];} COLD_CACHE;
STATIC COLD_CACHE mColdStructCache,mColdStringCache;
STATIC UINT32 mColdStructStart,mColdStructEnd,mColdStringStart,mColdStringEnd;
STATIC_ASSERT(OFFSET_OF(PIANO_COLD_BATCH_STATE,Words)==88,"batch ASM words");
STATIC EFI_MEMORY_REGION_DESCRIPTOR *mColdMap;STATIC UINT8 mColdCount;
STATIC UINT64 mColdDtbBase,mColdDtbBytes,mColdDtbReadBytes;
STATIC EFI_GUID mColdGuid=PIANO_COLD_OBJECT_HOB_GUID;
STATIC BOOLEAN ColdSpan(UINT64 B,UINT64 N,UINT64 A,UINT64 Z){return B&&N&&B<=MAX_UINT64-N&&Z&&A>=B&&A-B<=N&&Z<=N-(A-B);}
STATIC BOOLEAN ColdOverlap(UINT64 A,UINT64 N,UINT64 B,UINT64 Z){return A<B+Z&&B<A+N;}
STATIC EFI_STATUS ColdCpu(PIANO_COLD_CPU *C){
#ifdef PIANO_COLD_OBJECT_HOST_TEST
 extern EFI_STATUS PianoColdHostCpu(PIANO_COLD_CPU*);return PianoColdHostCpu(C);
#elif defined(__aarch64__)
 __asm__ volatile("mrs %0, CurrentEL":"=r"(C->El));if(C->El!=4)return EFI_UNSUPPORTED;
 __asm__ volatile("adr %0, .\n mov %1, sp\n mrs %2, sctlr_el1\n mrs %3, vbar_el1\n mrs %4, daif\n mrs %5, SPSel\n mrs %6, ttbr0_el1\n mrs %7, ttbr1_el1\n mrs %8, cntvct_el0\n mrs %9, cntfrq_el0"
 :"=r"(C->Pc),"=r"(C->Sp),"=r"(C->Sctlr),"=r"(C->Vbar),"=r"(C->Daif),"=r"(C->SpSel),"=r"(C->Ttbr0),"=r"(C->Ttbr1),"=r"(C->Counter),"=r"(C->Frequency)::"memory");return EFI_SUCCESS;
#else
 (VOID)C;return EFI_UNSUPPORTED;
#endif
}
STATIC CONST EFI_MEMORY_REGION_DESCRIPTOR *ColdNamed(CONST CHAR8 *Name){
 CONST EFI_MEMORY_REGION_DESCRIPTOR *R=NULL;for(UINTN I=0;I<mColdCount;++I)if(!AsciiStrCmp(mColdMap[I].Name,Name)){if(R)return NULL;R=&mColdMap[I];}return R;
}
STATIC BOOLEAN ColdKnown(CONST CHAR8 *Name,UINT64 A,UINT64 N){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=ColdNamed(Name);return R&&ColdSpan(R->Address,R->Length,A,N);}
STATIC BOOLEAN ColdInput(UINT64 A,UINT64 N){return ColdKnown("Kernel",A,N)||ColdKnown("DXE_Heap",A,N)||ColdKnown("DXE_Heap_Upper",A,N);}
STATIC BOOLEAN ColdSource(UINT64 A,UINT64 N){return ColdInput(A,N)||ColdKnown("FD_Reserved",A,N)||ColdKnown("UEFI_FD",A,N);}
STATIC EFI_STATUS ColdAdd(PIANO_COLD_OBJECT_ROLE Role,UINT64 B,UINT64 N){
 if(!ColdSpan(B,N,B,N)||mCold.Count>=PIANO_COLD_OBJECT_MAX)return EFI_COMPROMISED_DATA;
 mCold.Objects[mCold.Count++]=(PIANO_COLD_BOOT_OBJECT){B,N,Role,0};return EFI_SUCCESS;
}
STATIC EFI_STATUS ColdFresh(VOID){
 PIANO_COLD_CPU C={0};EFI_STATUS E=ColdCpu(&C);if(E!=EFI_SUCCESS)return E;mCold.After=C;
 if(C.Counter>=mCold.Cpu.Counter){mCold.ElapsedTicks=C.Counter-mCold.Cpu.Counter;if(C.Frequency)mCold.ElapsedUsecs=mCold.ElapsedTicks<=MAX_UINT64/1000000?mCold.ElapsedTicks*1000000/C.Frequency:MAX_UINT64;}
 if(C.El!=4||C.SpSel!=1||(C.Sctlr&(BIT0|BIT2))||C.Sctlr!=mCold.Cpu.Sctlr||C.Vbar!=mCold.Cpu.Vbar||C.Daif!=mCold.Cpu.Daif||
    C.Ttbr0!=mCold.Cpu.Ttbr0||C.Ttbr1!=mCold.Cpu.Ttbr1||C.Frequency!=mCold.Cpu.Frequency||
    !ColdKnown("UEFI_FD",C.Pc,4)||!ColdKnown("UEFI_Stack",C.Sp-1,1))return EFI_NOT_READY;
 if(!C.Frequency||C.Frequency>1000000000ULL||C.Counter<mCold.Cpu.Counter||mCold.ElapsedUsecs>=PIANO_COLD_MAX_USECS||mCold.Loads>=PIANO_COLD_TOTAL_MAX/4)return EFI_TIMEOUT;
 return EFI_SUCCESS;
}
STATIC EFI_STATUS ColdRead(UINT64 A,UINTN N,VOID *Out){
 if(!Out||!N||N>PIANO_COLD_READ_MAX||(A&3)||(N&3))return EFI_INVALID_PARAMETER;
 // Independent typed admission. No arbitrary cookie, table entry or report
 // flag opens a physical address. The SMEM adapter remains entirely separate.
 if(!ColdSpan(PIANO_COLD_HANDOFF_ADDRESS,4096,A,N)&&!ColdSpan(mColdDtbBase,mColdDtbReadBytes,A,N)&&
    !(mCold.Handoff.ShimBase&&A==mCold.Handoff.ShimBase&&N==64&&ColdSource(A,N)))return EFI_ACCESS_DENIED;
 mCold.LastReadStart=A;mCold.LastReadBytes=(UINT32)N;EFI_STATUS E=ColdFresh();
 if(E==EFI_SUCCESS&&N/4>PIANO_COLD_TOTAL_MAX/4-mCold.Loads)E=EFI_TIMEOUT;
 if(E==EFI_SUCCESS){ZeroMem(&mColdBatch,sizeof(mColdBatch));mCold.GuardBatches++;UINTN V=PianoColdSecRead256(A,mColdScratch,&mColdBatch,N);mCold.LastRead=mColdBatch.Read;
  if(mColdBatch.Words>N/4){ZeroMem(mColdScratch,sizeof(mColdScratch));return EFI_COMPROMISED_DATA;}
  mCold.Loads+=(UINT32)mColdBatch.Words;if(mColdBatch.Words)mCold.LastCompletedAddress=A+(mColdBatch.Words-1)*4;
  if(V==1){mCold.Loads++;mCold.RecoveredFaults++;mCold.LastFault=mCold.LastRead;E=EFI_NOT_READY;}else if(V||mColdBatch.Words!=N/4)E=EFI_UNSUPPORTED;else E=ColdFresh();
 }if(E==EFI_SUCCESS)CopyMem(Out,mColdScratch,N);ZeroMem(mColdScratch,sizeof(mColdScratch));return E;
}
STATIC EFI_STATUS ColdStable(UINT64 A,UINTN N,VOID *Out){UINT8 X[256],Y[256];EFI_STATUS E=ColdRead(A,N,X);if(E!=EFI_SUCCESS)return E;E=ColdRead(A,N,Y);if(E!=EFI_SUCCESS)return E;if(CompareMem(X,Y,N))return EFI_MEDIA_CHANGED;CopyMem(Out,X,N);return EFI_SUCCESS;}
STATIC UINT32 ColdBe(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC EFI_STATUS ColdDtbRead(UINT32 At,UINTN N,VOID *Out){
 if(!N||At>mColdDtbBytes||N>mColdDtbBytes-At)return EFI_COMPROMISED_DATA;
 COLD_CACHE *Cache=NULL;if(At>=mColdStructStart&&At+N<=mColdStructEnd)Cache=&mColdStructCache;else if(At>=mColdStringStart&&At+N<=mColdStringEnd)Cache=&mColdStringCache;
 if(Cache){
  UINT8 Copy[256];if(N>sizeof(Copy))return EFI_BAD_BUFFER_SIZE;UINTN Done=0;
  while(Done<N){UINT32 Here=At+(UINT32)Done;
   if(Cache->Valid&&Here>=Cache->Start&&Here-Cache->Start<Cache->Bytes)mCold.CacheHits++;
    else{Cache->Valid=FALSE;UINT32 Start=Here&~255U;UINT32 Bytes=MIN(256U,(UINT32)(mColdDtbReadBytes-Start));if(!Bytes||Here-Start>=Bytes)return EFI_BAD_BUFFER_SIZE;
    EFI_STATUS E=ColdRead(mColdDtbBase+Start,Bytes,Cache->Data);if(E!=EFI_SUCCESS)return E;Cache->Start=Start;Cache->Bytes=Bytes;Cache->Valid=TRUE;mCold.CacheFills++;
   }
   UINTN Take=MIN(N-Done,Cache->Bytes-(Here-Cache->Start));CopyMem(Copy+Done,Cache->Data+Here-Cache->Start,Take);Done+=Take;
  }CopyMem(Out,Copy,N);return EFI_SUCCESS;
 }
 UINT32 First=At&~3U;UINTN Bytes=ALIGN_VALUE((UINTN)(At-First)+N,4);UINT8 Buffer[256];if(Bytes>sizeof(Buffer))return EFI_BAD_BUFFER_SIZE;
 EFI_STATUS E=ColdRead(mColdDtbBase+First,Bytes,Buffer);if(E==EFI_SUCCESS)CopyMem(Out,Buffer+At-First,N);return E;
}
STATIC EFI_STATUS ColdDtbWord(UINT32 At,UINT32 *V){UINT8 B[4];EFI_STATUS E=ColdDtbRead(At,4,B);if(E==EFI_SUCCESS)*V=ColdBe(B);return E;}
STATIC EFI_STATUS ColdString(UINT32 At,UINT32 Limit,CHAR8 *Name,UINTN Capacity,UINT32 *Used){
 for(UINT32 I=0;I<Capacity&&At<=Limit&&I<Limit-At;++I){EFI_STATUS E=ColdDtbRead(At+I,1,&Name[I]);if(E!=EFI_SUCCESS)return E;if(!Name[I]){*Used=I+1;return EFI_SUCCESS;}}return EFI_COMPROMISED_DATA;
}
typedef struct {UINT8 Header[40];UINT32 Bytes;UINT64 Start,End;} COLD_DTB;
STATIC EFI_STATUS ColdParseDtb(COLD_DTB *D){
 ZeroMem(&mColdStructCache,sizeof(mColdStructCache));ZeroMem(&mColdStringCache,sizeof(mColdStringCache));mColdStructStart=mColdStructEnd=mColdStringStart=mColdStringEnd=0;
 ZeroMem(D,sizeof(*D));EFI_STATUS E=ColdRead(mColdDtbBase,40,D->Header);if(E!=EFI_SUCCESS)return E;
 UINT32 Total=ColdBe(D->Header+4),Struct=ColdBe(D->Header+8),Strings=ColdBe(D->Header+12),Reserve=ColdBe(D->Header+16),StringBytes=ColdBe(D->Header+32),StructBytes=ColdBe(D->Header+36);
 if(ColdBe(D->Header)!=0xd00dfeed||Total<40||Total>PIANO_COLD_DTB_MAX||Total!=mColdDtbBytes||ColdBe(D->Header+20)!=17||ColdBe(D->Header+24)>17||
    (Struct&3)||Struct<40||Struct>Total||StructBytes>Total-Struct||Strings<40||Strings>Total||StringBytes>Total-Strings||!StructBytes||!StringBytes||
    Reserve<40||(Reserve&7)||Reserve>Total||Total-Reserve<16||
    ColdOverlap(Struct,StructBytes,Strings,StringBytes)||ColdOverlap(Reserve,16,Struct,StructBytes)||ColdOverlap(Reserve,16,Strings,StringBytes))return EFI_COMPROMISED_DATA;
 mColdStructStart=Struct;mColdStructEnd=Struct+StructBytes;mColdStringStart=Strings;mColdStringEnd=Strings+StringBytes;
 D->Bytes=Total;UINT32 At=Struct,Limit=Struct+StructBytes,Depth=0;BOOLEAN Chosen=FALSE,Seen=FALSE,GotStart=FALSE,GotEnd=FALSE;
 while(At<=Limit&&Limit-At>=4){UINT32 Token;E=ColdDtbWord(At,&Token);if(E!=EFI_SUCCESS)return E;At+=4;
  if(Token==1){CHAR8 Name[128];UINT32 Used=0;E=ColdString(At,Limit,Name,sizeof(Name),&Used);if(E!=EFI_SUCCESS)return E;if(!Depth&&Name[0])return EFI_COMPROMISED_DATA;
   if(Depth==1&&!AsciiStrCmp(Name,"chosen")){if(Seen)return EFI_COMPROMISED_DATA;Chosen=Seen=TRUE;}if(++Depth>64)return EFI_COMPROMISED_DATA;At=ALIGN_VALUE(At+Used,4);}
  else if(Token==2){if(!Depth)return EFI_COMPROMISED_DATA;if(Depth==2&&Chosen)return GotStart&&GotEnd?EFI_SUCCESS:EFI_NOT_FOUND;if(Depth==2)Chosen=FALSE;Depth--;}
  else if(Token==3){UINT32 N,Offset;if(Limit-At<8)return EFI_COMPROMISED_DATA;E=ColdDtbWord(At,&N);if(E!=EFI_SUCCESS)return E;E=ColdDtbWord(At+4,&Offset);if(E!=EFI_SUCCESS)return E;At+=8;
   if(!Depth||N>Limit-At||Offset>=StringBytes)return EFI_COMPROMISED_DATA;
   if(Chosen&&Depth==2){CHAR8 Name[128];UINT32 Used;E=ColdString(Strings+Offset,Strings+StringBytes,Name,sizeof(Name),&Used);if(E!=EFI_SUCCESS)return E;
    BOOLEAN Start=!AsciiStrCmp(Name,"linux,initrd-start"),End=!AsciiStrCmp(Name,"linux,initrd-end");if(Start||End){if((N!=4&&N!=8)||(Start?GotStart:GotEnd))return EFI_COMPROMISED_DATA;UINT8 B[8];E=ColdDtbRead(At,N,B);if(E!=EFI_SUCCESS)return E;UINT64 V=0;for(UINTN I=0;I<N;++I)V=(V<<8)|B[I];if(Start){D->Start=V;GotStart=TRUE;}else{D->End=V;GotEnd=TRUE;}}
   }At=ALIGN_VALUE(At+N,4);if(At>Limit)return EFI_COMPROMISED_DATA;}
  else if(Token==4)continue;else if(Token==9)return !Depth&&GotStart&&GotEnd?EFI_SUCCESS:EFI_NOT_FOUND;else return EFI_COMPROMISED_DATA;
 }return EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS ColdLayout(VOID){
 CONST struct {CONST CHAR8 *Name;PIANO_COLD_OBJECT_ROLE Role;} Items[]={{"UEFI_FD",PianoColdObjectFirmware},{"UEFI_Stack",PianoColdObjectStack},{"CPU_Vectors",PianoColdObjectVectorReservation},{"MMU_PageTables",PianoColdObjectMmuReservation},{"SEC_Heap",PianoColdObjectSecHeap},{"Sched_Heap",PianoColdObjectScheduler},{"FV_Region",PianoColdObjectFv},{"BootHandoff",PianoColdObjectHandoff}};
 for(UINTN I=0;I<ARRAY_SIZE(Items);++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=ColdNamed(Items[I].Name);if(!R||ColdAdd(Items[I].Role,R->Address,R->Length)!=EFI_SUCCESS)return EFI_COMPROMISED_DATA;}
 if(!ColdKnown("UEFI_FD",FixedPcdGet64(PcdFdBaseAddress),FixedPcdGet32(PcdFdSize))||!ColdKnown("UEFI_Stack",FixedPcdGet64(PcdCPUCoresStackBase),FixedPcdGet32(PcdCPUCorePrimaryStackSize))||
    !ColdKnown("BootHandoff",PIANO_COLD_HANDOFF_ADDRESS,144))return EFI_COMPROMISED_DATA;
 return ColdAdd(PianoColdObjectActiveVector,mCold.Cpu.Vbar,2048);
}
STATIC EFI_STATUS ColdFinish(EFI_STATUS E,PIANO_COLD_OBJECT_REASON Reason){mCold.Status=E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;mCold.Reason=E==EFI_TIMEOUT?PianoColdReasonBudget:Reason;mCold.Finished=TRUE;mCold.ReportCrc32=PianoColdObjectsCrc(&mCold);return mCold.Status;}
EFI_STATUS PianoColdBootObjectsObserve(VOID){
 if(mCold.Attempted)return EFI_ALREADY_STARTED;ZeroMem(&mCold,sizeof(mCold));mCold.Attempted=TRUE;mCold.Version=PIANO_COLD_OBJECT_VERSION;mCold.Bytes=sizeof(mCold);mCold.HandoffStatus=mCold.DtbStatus=mCold.PublishStatus=EFI_NOT_STARTED;
 GetMemoryMap(&mColdMap,&mColdCount);if(!mColdMap||!mColdCount)return ColdFinish(EFI_NOT_READY,PianoColdReasonLayout);
 EFI_STATUS E=ColdCpu(&mCold.Cpu);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonCpu);
 if(mCold.Cpu.El!=4||mCold.Cpu.SpSel!=1||(mCold.Cpu.Sctlr&(BIT0|BIT2))||(mCold.Cpu.Vbar&2047)||
    !ColdKnown("UEFI_FD",mCold.Cpu.Pc,4)||!ColdKnown("UEFI_Stack",mCold.Cpu.Sp-1,1)||
    (!ColdKnown("UEFI_FD",mCold.Cpu.Vbar,2048)&&!ColdKnown("CPU_Vectors",mCold.Cpu.Vbar,2048)))return ColdFinish(EFI_NOT_READY,PianoColdReasonCpu);
 E=ColdLayout();if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonLayout);
 E=mCold.HandoffStatus=ColdStable(PIANO_COLD_HANDOFF_ADDRESS,144,&mCold.Handoff);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonRead);
 PIANO_COLD_BOOT_HANDOFF *H=&mCold.Handoff;if(H->Magic!=PIANO_COLD_HANDOFF_MAGIC)return ColdFinish(EFI_NOT_READY,PianoColdReasonHandoff);
 if(H->ExtensionMagic!=PIANO_COLD_EXTENSION_MAGIC)return ColdFinish(EFI_NOT_READY,PianoColdReasonLegacyHandoff);
 if(H->Version!=1||H->Bytes!=144||H->Reserved||H->Crc32!=PianoColdHandoffCrc(H)||(H->Flags&~127ULL)||(H->Flags&63)!=63||H->EntryEl!=4||
    H->FdBase!=FixedPcdGet64(PcdFdBaseAddress)||H->FdBytes!=FixedPcdGet32(PcdFdSize)||!H->ShimBytes||H->ShimBytes>65536||
    !ColdSpan(H->ShimBase,H->ShimBytes,H->EntryPc,4)||H->ShimBase>MAX_UINT64-H->ShimBytes||H->FdSource!=H->ShimBase+H->ShimBytes||
    !ColdSource(H->ShimBase,H->ShimBytes)||!ColdSource(H->FdSource,H->FdBytes))return ColdFinish(EFI_NOT_READY,PianoColdReasonHandoff);
 if(ColdOverlap(H->ShimBase,H->ShimBytes,PIANO_COLD_HANDOFF_ADDRESS,144)||ColdOverlap(H->FdSource,H->FdBytes,PIANO_COLD_HANDOFF_ADDRESS,144)||
    (H->FdSource!=H->FdBase&&ColdOverlap(H->FdSource,H->FdBytes,H->FdBase,H->FdBytes))||((H->Flags&64)!=0)!=(H->FdSource!=H->FdBase))return ColdFinish(EFI_NOT_READY,PianoColdReasonHandoff);
 if(!H->Counter||H->Frequency!=mCold.Cpu.Frequency||mCold.Cpu.Counter<H->Counter)return ColdFinish(EFI_NOT_READY,PianoColdReasonEpoch);mCold.Epoch=H->Counter;
 UINT8 Header[64];E=ColdStable(H->ShimBase,sizeof(Header),Header);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonRead);UINT64 Base,Bytes;UINT32 Magic;CopyMem(&Base,Header+8,8);CopyMem(&Bytes,Header+16,8);CopyMem(&Magic,Header+56,4);
 if(Base!=H->FdBase||Bytes!=H->FdBytes||Magic!=0x644d5241)return ColdFinish(EFI_NOT_READY,PianoColdReasonHandoff);
 ColdAdd(PianoColdObjectOriginalShim,H->ShimBase,H->ShimBytes);ColdAdd(PianoColdObjectOriginalFd,H->FdSource,H->FdBytes);
 if((H->Dtb&3)||!ColdInput(H->Dtb,40))return ColdFinish(EFI_NOT_READY,PianoColdReasonDtb);mColdDtbBase=H->Dtb;mColdDtbBytes=mColdDtbReadBytes=40;
 UINT8 DtbHeader[40];E=ColdStable(H->Dtb,40,DtbHeader);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonRead);UINT32 Total=ColdBe(DtbHeader+4);if(Total<40||Total>PIANO_COLD_DTB_MAX||!ColdInput(H->Dtb,ALIGN_VALUE(Total,4)))return ColdFinish(EFI_NOT_READY,PianoColdReasonDtb);mColdDtbBytes=Total;mColdDtbReadBytes=ALIGN_VALUE(Total,4);
 COLD_DTB A,B;E=mCold.DtbStatus=ColdParseDtb(&A);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonDtb);E=mCold.DtbStatus=ColdParseDtb(&B);if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonDtb);
 if(CompareMem(&A,&B,sizeof(A)))return ColdFinish(EFI_MEDIA_CHANGED,PianoColdReasonCoherence);
 if(A.End<=A.Start||A.End-A.Start>0x10000000||!ColdInput(A.Start,A.End-A.Start))return ColdFinish(EFI_NOT_READY,PianoColdReasonInitrd);
 CopyMem(mCold.DtbHeader,A.Header,40);mCold.DtbBytes=A.Bytes;mCold.DtbHeaderCrc32=PianoEarlyMemoryBytesCrc32(A.Header,40);mCold.InitrdStart=A.Start;mCold.InitrdEnd=A.End;
 ColdAdd(PianoColdObjectFactoryDtb,H->Dtb,A.Bytes);ColdAdd(PianoColdObjectCombinedInitrd,A.Start,A.End-A.Start);
 E=ColdFresh();if(E!=EFI_SUCCESS)return ColdFinish(E,PianoColdReasonCpu);ColdCpu(&mCold.After);mCold.Coherent=TRUE;return ColdFinish(EFI_SUCCESS,PianoColdReasonNone);
}
EFI_STATUS PianoColdBootObjectsPublishHob(VOID){
 if(!mCold.Finished||mCold.Published)return EFI_NOT_READY;
 // Publishing frozen failure evidence is not another protected target read.
 // Probe time/load limits remain strict, but cannot suppress their own report.
 PIANO_COLD_CPU C={0};EFI_STATUS E=ColdCpu(&C);
 if(E!=EFI_SUCCESS||C.El!=4||C.SpSel!=1||(C.Sctlr&(BIT0|BIT2))||!ColdKnown("UEFI_FD",C.Pc,4)||!ColdKnown("UEFI_Stack",C.Sp-1,1)){
  mCold.PublishStatus=E==EFI_SUCCESS?EFI_NOT_READY:E;mCold.ReportCrc32=PianoColdObjectsCrc(&mCold);return mCold.PublishStatus;
 }
 EFI_HOB_HANDOFF_INFO_TABLE *H=GetHobList();
 if(!H||!ColdKnown("DXE_Heap",(UINT64)(UINTN)H,sizeof(*H))||H->Header.HobType!=EFI_HOB_TYPE_HANDOFF||H->Header.HobLength!=sizeof(*H)||
    H->EfiFreeMemoryBottom<=(UINT64)(UINTN)H||H->EfiFreeMemoryTop<H->EfiFreeMemoryBottom||
    !ColdKnown("DXE_Heap",(UINT64)(UINTN)H,H->EfiFreeMemoryBottom-(UINT64)(UINTN)H)||mCold.Count>=PIANO_COLD_OBJECT_MAX){mCold.PublishStatus=EFI_COMPROMISED_DATA;mCold.ReportCrc32=PianoColdObjectsCrc(&mCold);return mCold.PublishStatus;}
 if(H->EfiFreeMemoryTop-H->EfiFreeMemoryBottom<ALIGN_VALUE(sizeof(EFI_HOB_GUID_TYPE)+sizeof(mCold),8)){mCold.PublishStatus=EFI_OUT_OF_RESOURCES;mCold.ReportCrc32=PianoColdObjectsCrc(&mCold);return mCold.PublishStatus;}
 VOID *Data=BuildGuidHob(&mColdGuid,sizeof(mCold));if(!Data)return mCold.PublishStatus=EFI_OUT_OF_RESOURCES;
 E=ColdAdd(PianoColdObjectHobHeap,(UINT64)(UINTN)H,H->EfiFreeMemoryBottom-(UINT64)(UINTN)H);if(E!=EFI_SUCCESS)return E;
 mCold.Published=TRUE;mCold.PublishStatus=EFI_SUCCESS;mCold.ReportCrc32=PianoColdObjectsCrc(&mCold);CopyMem(Data,&mCold,sizeof(mCold));return EFI_SUCCESS;
}
CONST PIANO_COLD_BOOT_OBJECT_REPORT *PianoColdBootObjectsReport(VOID){return &mCold;}
