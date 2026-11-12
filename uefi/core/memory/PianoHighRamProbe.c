// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoHighRamProbe.h"

#define ADDRESS_MASK 0x0000FFFFFFFFF000ULL
typedef struct { UINTN Bytes,Stride,Key; UINT32 Version; EFI_STATUS Status; } MAP;
STATIC VOID Clear(VOID *P,UINTN N) { UINTN I;for(I=0;I<N;I++) { ((UINT8 *)P)[I]=0; } }
STATIC VOID Copy(VOID *D,CONST VOID *S,UINTN N) { UINTN I;for(I=0;I<N;I++) { ((UINT8 *)D)[I]=((CONST UINT8 *)S)[I]; } }
STATIC BOOLEAN Equal(CONST VOID *A,CONST VOID *B,UINTN N) { UINTN I;for(I=0;I<N;I++) { if(((CONST UINT8 *)A)[I]!=((CONST UINT8 *)B)[I])return FALSE; }return TRUE; }
// Public result statuses never expose a warning as apparent success. This is
// classification for normalization, not an EFI_ERROR-based acceptance check.
STATIC EFI_STATUS PublicStatus(EFI_STATUS Raw) { return Raw==EFI_SUCCESS?EFI_SUCCESS:EFI_ERROR(Raw)?Raw:EFI_DEVICE_ERROR; }
STATIC BOOLEAN Span(UINT64 Base,UINT64 Bytes,UINT64 Address,UINT64 Size) {
  return Bytes!=0 && Base<=MAX_UINT64-Bytes && Address>=Base && Size!=0 && Address<=MAX_UINT64-Size && Address+Size<=Base+Bytes;
}
STATIC UINT64 PaLimit(CONST PIANO_HIGH_RAM_CPU *C) {
  STATIC CONST UINT8 Widths[6]={32,36,40,42,44,48};
  UINT32 Ips=(UINT32)((C->Tcr>>32)&7);return Ips<6?(1ULL<<Widths[Ips]):0;
}
STATIC BOOLEAN ParAddressValid(CONST PIANO_HIGH_RAM_CPU *C,UINT64 Par) {
  // Include all possible 52-bit PAR OA bits in the rejection check, even
  // though this decoder only accepts the current classic <=48-bit regime.
  return PaLimit(C)!=0 && (Par&0x000FFFFFFFFFF000ULL)<PaLimit(C);
}
STATIC EFI_STATUS CpuValid(CONST PIANO_HIGH_RAM_CPU *C,UINT32 *Bits,UINT32 *StartLevel) {
  UINT32 T0=(UINT32)(C->Tcr&63),Ips=(UINT32)((C->Tcr>>32)&7);
  // Current pinned EL1 4K classic tables only. HA/HD could update descriptor
  // access/dirty bits; reject them in this read-only profile. DS/LPA2, endianness
  // and extended permission formats need a separate audited decoder.
  if(C->CurrentEl!=4 || !(C->Sctlr&1) || (C->Sctlr&BIT25) ||
     ((C->Tcr>>14)&3)!=0 || (C->Tcr&BIT7) || (C->Tcr&(BIT39|BIT40|BIT59)) ||
     T0<16 || T0>39 || Ips>5 || (C->Ttbr0&0xffe) ||
     (C->Ttbr0&ADDRESS_MASK)>=PaLimit(C)) return EFI_UNSUPPORTED;
  *Bits=64-T0;*StartLevel=(T0-16)/9;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS CaptureMap(CONST PIANO_HIGH_RAM_ENV *E,MAP *M) {
  Clear(M,sizeof(*M));M->Status=EFI_NOT_READY;
  if(E->Boot==NULL || E->Boot->GetMemoryMap==NULL || E->MapBuffer==NULL || (UINTN)E->MapBuffer%8 || E->MapCapacity<40) return M->Status;
  M->Bytes=E->MapCapacity;
  M->Status=E->Boot->GetMemoryMap(&M->Bytes,E->MapBuffer,&M->Key,&M->Stride,&M->Version);
  if(M->Status!=EFI_SUCCESS)return M->Status;
  if(M->Bytes>E->MapCapacity || M->Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || M->Stride>256 ||
     M->Stride%8 || !M->Bytes || M->Bytes%M->Stride || M->Version!=1 || M->Bytes/M->Stride>256) return M->Status=EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<M->Bytes;I+=M->Stride) {
    EFI_MEMORY_DESCRIPTOR A;Copy(&A,(UINT8 *)E->MapBuffer+I,sizeof(A));
    if(!A.NumberOfPages || A.NumberOfPages>MAX_UINT64/4096 || A.PhysicalStart%4096 ||
       A.PhysicalStart>MAX_UINT64-A.NumberOfPages*4096) return M->Status=EFI_COMPROMISED_DATA;
    for(UINTN J=0;J<I;J+=M->Stride) {
      EFI_MEMORY_DESCRIPTOR B;Copy(&B,(UINT8 *)E->MapBuffer+J,sizeof(B));
      if(A.PhysicalStart<B.PhysicalStart+B.NumberOfPages*4096 &&
         B.PhysicalStart<A.PhysicalStart+A.NumberOfPages*4096) return M->Status=EFI_COMPROMISED_DATA;
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindEfi(CONST PIANO_HIGH_RAM_ENV *E,CONST MAP *M,UINT64 Pa,UINT64 Bytes,EFI_MEMORY_DESCRIPTOR *D) {
  Clear(D,sizeof(*D));if(M->Status!=EFI_SUCCESS)return M->Status;
  for(UINTN I=0;I<M->Bytes;I+=M->Stride) {
    EFI_MEMORY_DESCRIPTOR R;Copy(&R,(UINT8 *)E->MapBuffer+I,sizeof(R));
    if(Span(R.PhysicalStart,R.NumberOfPages*4096,Pa,Bytes)) { *D=R;return EFI_SUCCESS; }
  }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS Gcd(CONST PIANO_HIGH_RAM_ENV *E,UINT64 Pa,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  Clear(D,sizeof(*D));
  if(E->Dxe==NULL || E->Dxe->GetMemorySpaceDescriptor==NULL)return EFI_NOT_READY;
  EFI_STATUS S=E->Dxe->GetMemorySpaceDescriptor(Pa,D);
  if(S==EFI_SUCCESS && !Span(D->BaseAddress,D->Length,Pa,1))return EFI_COMPROMISED_DATA;
  return S;
}
STATIC UINT64 ParPa(UINT64 Par,UINT64 Va) { return (Par&ADDRESS_MASK)|(Va&4095); }
STATIC UINT64 LevelSize(UINT32 Level) { return 1ULL<<(39-9*Level); }
STATIC EFI_STATUS TableWord(CONST PIANO_HIGH_RAM_ENV *E,CONST MAP *M,CONST PIANO_HIGH_RAM_CPU *Cpu,UINT64 Address,
                            UINT64 *Par,UINT64 *Value,UINT64 *Reasons) {
  EFI_MEMORY_DESCRIPTOR D;EFI_STATUS S;
  if(E->LowTablePagesAndRecoveryVerified!=TRUE || E->ReadTableWord==NULL ||
     E->ClassicEl1Stage1PhysicalContractVerified!=TRUE) {
    *Reasons|=PianoHighTableGuardUnknown;return EFI_NOT_READY;
  }
  // Whitelisted low heap excludes known start/hwfence carveout conflicts.
  if(!Span(PIANO_HIGH_RAM_TABLE_LOW,PIANO_HIGH_RAM_TABLE_END-PIANO_HIGH_RAM_TABLE_LOW,Address,8) ||
     (Address>=0xD4E23000ULL && Address<0xD5100000ULL) || Address%8) {
    *Reasons|=PianoHighTableOwnerUnknown;return EFI_ACCESS_DENIED;
  }
  S=FindEfi(E,M,Address&~4095ULL,4096,&D);
  if(S!=EFI_SUCCESS || (D.Type!=EfiBootServicesData && D.Type!=EfiLoaderData) || !(D.Attribute&EFI_MEMORY_WB)) {
    *Reasons|=PianoHighTableOwnerUnknown;return EFI_ACCESS_DENIED;
  }
  S=E->AtRead(E->Context,Address,Par);
  if(S!=EFI_SUCCESS || (*Par&1) || !ParAddressValid(Cpu,*Par) || ParPa(*Par,Address)!=Address ||
     ((*Par>>56)&255)!=0xff) {
    *Reasons|=PianoHighTableOwnerUnknown;return EFI_ACCESS_DENIED;
  }
  S=E->ReadTableWord(E->Context,Address,Value);
  if(S!=EFI_SUCCESS)*Reasons|=PianoHighTableReadFailed;
  return S;
}
STATIC EFI_STATUS Walk(CONST PIANO_HIGH_RAM_ENV *E,CONST MAP *M,CONST PIANO_HIGH_RAM_CPU *Cpu,PIANO_HIGH_RAM_ROW *R) {
  UINT32 Bits,First;EFI_STATUS S=CpuValid(Cpu,&Bits,&First);UINT64 Table=Cpu->Ttbr0&ADDRESS_MASK,Parents=0;
  if(S!=EFI_SUCCESS || R->Va>=(1ULL<<Bits)) { R->Reasons|=PianoHighCpuUnknown;return EFI_UNSUPPORTED; }
  for(UINT32 Level=First;Level<=3;Level++) {
    UINT64 Block=LevelSize(Level),Index=(R->Va>>(39-9*Level))&511,Value=0,Par=0;
    if(Table>MAX_UINT64-Index*8) { R->Reasons|=PianoHighInvalidPte;return EFI_COMPROMISED_DATA; }
    PIANO_HIGH_RAM_PTE *P=&R->Pte[R->Depth];P->PteVa=Table+Index*8;P->Level=Level;R->Depth++;
    S=TableWord(E,M,Cpu,P->PteVa,&Par,&Value,&R->Reasons);P->TablePar=Par;P->Value=Value;
    if(S!=EFI_SUCCESS)return S;
    UINT64 Type=Value&3;
    if(Type==3 && Level<3) {
      // Reject output/table address bits unsupported by this classic decoder.
      if((Value&0x000F000000000000ULL) || (Value&ADDRESS_MASK)>=PaLimit(Cpu)) {
        R->Reasons|=PianoHighInvalidPte;return EFI_UNSUPPORTED;
      }
      if(!(Cpu->Tcr&BIT41))Parents|=Value&(BIT59|BIT60|BIT61|BIT62);
      Table=Value&ADDRESS_MASK;continue;
    }
    R->Granule=Block;
    if(!((Type==1 && Level>0 && Level<3)||(Type==3 && Level==3))) {
      R->Reasons|=PianoHighInvalidPte;return EFI_NOT_FOUND;
    }
    if((Value&ADDRESS_MASK)&(Block-1) || (Value&0x000F000000000000ULL) ||
       (Value&ADDRESS_MASK)>PaLimit(Cpu)-Block) {
      R->Reasons|=PianoHighInvalidPte;return EFI_COMPROMISED_DATA;
    }
    R->Leaf=Value;R->Hierarchical=Parents;R->WalkPa=(Value&ADDRESS_MASK)|(R->Va&(Block-1));
    R->AttrIndex=(UINT8)((Value>>2)&7);R->MairByte=(UINT8)(Cpu->Mair>>(R->AttrIndex*8));
    R->Shareability=(UINT8)((Value>>8)&3);R->Ap=(UINT8)((Value>>6)&3);R->Af=(Value&BIT10)!=0;
    R->Pxn=(Value&BIT53)!=0 || (Parents&BIT59)!=0;R->Uxn=(Value&BIT54)!=0 || (Parents&BIT60)!=0;
    R->HierReadOnly=(Parents&BIT62)!=0;
    if(!R->Af || R->MairByte!=0xff || R->Shareability!=3 || !R->Pxn || !R->Uxn ||
       (Value&BIT51))R->Reasons|=PianoHighAttrsUnknown;
    if((R->Ap&2) || R->HierReadOnly)R->Reasons|=PianoHighWritePermissionUnknown;
    return EFI_SUCCESS;
  }
  return EFI_COMPROMISED_DATA;
}
// Bounded no-library formatting. Each line is <256 bytes; all fields are raw
// evidence. Unknown bits also get explicit names, no successful-read claim.
typedef struct {CHAR8 Data[256];UINTN Used;} LINE;
STATIC VOID Text(LINE *L,CONST CHAR8 *S) { while(*S && L->Used+1<sizeof(L->Data))L->Data[L->Used++]=*S++;L->Data[L->Used]=0; }
STATIC VOID Hex(LINE *L,CONST CHAR8 *Name,UINT64 Value) {Text(L,Name);Text(L,"=0x");for(INTN I=15;I>=0;I--){CHAR8 C="0123456789abcdef"[(Value>>(I*4))&15];if(L->Used+1<sizeof(L->Data))L->Data[L->Used++]=C;}L->Data[L->Used]=0;Text(L," ");}
STATIC VOID Emit(CONST PIANO_HIGH_RAM_ENV *E,LINE *L) {if(E->Log!=NULL){Text(L,"\n");E->Log(E->Context,L->Data);}Clear(L,sizeof(*L));}
STATIC VOID LogRow(CONST PIANO_HIGH_RAM_ENV *E,CONST PIANO_HIGH_RAM_ROW *R) {
  LINE L;Clear(&L,sizeof(L));Text(&L,"PIANO_HIGH_RAM_AT ");Hex(&L,"va",R->Va);Hex(&L,"end",R->End);Hex(&L,"pa_from_par",R->PaFromPar);Hex(&L,"par",R->Par);Hex(&L,"status",R->AtStatus);Hex(&L,"unknown",R->Reasons);Emit(E,&L);
  Text(&L,"PIANO_HIGH_RAM_EFI ");Hex(&L,"status",R->EfiStatus);Hex(&L,"type",R->Efi.Type);Hex(&L,"base",R->Efi.PhysicalStart);Hex(&L,"pages",R->Efi.NumberOfPages);Hex(&L,"attrs",R->Efi.Attribute);Emit(E,&L);
  Text(&L,"PIANO_HIGH_RAM_GCD ");Hex(&L,"status",R->GcdStatus);Hex(&L,"type",R->Gcd.GcdMemoryType);Hex(&L,"base",R->Gcd.BaseAddress);Hex(&L,"length",R->Gcd.Length);Hex(&L,"capabilities",R->Gcd.Capabilities);Hex(&L,"attrs",R->Gcd.Attributes);Hex(&L,"owner",(UINTN)R->Gcd.ImageHandle);Emit(E,&L);
  Text(&L,"PIANO_HIGH_RAM_MEMORY_ATTRIBUTE ");Hex(&L,"status",R->MemoryAttributeStatus);Hex(&L,"attrs",R->MemoryAttributes);Hex(&L,"sample_bytes",4096);Emit(E,&L);
  Text(&L,"PIANO_HIGH_RAM_WALK ");Hex(&L,"status",R->WalkStatus);Hex(&L,"pa",R->WalkPa);Hex(&L,"block_bytes",R->Granule);Hex(&L,"leaf",R->Leaf);Hex(&L,"parents",R->Hierarchical);Hex(&L,"mair_byte",R->MairByte);Hex(&L,"ap",R->Ap);Emit(E,&L);
  for(UINT32 I=0;I<R->Depth;I++) {Text(&L,"PIANO_HIGH_RAM_PTE ");Hex(&L,"level",R->Pte[I].Level);Hex(&L,"va",R->Pte[I].PteVa);Hex(&L,"par",R->Pte[I].TablePar);Hex(&L,"value",R->Pte[I].Value);Emit(E,&L);}
  STATIC CONST struct {UINT64 Bit;CONST CHAR8 *Name;} Names[]={
    {PianoHighCpuUnknown,"unsupported_cpu_or_table_format"},{PianoHighMapUnknown,"efi_snapshot_missing_or_invalid"},
    {PianoHighGcdUnknown,"gcd_missing_or_invalid"},{PianoHighTargetNotOccupied,"target_not_type2_allocated_wb"},
    {PianoHighAtFault,"target_at_fault"},{PianoHighTableGuardUnknown,"low_table_guard_or_regime_unverified"},
    {PianoHighTableOwnerUnknown,"table_page_owner_translation_or_cache_unknown"},{PianoHighTableReadFailed,"protected_table_read_failed"},
    {PianoHighInvalidPte,"invalid_or_unsupported_pte"},{PianoHighTranslationMismatch,"pte_at_identity_mismatch"},
    {PianoHighAttrsUnknown,"af_cache_or_shareability_unknown"},{PianoHighPhysicalRegimeUnknown,"stage1_output_physical_contract_unknown"},
    {PianoHighStateChanged,"cpu_translation_state_changed"},{PianoHighWritePermissionUnknown,"pte_write_permission_unknown"},
    {PianoHighOwnershipUnknown,"physical_phase_ownership_not_proven_by_translation"},
    {PianoHighMemoryAttributeUnknown,"efi_memory_attribute_provider_missing_unverified_or_failed"}};
  for(UINTN I=0;I<sizeof(Names)/sizeof(Names[0]);I++)if(R->Reasons&Names[I].Bit) {Text(&L,"PIANO_HIGH_RAM_UNKNOWN reason=");Text(&L,Names[I].Name);Emit(E,&L);}
}
STATIC VOID One(CONST PIANO_HIGH_RAM_ENV *E,CONST MAP *M,CONST PIANO_HIGH_RAM_CPU *Cpu,UINT64 Va,PIANO_HIGH_RAM_ROW *R) {
  Clear(R,sizeof(*R));R->Va=Va;R->End=Va+4096;
  R->AtStatus=E->AtRead(E->Context,Va,&R->Par);
  if(R->AtStatus!=EFI_SUCCESS || (R->Par&1) || !ParAddressValid(Cpu,R->Par))R->Reasons|=PianoHighAtFault;
  else R->PaFromPar=ParPa(R->Par,Va);
  R->EfiStatus=FindEfi(E,M,Va,4096,&R->Efi);
  if(R->EfiStatus!=EFI_SUCCESS)R->Reasons|=PianoHighMapUnknown;
  R->GcdStatus=Gcd(E,Va,&R->Gcd);if(R->GcdStatus!=EFI_SUCCESS)R->Reasons|=PianoHighGcdUnknown;
  R->MemoryAttributeStatus=EFI_NOT_READY;
  if(E->MemoryAttributeProviderVerified==TRUE && E->MemoryAttribute!=NULL && E->MemoryAttribute->GetMemoryAttributes!=NULL)
    R->MemoryAttributeStatus=E->MemoryAttribute->GetMemoryAttributes(E->MemoryAttribute,Va,4096,&R->MemoryAttributes);
  if(R->MemoryAttributeStatus!=EFI_SUCCESS)R->Reasons|=PianoHighMemoryAttributeUnknown;
  if(R->EfiStatus!=EFI_SUCCESS || R->Efi.Type!=EfiLoaderData || !(R->Efi.Attribute&EFI_MEMORY_WB) ||
     R->GcdStatus!=EFI_SUCCESS || R->Gcd.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory || R->Gcd.ImageHandle==NULL)
    R->Reasons|=PianoHighTargetNotOccupied;
  if(E->ClassicEl1Stage1PhysicalContractVerified!=TRUE)R->Reasons|=PianoHighPhysicalRegimeUnknown;
  R->WalkStatus=Walk(E,M,Cpu,R);
  if(R->WalkStatus==EFI_SUCCESS && !(R->Reasons&PianoHighAtFault)) {
    if(R->WalkPa!=R->PaFromPar || R->WalkPa!=Va || ((R->Par>>56)&255)!=R->MairByte ||
       ((R->Par>>7)&3)!=R->Shareability)R->Reasons|=PianoHighTranslationMismatch;
    if(R->MemoryAttributeStatus==EFI_SUCCESS) {
      if((R->MemoryAttributes&EFI_MEMORY_RP) || ((R->MemoryAttributes&EFI_MEMORY_XP)!=0)!=R->Pxn)
        R->Reasons|=PianoHighAttrsUnknown;
      if(R->MemoryAttributes&EFI_MEMORY_RO)R->Reasons|=PianoHighWritePermissionUnknown;
    }
  }
  if(R->Granule)R->End=(Va&~(R->Granule-1))+R->Granule;
  if(R->End>PIANO_HIGH_RAM_END)R->End=PIANO_HIGH_RAM_END;
  // Split descriptor/GCD boundaries, including holes, before saying segment
  // has these firmware attributes. No unknown span inherits prior evidence.
  if(R->EfiStatus==EFI_SUCCESS && R->Efi.PhysicalStart+R->Efi.NumberOfPages*4096<R->End)
    R->End=R->Efi.PhysicalStart+R->Efi.NumberOfPages*4096;
  if(M->Status==EFI_SUCCESS)for(UINTN I=0;I<M->Bytes;I+=M->Stride) {
    EFI_MEMORY_DESCRIPTOR D;Copy(&D,(UINT8 *)E->MapBuffer+I,sizeof(D));
    UINT64 End=D.PhysicalStart+D.NumberOfPages*4096;
    if(D.PhysicalStart>Va && D.PhysicalStart<R->End)R->End=D.PhysicalStart;
    if(End>Va && End<R->End)R->End=End;
  }
  if(R->GcdStatus==EFI_SUCCESS && R->Gcd.BaseAddress+R->Gcd.Length<R->End)R->End=R->Gcd.BaseAddress+R->Gcd.Length;
  if(R->End<=Va || R->End%4096) {R->End=Va+4096;R->Reasons|=PianoHighAttrsUnknown;}
}
EFI_STATUS PianoHighRamProbe(CONST PIANO_HIGH_RAM_ENV *E,PIANO_HIGH_RAM_RESULT *Result) {
  MAP M;UINT32 Bits,First;UINT64 Va=PIANO_HIGH_RAM_BASE;EFI_STATUS S;
  if(Result==NULL)return EFI_INVALID_PARAMETER;Clear(Result,sizeof(*Result));Result->CoveredEnd=Va;
  if(!PIANO_HIGH_RAM_PROBE_EXPERIMENT || E==NULL || E->ExplicitEnable!=TRUE) {Result->Reasons=PianoHighDisabled;return EFI_UNSUPPORTED;}
  if(E->State==NULL || E->AtRead==NULL || E->RowBudget==0 || E->RowBudget>PIANO_HIGH_RAM_MAX_ROWS)return EFI_INVALID_PARAMETER;
  S=E->State(E->Context,&Result->Cpu);
  if(S!=EFI_SUCCESS || CpuValid(&Result->Cpu,&Bits,&First)!=EFI_SUCCESS || PIANO_HIGH_RAM_END>(1ULL<<Bits)) {
    Result->Reasons=PianoHighCpuUnknown;return EFI_UNSUPPORTED;
  }
  CaptureMap(E,&M);Result->MapStatus=M.Status;Result->MapBytes=M.Bytes;Result->DescriptorBytes=M.Stride;Result->DescriptorVersion=M.Version;
  while(Va<PIANO_HIGH_RAM_END && Result->Rows<E->RowBudget) {
    PIANO_HIGH_RAM_ROW R;PIANO_HIGH_RAM_CPU After;One(E,&M,&Result->Cpu,Va,&R);
    S=E->State(E->Context,&After);
    if(S!=EFI_SUCCESS || !Equal(&After,&Result->Cpu,sizeof(After)))R.Reasons|=PianoHighStateChanged;
    R.Reasons|=PianoHighOwnershipUnknown; // EFI/GCD/AT metadata cannot prove phase ownership.
    Result->Reasons|=R.Reasons;Result->Rows++;Result->CoveredEnd=R.End;LogRow(E,&R);Va=R.End;
    if(R.Reasons&PianoHighStateChanged)break;
  }
  Result->Complete=Va==PIANO_HIGH_RAM_END;
  if(!Result->Complete)Result->Reasons|=PianoHighBudget;
  // These remain false even for fully mapped metadata; no high content read.
  Result->TranslationMetadataConsistent=Result->Complete && !(Result->Reasons&~(UINT64)PianoHighOwnershipUnknown);
  return Result->TranslationMetadataConsistent?EFI_SUCCESS:EFI_NOT_READY;
}

EFI_STATUS EFIAPI PianoHighRamArchitectureState(VOID *Context,PIANO_HIGH_RAM_CPU *Cpu) {
  (void)Context;if(Cpu==NULL)return EFI_INVALID_PARAMETER;
#if defined(__aarch64__) && PIANO_HIGH_RAM_PROBE_EXPERIMENT
  __asm__ volatile("mrs %0, CurrentEL":"=r"(Cpu->CurrentEl));
  if(Cpu->CurrentEl!=4)return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, mair_el1"
    :"=r"(Cpu->Sctlr),"=r"(Cpu->Tcr),"=r"(Cpu->Ttbr0),"=r"(Cpu->Mair)::"memory");
  return EFI_SUCCESS;
#else
  return EFI_UNSUPPORTED;
#endif
}
EFI_STATUS EFIAPI PianoHighRamArchitectureAtRead(VOID *Context,UINT64 Va,UINT64 *Par) {
  (void)Context;(void)Va;if(Par==NULL)return EFI_INVALID_PARAMETER;
#if defined(__aarch64__) && PIANO_HIGH_RAM_PROBE_EXPERIMENT
  PIANO_HIGH_RAM_CPU Cpu;UINT32 Bits,First;UINT64 Old,Mask;
  EFI_STATUS S=PianoHighRamArchitectureState(NULL,&Cpu);
  if(S!=EFI_SUCCESS || CpuValid(&Cpu,&Bits,&First)!=EFI_SUCCESS || Va>=(1ULL<<Bits))return EFI_UNSUPPORTED;
  // Short DAIF mask protects this CPU's PAR scratch and restores exact DAIF.
  // No TCR/TTBR/MAIR/SCTLR or memory map changes, no target dereference.
  __asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1r, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
    :"=&r"(Mask),"=&r"(Old),"=&r"(*Par):"r"(Va):"memory");return EFI_SUCCESS;
#else
  return EFI_UNSUPPORTED;
#endif
}
EFI_STATUS EFIAPI PianoHighRamArchitectureAtWrite(VOID *Context,UINT64 Va,UINT64 *Par) {
  (void)Context;(void)Va;if(Par==NULL)return EFI_INVALID_PARAMETER;
#if defined(__aarch64__) && PIANO_HIGH_RAM_PROBE_EXPERIMENT && PIANO_HIGH_RAM_PATTERN_EXPERIMENT
  PIANO_HIGH_RAM_CPU Cpu;UINT32 Bits,First;UINT64 Old,Mask;
  EFI_STATUS S=PianoHighRamArchitectureState(NULL,&Cpu);
  if(S!=EFI_SUCCESS || CpuValid(&Cpu,&Bits,&First)!=EFI_SUCCESS || Va>=(1ULL<<Bits))return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1w, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
    :"=&r"(Mask),"=&r"(Old),"=&r"(*Par):"r"(Va):"memory");return EFI_SUCCESS;
#else
  return EFI_UNSUPPORTED;
#endif
}

EFI_STATUS PianoHighRamPattern4K(CONST PIANO_HIGH_RAM_ENV *E,CONST PIANO_HIGH_RAM_PATTERN *P,
                                UINT64 Va,PIANO_HIGH_RAM_PATTERN_RESULT *Result) {
  if(Result==NULL)return EFI_INVALID_PARAMETER;Clear(Result,sizeof(*Result));Result->Status=EFI_UNSUPPORTED;Result->RestoreStatus=EFI_NOT_STARTED;
  if(!PIANO_HIGH_RAM_PROBE_EXPERIMENT || !PIANO_HIGH_RAM_PATTERN_EXPERIMENT) {Result->Reasons=PianoHighPatternDisabled;return Result->Status;}
  if(E==NULL || P==NULL || E->ExplicitEnable!=TRUE || P->ExplicitEnable!=TRUE ||
     P->ExclusivePhaseOwnershipVerified!=TRUE || P->DmaAndOtherCpuQuiescedVerified!=TRUE ||
     P->CacheAndAliasContractVerified!=TRUE || P->FaultAndRestorationContractVerified!=TRUE ||
     P->SnapshotBufferLowOwnedVerified!=TRUE || E->ClassicEl1Stage1PhysicalContractVerified!=TRUE ||
     E->LowTablePagesAndRecoveryVerified!=TRUE) {Result->Reasons=PianoHighOwnershipUnknown;return Result->Status=EFI_NOT_READY;}
  if(E->State==NULL || E->AtRead==NULL || E->AtWrite==NULL || P->Read==NULL || P->Write==NULL ||
     P->CleanToPoc==NULL || P->GcdOwner==NULL || P->SavedPage==NULL || P->WorkPage==NULL || P->BufferBytes!=4096 ||
     Va%4096 || !Span(PIANO_HIGH_RAM_BASE,PIANO_HIGH_RAM_END-PIANO_HIGH_RAM_BASE,Va,4096))return Result->Status=EFI_INVALID_PARAMETER;
  UINT64 Saved=(UINTN)P->SavedPage,Work=(UINTN)P->WorkPage;
  if(Saved>MAX_UINT64-4096 || Work>MAX_UINT64-4096 || !(Saved+4096<=Work || Work+4096<=Saved) ||
     Saved+4096>PIANO_HIGH_RAM_BASE || Work+4096>PIANO_HIGH_RAM_BASE)return Result->Status=EFI_INVALID_PARAMETER;
  MAP M;PIANO_HIGH_RAM_CPU Cpu,After;PIANO_HIGH_RAM_ROW R;UINT64 WritePar=0;
  EFI_STATUS S=E->State(E->Context,&Cpu);
  if(S!=EFI_SUCCESS) {Result->RawStatus=S;return Result->Status=PublicStatus(S);}
  UINT32 Bits,First;
  if(CpuValid(&Cpu,&Bits,&First)!=EFI_SUCCESS || Va>=(1ULL<<Bits)) {
    Result->Reasons=PianoHighCpuUnknown;return Result->Status=EFI_UNSUPPORTED;
  }
  if(CaptureMap(E,&M)!=EFI_SUCCESS) {Result->RawStatus=M.Status;Result->Reasons=PianoHighMapUnknown;return Result->Status=EFI_NOT_READY;}
  One(E,&M,&Cpu,Va,&R);LogRow(E,&R);
  if(R.Reasons || R.WalkStatus!=EFI_SUCCESS || R.Gcd.ImageHandle!=P->GcdOwner ||
     !Span(R.Efi.PhysicalStart,R.Efi.NumberOfPages*4096,Va,4096) || !Span(R.Gcd.BaseAddress,R.Gcd.Length,Va,4096)) {
    Result->Reasons=R.Reasons|PianoHighOwnershipUnknown;return Result->Status=EFI_NOT_READY;
  }
  S=E->AtWrite(E->Context,Va,&WritePar);
  if(S!=EFI_SUCCESS || (WritePar&1) || !ParAddressValid(&Cpu,WritePar) || ParPa(WritePar,Va)!=Va || ((WritePar>>56)&255)!=0xff) {
    Result->RawStatus=S;Result->Reasons=PianoHighWritePermissionUnknown;return Result->Status=EFI_ACCESS_DENIED;
  }
  S=E->State(E->Context,&After);if(S!=EFI_SUCCESS || !Equal(&Cpu,&After,sizeof(Cpu))) {
    Result->RawStatus=S;Result->Reasons=PianoHighStateChanged;return Result->Status=EFI_NOT_READY;
  }
  S=P->Read(E->Context,Va,P->SavedPage,4096);
  if(S!=EFI_SUCCESS) {Result->RawStatus=S;return Result->Status=PublicStatus(S);}
  Result->SaveCompleted=TRUE;
  for(UINTN I=0;I<4096;I++)P->WorkPage[I]=(UINT8)(0xa5^(I&255)^((I>>8)&255));
  Result->WriteAttempted=TRUE;S=P->Write(E->Context,Va,P->WorkPage,4096);
  if(S==EFI_SUCCESS)S=P->CleanToPoc(E->Context,Va,4096);
  if(S==EFI_SUCCESS)S=P->Read(E->Context,Va,P->WorkPage,4096);
  if(S==EFI_SUCCESS) {
    for(UINTN I=0;I<4096;I++)if(P->WorkPage[I]!=(UINT8)(0xa5^(I&255)^((I>>8)&255))) {S=EFI_COMPROMISED_DATA;break;}
    Result->PatternCompared=S==EFI_SUCCESS;
  }
  Result->RawStatus=S;
  // Even a failed/partial write triggers exactly one restore attempt. There is
  // no retry loop, cache manipulation backend or reset implementation here.
  Result->RestoreAttempted=TRUE;Result->RestoreStatus=P->Write(E->Context,Va,P->SavedPage,4096);
  EFI_STATUS Clean=P->CleanToPoc(E->Context,Va,4096);
  EFI_STATUS Read=P->Read(E->Context,Va,P->WorkPage,4096);
  if(Result->RestoreStatus==EFI_SUCCESS && Clean!=EFI_SUCCESS)Result->RestoreStatus=Clean;
  if(Result->RestoreStatus==EFI_SUCCESS && Read!=EFI_SUCCESS)Result->RestoreStatus=Read;
  if(Result->RestoreStatus==EFI_SUCCESS && !Equal(P->SavedPage,P->WorkPage,4096))Result->RestoreStatus=EFI_COMPROMISED_DATA;
  Result->RestoreRawStatus=Result->RestoreStatus;Result->RestoreStatus=PublicStatus(Result->RestoreRawStatus);
  Result->RestoreCompared=Result->RestoreRawStatus==EFI_SUCCESS;
  if(!Result->RestoreCompared) {Result->Reasons|=PianoHighPatternRestoreFailed;S=EFI_ABORTED;}
  Clear(P->WorkPage,4096);
  // Keep SavedPage available to the owner's recovery on a failed restore.
  if(Result->RestoreCompared)Clear(P->SavedPage,4096);
  return Result->Status=PublicStatus(S);
}
