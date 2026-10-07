// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual modified SEC InitializeMemory + real Mu HOB/MemoryPeim source.
// MMU and target loads are injected host adapters, never hardware evidence.
#include "PianoFrozenLowFixture.h"
#define PIANO_EARLY_HOST_TEST 1
#include "../../uefi/handoff/early-memory/PianoEarlyMemory.c"
// Cold object implementation has its own actual native/guard/HOB tests. These
// boundary counters verify the generated real SEC calls each producer once.
static UINTN cold_object_observes,cold_object_publishes;
EFI_STATUS PianoColdBootObjectsObserve(VOID){assert(!hob_list&&!mmu_calls);cold_object_observes++;return EFI_NOT_READY;}
EFI_STATUS PianoColdBootObjectsPublishHob(VOID){assert(hob_list&&!mmu_calls);cold_object_publishes++;return EFI_SUCCESS;}
#define HobConstructor HostArena
#include "PianoBoundSecMemory.h"
#undef HobConstructor
static UINT8 smem_bytes[PIANO_SMEM_BYTES];
static UINT64 cpu_el=4,cpu_sctlr=0,cpu_vbar=0x2000,cpu_daif=0x3c0;
static UINT64 cpu_spsel=1;
static UINT64 counter=1000,frequency=19200000;
static UINT64 failed_pa;
static UINT32 load_calls;
static BOOLEAN change_cpu;
static BOOLEAN second_payload_fault,second_payload_change,second_descriptor_fault;
static UINT32 payload_pass,descriptor_pass;
static unsigned cases;
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
EFI_STATUS PianoEarlyHostCpu(UINT64 *E,UINT64 *S,UINT64 *V,UINT64 *D,UINT64 *C,UINT64 *F,UINT64 *Sp){
  *E=cpu_el;*S=cpu_sctlr;*V=cpu_vbar;*D=cpu_daif;*C=counter++;*F=frequency;*Sp=cpu_spsel;return EFI_SUCCESS;
}
UINTN EFIAPI PianoSecRead32(UINT64 Address,UINT32 *Value,PIANO_SEC_READ_STATE *State){
  ++load_calls;State->Address=Address;State->OldVbar=cpu_vbar;State->OldDaif=cpu_daif;
  if(Address==PIANO_SMEM_BASE+0x3000){++payload_pass;if(payload_pass==2&&second_payload_change)smem_bytes[0x3040]^=1;}
  if(Address==PIANO_SMEM_BASE+0x8000)++descriptor_pass;
  if((second_payload_fault&&payload_pass==2&&Address==PIANO_SMEM_BASE+0x3004)||
     (second_descriptor_fault&&descriptor_pass==2&&Address==PIANO_SMEM_BASE+0x8014))failed_pa=Address;
  if(Address==failed_pa){State->Elr=0x1100;State->Esr=0x96000010;State->Far=Address;
    State->Spsr=0x3c5;State->Resume=0x1104;State->Faulted=1;return 1;}
  if(Address==PIANO_SMEM_COOKIE_LOW)*Value=0x81d08000;
  else if(Address==PIANO_SMEM_COOKIE_HIGH)*Value=0;
  else{assert(Address>=PIANO_SMEM_BASE&&Address+4<=PIANO_SMEM_BASE+PIANO_SMEM_BYTES);
    memcpy(Value,smem_bytes+Address-PIANO_SMEM_BASE,4);}
  if(change_cpu)cpu_sctlr|=1;
  return 0;
}
VOID PianoEarlyHostFatal(VOID){longjmp(jump,3);}
static void p32(UINT8 *P,UINT32 V){memcpy(P,&V,4);}
static void p64(UINT8 *P,UINT64 V){memcpy(P,&V,8);}
static void reset_cold(void){
  memset(&mReport,0,sizeof(mReport));memset(&mWork,0,sizeof(mWork));memset(mScratch,0,sizeof(mScratch));
  memset(smem_bytes,0,sizeof(smem_bytes));cpu_el=4;cpu_sctlr=0;cpu_vbar=0x2000;cpu_daif=0x3c0;
  cpu_spsel=1;
  counter=1000;frequency=19200000;load_calls=0;failed_pa=0;change_cpu=FALSE;
  second_payload_fault=second_payload_change=second_descriptor_fault=FALSE;payload_pass=descriptor_pass=0;
  p32(smem_bytes+0x5c,0xb0001);p32(smem_bytes+0xc0,1);
  p32(smem_bytes+0xc4,0x100000);p32(smem_bytes+0xc8,0x100000);
  UINT8 *T=smem_bytes+0xd0+402*16;p32(T,1);p32(T+4,0x3000);p32(T+8,168);
  UINT8 *P=smem_bytes+0x3000;p32(P,0x9da5e0a8);p32(P+4,0xaf9ec4e2);p32(P+8,2);p32(P+16,2);
  P+=24;p64(P+16,0x80000000);p64(P+24,0x400000000ULL);p32(P+36,14);p32(P+44,1);p64(P+64,0x3f0000000ULL);
  P+=72;p64(P+16,0xa7100000);p64(P+24,0x300000);p32(P+36,14);p32(P+44,5);
}
static void descriptor(void){
  UINT8 *D=smem_bytes+0x8000;p32(D,0x49494953);p32(D+4,PIANO_SMEM_BYTES);p64(D+8,PIANO_SMEM_BASE);
  p32(D+16,0x00010200);p32(D+20,0x000c4853);p64(D+24,0x1122334455667788ULL);
}
static void result(EFI_STATUS S){assert(PianoEarlyMemoryObserveCold()==S);assert(mReport.Status==S&&mReport.Finished);
  assert(!mReport.MemoryOwnershipGranted&&!mReport.HighDdrPublished);++cases;}
