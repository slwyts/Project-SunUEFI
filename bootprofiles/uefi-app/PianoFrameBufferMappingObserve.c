// SPDX-License-Identifier: BSD-2-Clause-Patent
// CPU controls and translation metadata only; no framebuffer/PTE load or write.
#include "PianoFrameBufferMappingObserve.h"
#include <Protocol/GraphicsOutput.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
STATIC PIANO_FB_MAPPING_REPORT mReport={.Revision=1};
STATIC BOOLEAN mBusy;
STATIC CONST UINT64 mPages[]={PIANO_FB_MAPPING_BASE,0xfd509000ULL,0xfe212000ULL};
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Live(PIANO_FB_MAPPING_ALIVE Alive,PIANO_FB_MAPPING_SNAPSHOT *S){
  if(mReport.ServicesLost||Alive()!=TRUE){mReport.ServicesLost=TRUE;if(S){S->ServicesLost=TRUE;S->Status=EFI_ABORTED;}return FALSE;}return TRUE;
}
STATIC EFI_STATUS Cpu(PIANO_FB_CPU_STATE *S){
#ifdef PIANO_FB_MAPPING_HOST_TEST
  extern EFI_STATUS PianoFbMappingHostCpu(PIANO_FB_CPU_STATE *);return PianoFbMappingHostCpu(S);
#elif defined(__aarch64__)
  __asm__ volatile("mrs %0, CurrentEL":"=r"(S->El));if(S->El!=4)return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, ttbr1_el1\n mrs %4, mair_el1\n mrs %5, daif"
    :"=r"(S->Sctlr),"=r"(S->Tcr),"=r"(S->Ttbr0),"=r"(S->Ttbr1),"=r"(S->Mair),"=r"(S->Daif)::"memory");return EFI_SUCCESS;
#else
  (VOID)S;return EFI_UNSUPPORTED;
#endif
}
STATIC BOOLEAN ValidCpu(CONST PIANO_FB_CPU_STATE *S){
  STATIC CONST UINT8 Width[]={32,36,40,42,44,48};
  UINT32 T0=(UINT32)(S->Tcr&63),Ips=(UINT32)((S->Tcr>>32)&7);
  if(S->El!=4 || !(S->Sctlr&1) || (S->Sctlr&BIT25) || T0<16 || T0>39 || Ips>5 ||
     (S->Tcr&(BIT7|BIT37|BIT38|BIT39|BIT40|BIT59)) || ((S->Tcr>>14)&3)!=0 || (S->Ttbr0&0xffe))return FALSE;
  UINT64 Limit=MIN(1ULL<<(64-T0),1ULL<<Width[Ips]);
  return (S->Ttbr0&0x0000fffffffff000ULL)<(1ULL<<Width[Ips]) && PIANO_FB_MAPPING_BASE+PIANO_FB_MAPPING_BYTES<=Limit;
}
STATIC EFI_STATUS At(UINT64 Address,CONST PIANO_FB_CPU_STATE *Expected,UINT64 *Par,PIANO_FB_CPU_STATE *Current){
  // HA/HD would permit hardware PTE updates during AT. Refuse them, DS/LPA2,
  // disabled MMU and unsupported geometry; never repair CPU controls. Recheck
  // under DAIF masking so an IRQ between GCD and AT cannot enable these bits.
#if defined(__aarch64__) && !defined(PIANO_FB_MAPPING_HOST_TEST)
  UINT64 OuterMask;__asm__ volatile("mrs %0, daif\n msr daifset, #15":"=r"(OuterMask)::"memory");
#endif
  EFI_STATUS Status=Cpu(Current);
#if defined(__aarch64__) && !defined(PIANO_FB_MAPPING_HOST_TEST)
  Current->Daif=OuterMask;
#endif
  if(Status!=EFI_SUCCESS)goto Done;
  if(!ValidCpu(Current)){Status=EFI_UNSUPPORTED;goto Done;}
  if(CompareMem(Expected,Current,sizeof(*Current))){Status=EFI_NOT_READY;goto Done;}
#ifdef PIANO_FB_MAPPING_HOST_TEST
  extern EFI_STATUS PianoFbMappingHostAt(UINT64,UINT64 *);Status=PianoFbMappingHostAt(Address,Par);
#elif defined(__aarch64__)
  // Preserve caller interrupt masks and PAR exactly. AT performs a CPU table
  // walk; this module never loads framebuffer data or walks PTEs in software.
  UINT64 Mask,Old;__asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1r, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
    :"=&r"(Mask),"=&r"(Old),"=&r"(*Par):"r"(Address):"memory");Status=EFI_SUCCESS;
