// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure shared shape/integrity validation; no SEC/DXE services or target reads.
#include "PianoColdBootObjects.h"
STATIC BOOLEAN ColdSpan(UINT64 B,UINT64 N,UINT64 A,UINT64 Z){return B&&N&&B<=MAX_UINT64-N&&Z&&A>=B&&A-B<=N&&Z<=N-(A-B);}
STATIC UINT32 ColdBe(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC CONST PIANO_COLD_BOOT_OBJECT *ColdReportRole(CONST PIANO_COLD_BOOT_OBJECT_REPORT *R,PIANO_COLD_OBJECT_ROLE Role){CONST PIANO_COLD_BOOT_OBJECT *O=NULL;for(UINT32 I=0;I<R->Count;++I)if(R->Objects[I].Role==Role){if(O)return NULL;O=&R->Objects[I];}return O;}
EFI_STATUS PianoColdBootObjectsValidate(CONST PIANO_COLD_BOOT_OBJECT_REPORT *R){
 if(!R||R->Version!=PIANO_COLD_OBJECT_VERSION||R->Bytes!=sizeof(*R)||R->Reserved||R->ReservedFlags||R->ReservedCache||R->ReportCrc32!=PianoColdObjectsCrc(R)||R->Attempted!=TRUE||R->Finished!=TRUE||R->Published>TRUE||R->Coherent>TRUE||R->Count>PIANO_COLD_OBJECT_MAX||
    R->Reason>PianoColdReasonBudget||R->Loads>PIANO_COLD_TOTAL_MAX/4||R->RecoveredFaults>R->Loads||(R->Status!=EFI_SUCCESS&&!EFI_ERROR(R->Status))||
    R->ReservedRead||R->MemoryOwnershipGranted||R->HighDdrPublished||R->AuthorityReady||
    R->FactoryPanel>PianoFactoryPanelCsot||R->FactoryBootargsBytes>PIANO_FACTORY_BOOTARGS_MAX||R->FactoryPanelArguments>R->FactoryBootargsBytes||
    (R->FactoryPanel&&R->FactoryPanelArguments!=1)||(!R->FactoryBootargsBytes&&(R->FactoryPanel||R->FactoryBootargsCrc32||R->FactoryPanelArguments)))return EFI_COMPROMISED_DATA;
 for(UINT32 I=0;I<R->Count;++I)if(R->Objects[I].Reserved||R->Objects[I].Role<=PianoColdObjectNone||R->Objects[I].Role>PianoColdObjectHobHeap||!ColdSpan(R->Objects[I].Base,R->Objects[I].Bytes,R->Objects[I].Base,R->Objects[I].Bytes))return EFI_COMPROMISED_DATA;
 for(UINT32 I=R->Count;I<PIANO_COLD_OBJECT_MAX;++I)if(R->Objects[I].Base||R->Objects[I].Bytes||R->Objects[I].Role||R->Objects[I].Reserved)return EFI_COMPROMISED_DATA;
 if(R->Status!=EFI_SUCCESS)return EFI_NOT_READY;
 CONST PIANO_COLD_BOOT_HANDOFF *H=&R->Handoff;
 if(!R->Coherent||!R->Epoch||R->Epoch!=H->Counter||H->Magic!=PIANO_COLD_HANDOFF_MAGIC||H->ExtensionMagic!=PIANO_COLD_EXTENSION_MAGIC||H->Version!=1||H->Bytes!=144||H->Reserved||
    H->Crc32!=PianoColdHandoffCrc(H)||H->EntryEl!=4||(H->Flags&~127ULL)||(H->Flags&63)!=63||H->FdSource!=H->ShimBase+H->ShimBytes||!ColdSpan(H->ShimBase,H->ShimBytes,H->EntryPc,4)||
    R->DtbHeaderCrc32!=PianoEarlyMemoryBytesCrc32(R->DtbHeader,40)||ColdBe(R->DtbHeader)!=0xd00dfeed||ColdBe(R->DtbHeader+4)!=R->DtbBytes||
    R->InitrdEnd<=R->InitrdStart||R->Cpu.El!=4||R->Cpu.SpSel!=1||(R->Cpu.Sctlr&(BIT0|BIT2))||R->After.El!=R->Cpu.El||R->After.Sctlr!=R->Cpu.Sctlr||R->After.Vbar!=R->Cpu.Vbar)return EFI_COMPROMISED_DATA;
 CONST PIANO_COLD_BOOT_OBJECT *Fd=ColdReportRole(R,PianoColdObjectFirmware),*Stack=ColdReportRole(R,PianoColdObjectStack),*Vector=ColdReportRole(R,PianoColdObjectActiveVector),*Shim=ColdReportRole(R,PianoColdObjectOriginalShim),*Source=ColdReportRole(R,PianoColdObjectOriginalFd),*Dtb=ColdReportRole(R,PianoColdObjectFactoryDtb),*Initrd=ColdReportRole(R,PianoColdObjectCombinedInitrd);
 if(!Fd||!Stack||!Vector||!Shim||!Source||!Dtb||!Initrd||Fd->Base!=H->FdBase||Fd->Bytes!=H->FdBytes||!ColdSpan(Fd->Base,Fd->Bytes,R->Cpu.Pc,4)||
    !ColdSpan(Stack->Base,Stack->Bytes,R->Cpu.Sp-1,1)||Vector->Base!=R->Cpu.Vbar||Vector->Bytes!=2048||Shim->Base!=H->ShimBase||Shim->Bytes!=H->ShimBytes||Source->Base!=H->FdSource||Source->Bytes!=H->FdBytes||
    Dtb->Base!=H->Dtb||Dtb->Bytes!=R->DtbBytes||Initrd->Base!=R->InitrdStart||Initrd->Bytes!=R->InitrdEnd-R->InitrdStart||
    (R->Published&&!ColdReportRole(R,PianoColdObjectHobHeap)))return EFI_COMPROMISED_DATA;
 return EFI_SUCCESS;
}