int main(void){
  reset_cold();assert(PianoEarlyMemoryPublishHob()==EFI_NOT_READY);++cases;
  result(EFI_SUCCESS);assert(mReport.ColdStateVerified&&mReport.Smem.Parsed&&mReport.Smem.BankCount==1);
  assert(mReport.Smem.Banks[0].AvailableLength==0x3f0000000ULL&&mReport.Smem.PreloadedCount==1);
  assert(mReport.Smem.CookieRepeatedEqual&&mReport.LoadCount==load_calls);++cases;
  assert(mReport.Version==2&&mReport.RawPayloadCoherent&&mReport.RawPayloadBytes==168&&
    mReport.RawPayloadCrc32==mReport.Smem.PayloadCrc32&&!memcmp(mReport.RawPayload,smem_bytes+0x3000,168));++cases;
  UINT32 before=load_calls;assert(PianoEarlyMemoryObserveCold()==EFI_ALREADY_STARTED&&load_calls==before);++cases;
  reset_cold();cpu_el=8;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();cpu_spsel=0;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();cpu_sctlr=1;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();cpu_sctlr=4;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();cpu_vbar=0;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();cpu_vbar=0x2010;result(EFI_UNSUPPORTED);assert(load_calls==0);++cases;
  reset_cold();frequency=0;result(EFI_TIMEOUT);assert(load_calls==0);++cases;
  reset_cold();frequency=1000000001ULL;result(EFI_TIMEOUT);assert(load_calls==0);++cases;
  reset_cold();failed_pa=PIANO_SMEM_BASE+0x5c;result(EFI_DEVICE_ERROR);
  assert(mReport.RecoveredFaults==1&&mReport.Smem.CallbackStatus==EFI_NOT_READY&&!mReport.Smem.Parsed);++cases;
  reset_cold();failed_pa=PIANO_SMEM_COOKIE_LOW;result(EFI_SUCCESS);
  assert(mReport.RecoveredFaults==1&&mReport.Smem.CookieStatus!=EFI_SUCCESS&&mReport.Smem.Parsed);
  assert(mReport.LastFault.Far==PIANO_SMEM_COOKIE_LOW&&mReport.LastFault.Elr==0x1100);++cases;
  reset_cold();change_cpu=TRUE;result(EFI_DEVICE_ERROR);assert(!mReport.Smem.Parsed);++cases;
  reset_cold();p32(smem_bytes+0x3000,0);result(EFI_COMPROMISED_DATA);++cases;
  reset_cold();p32(smem_bytes+0x3008,3);result(EFI_SUCCESS);assert(mReport.Smem.RamVersion==3);++cases;
  reset_cold();p32(smem_bytes+0x3008,4);result(EFI_UNSUPPORTED);++cases;
  assert(mReport.RawPayloadCoherent&&!mReport.Smem.Parsed&&!mReport.Smem.BankCount&&!mReport.Smem.PreloadedCount);++cases;
  reset_cold();descriptor();p32(smem_bytes+0x3008,3);p64(smem_bytes+0x3000+24+16,MAX_UINT64-3);p64(smem_bytes+0x3000+24+64,8);
  result(EFI_COMPROMISED_DATA);assert(mReport.Smem.Reason==PianoSmemReasonRamRange&&
    !mReport.Smem.Parsed&&!mReport.Smem.BankCount&&!mReport.Smem.PreloadedCount&&mReport.RawPayloadCoherent&&
    mReport.RawDescriptorCoherent&&mReport.RawDescriptorBytes==32&&mReport.Descriptor.RepeatedEqual&&
    !memcmp(mReport.RawDescriptor,smem_bytes+0x8000,32));++cases;
  reset_cold();descriptor();p32(smem_bytes+0x3008,3);p64(smem_bytes+0x3000+24+24,0);p64(smem_bytes+0x3000+24+64,0);
  result(EFI_SUCCESS);assert(mReport.Smem.Parsed&&mReport.Smem.BankCount==1&&!mReport.Smem.Banks[0].RawSize&&
    !mReport.Smem.Banks[0].AvailableLength&&mReport.RawPayloadCoherent);++cases;
  reset_cold();descriptor();second_payload_fault=TRUE;result(EFI_DEVICE_ERROR);
  assert(mReport.Smem.RepeatedMetadataEqual&&!mReport.Smem.RepeatedPayloadEqual&&!mReport.RawPayloadCoherent&&
    !mReport.RawPayloadBytes&&!mReport.RawPayloadCrc32&&!mReport.RawDescriptorCoherent);++cases;
  reset_cold();descriptor();second_payload_change=TRUE;result(EFI_MEDIA_CHANGED);
  assert(!mReport.RawPayloadCoherent&&!mReport.RawPayloadBytes&&!mReport.RawDescriptorCoherent);++cases;
  reset_cold();descriptor();second_descriptor_fault=TRUE;result(EFI_SUCCESS);
  assert(mReport.RawPayloadCoherent&&!mReport.RawDescriptorCoherent&&!mReport.RawDescriptorBytes&&
    !mReport.Descriptor.RepeatedEqual&&mReport.Descriptor.Status==EFI_NOT_READY);++cases;
  reset_cold();descriptor();p32(smem_bytes+0xd0+402*16+8,PIANO_SMEM_PAYLOAD_MAX);
  p32(smem_bytes+0x8014,((PIANO_EARLY_SIII_RAW_MAX-20)<<16)|0x4853);
  result(EFI_SUCCESS);assert(mReport.RawPayloadBytes==8192&&mReport.RawDescriptorBytes==2048&&
    mReport.RawPayloadCoherent&&mReport.RawDescriptorCoherent);
  assert(sizeof(mReport)+sizeof(EFI_HOB_GUID_TYPE)<65536);++cases;
  reset_cold();mReport.Attempted=TRUE;mReport.EntrySctlr=cpu_sctlr;mReport.EntryVbar=cpu_vbar;
  mReport.EntryDaif=cpu_daif;mStart=counter;mFrequency=frequency;
  UINT32 output[2]={0xa5a5a5a5,0xa5a5a5a5};failed_pa=PIANO_SMEM_BASE+4;
  assert(TryRead(&mReport,PIANO_SMEM_BASE,8,output)==EFI_NOT_READY);
  assert(output[0]==0xa5a5a5a5&&output[1]==0xa5a5a5a5);++cases;
  assert(TryRead(&mReport,0x90000000,4,output)==EFI_ACCESS_DENIED);++cases;
  assert(TryRead(&mReport,PIANO_SMEM_COOKIE_LOW,8,output)==EFI_ACCESS_DENIED);++cases;
  assert(TryRead(&mReport,PIANO_SMEM_BASE,3,output)==EFI_INVALID_PARAMETER);++cases;
  assert(TryRead(&mReport,PIANO_SMEM_BASE,4,&mReport)==EFI_ACCESS_DENIED);++cases;
  counter=mStart+frequency/10;assert(TryRead(&mReport,PIANO_SMEM_BASE,4,output)==EFI_TIMEOUT);++cases;
  counter=mStart-1;assert(TryRead(&mReport,PIANO_SMEM_BASE,4,output)==EFI_TIMEOUT);++cases;
  // Consume the actual product SEC body and actual HOB/MemoryPeim once.
  reset_cold();setup(FALSE);EFI_MEMORY_REGION_DESCRIPTOR *Product;UINT8 N;
  PianoProductLowGetMemoryMap(&Product,&N);memcpy(rows,Product,N*sizeof(*rows));count=N;hob_list=NULL;
  assert(PianoBoundInitializeMemory()==EFI_SUCCESS&&mmu_calls==1&&mReport.Published);++cases;
  assert(arena_base==0xBD980000&&arena_end==0xD4E23000&&!mReport.HighDdrPublished);++cases;
  unsigned diagnostic_hobs=0;EFI_PEI_HOB_POINTERS P;P.Raw=hob_list;
  while(!END_OF_HOB_LIST(P)){
    if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_GUID_EXTENSION&&!memcmp(&P.Guid->Name,&mHobGuid,sizeof(mHobGuid))){
      ++diagnostic_hobs;PIANO_EARLY_MEMORY_REPORT *R=GET_GUID_HOB_DATA(P.Guid);
      assert(!memcmp(R,&mReport,sizeof(mReport))&&R->ColdStateVerified&&R->Smem.Parsed&&!R->HighDdrPublished);}
    P.Raw=GET_NEXT_HOB(P);
  }
  assert(diagnostic_hobs==1&&PianoEarlyMemoryPublishHob()==EFI_NOT_READY);++cases;
  before=load_calls;cpu_sctlr=1;assert(PianoEarlyMemoryObserveCold()==EFI_ALREADY_STARTED&&load_calls==before);++cases;
  // Failed discovery still publishes evidence and uses the unchanged low map.
  reset_cold();failed_pa=PIANO_SMEM_BASE+0x5c;setup(FALSE);
  memcpy(rows,Product,N*sizeof(*rows));count=N;hob_list=NULL;
  assert(PianoBoundInitializeMemory()==EFI_SUCCESS&&mmu_calls==1&&mReport.Published);
  assert(mReport.Status==EFI_DEVICE_ERROR&&mReport.RecoveredFaults==1&&!mReport.HighDdrPublished);++cases;
  printf("Actual cold SEC observer: %u cases, actual first PHIT/HOB/MemoryPeim path, immutable failure/success HOB, no high DDR or hardware claim\n",cases);
  return 0;
}