#else
  (VOID)Address;(VOID)Par;Status=EFI_UNSUPPORTED;
#endif
Done:
#if defined(__aarch64__) && !defined(PIANO_FB_MAPPING_HOST_TEST)
  __asm__ volatile("msr daif, %0"::"r"(OuterMask):"memory");
#endif
  return Status;
}
STATIC EFI_STATUS Metadata(PIANO_FB_MAPPING_ALIVE Alive,PIANO_FB_MAPPING_SNAPSHOT *S,PIANO_FB_MAPPING_ROUND *R){
  EFI_GRAPHICS_OUTPUT_PROTOCOL *G=NULL;
  if(!Live(Alive,S))return EFI_ABORTED;
  EFI_STATUS Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&G);
  if(!Live(Alive,S))return EFI_ABORTED;
  if(Status!=EFI_SUCCESS)return Exact(Status);
  if(!G||!G->Mode||!G->Mode->Info||G->Mode->SizeOfInfo<sizeof(*G->Mode->Info))return EFI_COMPROMISED_DATA;
  R->Gop=(UINTN)G;R->BltPc=(UINTN)G->Blt;R->ModePointer=(UINTN)G->Mode;R->InfoPointer=(UINTN)G->Mode->Info;
  R->Base=G->Mode->FrameBufferBase;R->Bytes=G->Mode->FrameBufferSize;
  R->Width=G->Mode->Info->HorizontalResolution;R->Height=G->Mode->Info->VerticalResolution;
  R->Stride=G->Mode->Info->PixelsPerScanLine;R->Format=(UINT32)G->Mode->Info->PixelFormat;
  return R->Base==PIANO_FB_MAPPING_BASE&&R->Bytes==PIANO_FB_MAPPING_BYTES&&
    R->Width==3200&&R->Height==2136&&R->Stride==3200&&R->Format==PixelBlueGreenRedReserved8BitPerColor?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
}
STATIC VOID Emit(CONST PIANO_FB_MAPPING_SNAPSHOT *S){
  DEBUG((DEBUG_WARN,"PIANO_FB_MAP phase=%a status=%r rounds=%u samples=%u coherent=%u lost=%u no_target_load=1 whole_range_ready=0\n",
    S->Phase,S->Status,S->Rounds,S->SampleCount,S->Coherent,S->ServicesLost));
  for(UINT32 J=0;J<S->Rounds;++J){CONST PIANO_FB_MAPPING_ROUND *R=&S->Round[J];
    DEBUG((DEBUG_WARN,"PIANO_FB_GOP round=%u interface=%lx blt=%lx base=%lx bytes=%lx meta_status=%r\n",
      J,(UINT64)R->Gop,(UINT64)R->BltPc,R->Base,R->Bytes,R->MetadataStatus));
    DEBUG((DEBUG_WARN,"PIANO_FB_CPU round=%u status=%r el=%lx sctlr=%lx tcr=%lx daif=%lx\n",
      J,R->CpuStatus,R->Before.El,R->Before.Sctlr,R->Before.Tcr,R->Before.Daif));
    DEBUG((DEBUG_WARN,"PIANO_FB_TABLES round=%u ttbr0=%lx ttbr1=%lx mair=%lx after_sctlr=%lx after_tcr=%lx\n",
      J,R->Before.Ttbr0,R->Before.Ttbr1,R->Before.Mair,R->After.Sctlr,R->After.Tcr));
    DEBUG((DEBUG_WARN,"PIANO_FB_CPU_AFTER round=%u el=%lx sctlr=%lx tcr=%lx daif=%lx\n",
      J,R->After.El,R->After.Sctlr,R->After.Tcr,R->After.Daif));
    DEBUG((DEBUG_WARN,"PIANO_FB_TABLES_AFTER round=%u ttbr0=%lx ttbr1=%lx mair=%lx\n",
      J,R->After.Ttbr0,R->After.Ttbr1,R->After.Mair));
    for(UINT32 I=0;I<3;++I){CONST PIANO_FB_MAPPING_SAMPLE *P=&R->Sample[I];
      DEBUG((DEBUG_WARN,"PIANO_FB_GCD round=%u i=%u page=%lx status=%r type=%u base=%lx bytes=%lx\n",
        J,I,P->Address,P->GcdStatus,P->GcdType,P->GcdBase,P->GcdLength));
      DEBUG((DEBUG_WARN,"PIANO_FB_ATTR round=%u i=%u caps=%lx attrs=%lx read_protected=%u\n",J,I,P->GcdCapabilities,P->GcdAttributes,P->ReadProtected));
      DEBUG((DEBUG_WARN,"PIANO_FB_PAR round=%u i=%u status=%r raw=%lx physical_page=%lx attr=%02x fault=%u identity=%u\n",
        J,I,P->AtStatus,P->Par,P->PhysicalPage,P->Attribute,P->ParFault,P->Identity));
    }
  }
}
EFI_STATUS PianoFrameBufferMappingObserve(CONST CHAR8 *Phase,PIANO_FB_MAPPING_ALIVE Alive){
  if(!Phase||!Alive)return EFI_INVALID_PARAMETER;UINTN Length=0;while(Length<32&&Phase[Length])++Length;
  if(!Length||Length==32)return EFI_INVALID_PARAMETER;if(mBusy)return EFI_ALREADY_STARTED;
  if(mReport.ServicesLost)return EFI_NOT_READY;if(mReport.Count==PIANO_FB_MAPPING_PHASES)return EFI_OUT_OF_RESOURCES;
  mBusy=TRUE;PIANO_FB_MAPPING_SNAPSHOT *S=&mReport.Snapshot[mReport.Count++];ZeroMem(S,sizeof(*S));CopyMem(S->Phase,Phase,Length);
  for(UINT32 J=0;J<2;++J){S->Round[J].MetadataStatus=S->Round[J].CpuStatus=EFI_NOT_STARTED;
    for(UINT32 I=0;I<3;++I){S->Round[J].Sample[I].Address=mPages[I];S->Round[J].Sample[I].GcdStatus=S->Round[J].Sample[I].AtStatus=EFI_NOT_STARTED;}}
  S->Status=EFI_NOT_READY;if(!Live(Alive,S))goto Done;
  if(!gBS||!gDS||!gBS->RaiseTPL||!gBS->RestoreTPL||!gBS->LocateProtocol||!gDS->GetMemorySpaceDescriptor)goto Done;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);if(!Live(Alive,S))goto Done;
  gBS->RestoreTPL(Tpl);if(!Live(Alive,S))goto Done;if(Tpl!=TPL_APPLICATION){S->Status=EFI_UNSUPPORTED;goto Done;}
  S->Status=EFI_SUCCESS;
  for(UINT32 J=0;J<2;++J){PIANO_FB_MAPPING_ROUND *R=&S->Round[J];++S->Rounds;
    R->MetadataStatus=Metadata(Alive,S,R);if(R->MetadataStatus!=EFI_SUCCESS){S->Status=R->MetadataStatus;goto Done;}
    R->CpuStatus=Cpu(&R->Before);if(!Live(Alive,S))goto Done;if(R->CpuStatus!=EFI_SUCCESS||!ValidCpu(&R->Before)){S->Status=R->CpuStatus==EFI_SUCCESS?EFI_UNSUPPORTED:Exact(R->CpuStatus);goto Done;}
    for(UINT32 I=0;I<3;++I){PIANO_FB_MAPPING_SAMPLE *P=&R->Sample[I];P->Address=mPages[I];P->GcdStatus=P->AtStatus=EFI_NOT_STARTED;
      EFI_GCD_MEMORY_SPACE_DESCRIPTOR D={0};if(!Live(Alive,S))goto Done;
      P->GcdStatus=gDS->GetMemorySpaceDescriptor(P->Address,&D);if(!Live(Alive,S))goto Done;
      P->GcdType=(UINT32)D.GcdMemoryType;P->GcdBase=D.BaseAddress;P->GcdLength=D.Length;P->GcdCapabilities=D.Capabilities;P->GcdAttributes=D.Attributes;P->ReadProtected=(D.Attributes&EFI_MEMORY_RP)!=0;
      // WT/BB and all other raw attributes are observations, never permissions.
      // AT is independent CPU metadata even if GCD data is unavailable.
      P->AtStatus=At(P->Address,&R->Before,&P->Par,&R->After);if(!Live(Alive,S))goto Done;
      P->ParFault=(P->Par&1)!=0;P->PhysicalPage=P->Par&0x000ffffffffff000ULL;P->Attribute=(UINT8)(P->Par>>56);P->Identity=P->AtStatus==EFI_SUCCESS&&!P->ParFault&&P->PhysicalPage==P->Address;
      ++S->SampleCount;
      if(P->AtStatus!=EFI_SUCCESS){S->Status=Exact(P->AtStatus);goto Done;}
      if(S->Status==EFI_SUCCESS && P->GcdStatus!=EFI_SUCCESS)S->Status=Exact(P->GcdStatus);
      if(S->Status==EFI_SUCCESS && P->AtStatus!=EFI_SUCCESS)S->Status=Exact(P->AtStatus);
      if(S->Status==EFI_SUCCESS && !P->Identity)S->Status=EFI_NOT_READY;
    }
    R->CpuStatus=Cpu(&R->After);if(!Live(Alive,S))goto Done;
    if(S->Status==EFI_SUCCESS && (R->CpuStatus!=EFI_SUCCESS||CompareMem(&R->Before,&R->After,sizeof(R->Before))))S->Status=R->CpuStatus==EFI_SUCCESS?EFI_NOT_READY:Exact(R->CpuStatus);
  }
  if(!CompareMem(&S->Round[0],&S->Round[1],sizeof(S->Round[0])))S->Coherent=TRUE;
  else if(S->Status==EFI_SUCCESS)S->Status=EFI_NOT_READY;
Done:
  if(!S->ServicesLost && Live(Alive,S))Emit(S);mBusy=FALSE;return S->Status;
}
EFI_STATUS PianoFrameBufferMappingReemit(PIANO_FB_MAPPING_ALIVE Alive){
  if(!Alive)return EFI_INVALID_PARAMETER;if(mBusy)return EFI_ALREADY_STARTED;
  if(!Live(Alive,NULL))return EFI_ABORTED;mBusy=TRUE;
  for(UINT32 I=0;I<mReport.Count;++I){if(!Live(Alive,NULL)){mBusy=FALSE;return EFI_ABORTED;}Emit(&mReport.Snapshot[I]);}
  mBusy=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoFrameBufferMappingRetained(VOID){return mReport.ServicesLost;}
CONST PIANO_FB_MAPPING_REPORT *PianoFrameBufferMappingGetReport(VOID){return &mReport;}
