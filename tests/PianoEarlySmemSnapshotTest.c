// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual SEC producer -> serialized GUID HOB -> actual DXE consumer/replay.
// Reuse the real guarded-reader boundary fixtures, never hardware evidence.
#define main PianoOriginalProductSmemMain
#include "PianoProductSmemTest.c"
#undef main

static UINT32 cold_case,cold_loads,payload_pass,descriptor_pass,cookie_pass;
static UINT64 cold_counter=1000;
static BOOLEAN hob_allocation_failed;
static PIANO_EARLY_MEMORY_REPORT frozen;

VOID *EFIAPI BuildGuidHob(CONST EFI_GUID *Guid,UINTN Bytes){
  assert(!memcmp(Guid,&early_guid,sizeof(*Guid))&&Bytes==sizeof(early_hob.Report));
  assert(Bytes+sizeof(EFI_HOB_GUID_TYPE)<65536);
  if(hob_allocation_failed)return NULL;
  early_hob.Header=(EFI_HOB_GUID_TYPE){.Header={.HobType=EFI_HOB_TYPE_GUID_EXTENSION,
    .HobLength=sizeof(early_hob)},.Name=*Guid};
  return &early_hob.Report;
}
EFI_STATUS PianoEarlyHostCpu(UINT64 *El,UINT64 *Sctlr,UINT64 *Vbar,UINT64 *Daif,
  UINT64 *Counter,UINT64 *Frequency,UINT64 *SpSel){
  *El=4;*Sctlr=0;*Vbar=0x2000;*Daif=0x3c0;*Counter=cold_counter++;
  *Frequency=19200000;*SpSel=1;return EFI_SUCCESS;
}
UINTN EFIAPI PianoSecRead32(UINT64 Address,UINT32 *Value,PIANO_SEC_READ_STATE *State){
  ++cold_loads;State->Address=Address;State->OldVbar=0x2000;State->OldDaif=0x3c0;
  if(Address==PIANO_SMEM_BASE+0x3000){++payload_pass;
    if(cold_case==3&&payload_pass==2)smem[0x3040]^=1;}
  if(Address==PIANO_SMEM_BASE+0x8000){++descriptor_pass;
    if(cold_case==5&&descriptor_pass==2)smem[0x8018]^=1;}
  BOOLEAN Fault=(cold_case==2&&payload_pass==2&&Address==PIANO_SMEM_BASE+0x3004)||
    (cold_case==4&&descriptor_pass==2&&Address==PIANO_SMEM_BASE+0x8014)||
    (cold_case==11&&Address==PIANO_SMEM_BASE+0x3004)||
    (cold_case==12&&Address==PIANO_SMEM_COOKIE_LOW)||
    (cold_case==13&&descriptor_pass==2&&Address==PIANO_SMEM_BASE+0x801c);
  if(Fault){State->Faulted=1;State->Elr=0x1100;State->Esr=0x96000010;
    State->Far=Address;State->Spsr=0x3c5;State->Resume=0x1104;return 1;}
  if(cold_case==38&&Address==PIANO_SMEM_BASE+0x3004)return 2;
  if(Address==PIANO_SMEM_COOKIE_LOW){++cookie_pass;
    *Value=cold_case==6?0x90000000:0x81d08000;
    if(cold_case==10&&cookie_pass==2)*Value+=4;
  }else if(Address==PIANO_SMEM_COOKIE_HIGH)*Value=0;
  else{assert(Address>=PIANO_SMEM_BASE&&Address-PIANO_SMEM_BASE<=PIANO_SMEM_BYTES-4);
    memcpy(Value,smem+Address-PIANO_SMEM_BASE,4);}
  return 0;
}
VOID PianoEarlyHostFatal(VOID){abort();}
static VOID p16(UINT8 *P,UINT16 Value){P[0]=(UINT8)Value;P[1]=(UINT8)(Value>>8);}
static VOID UpdateDescriptorCrc(PIANO_EARLY_MEMORY_REPORT *R){
  R->RawDescriptorCrc32=PianoEarlyMemoryBytesCrc32(R->RawDescriptor,R->RawDescriptorBytes);
  R->Descriptor.Crc32=R->RawDescriptorCrc32;memset(R->Descriptor.Prefix,0,sizeof(R->Descriptor.Prefix));
  memcpy(R->Descriptor.Prefix,R->RawDescriptor,MIN(sizeof(R->Descriptor.Prefix),R->RawDescriptorBytes));
}
static VOID PrepareCold(VOID){
  scenario=1;early_case=1;Setup();p32(smem+0x3008,3);
  UINT8 *D=smem+0x8000;p32(D,0x49494953);p32(D+4,PIANO_SMEM_BYTES);
  p64(D+8,PIANO_SMEM_BASE);p16(D+16,512);p16(D+18,1);
  p16(D+20,0x4853);p16(D+22,12);p64(D+24,0x1122334455667788ULL);
  if(cold_case==1)p64(smem+0x3000+24+64,0); // Coherent RamRange failure.
  if(cold_case==7){p32(smem+0xd0+402*16+8,8192);p16(D+22,2028);}
  if(cold_case==8){p16(D+18,2);p16(D+32,0x4853);p16(D+34,4);}
  if(cold_case==9)p64(D+8,0x90000000); // Reported region is diagnostic only.
  memcpy(before,smem,sizeof(smem));hob_allocation_failed=cold_case==37;
}
static VOID Tamper(PIANO_EARLY_MEMORY_REPORT *R){
  if(cold_case==14)R->RawPayloadBytes=8193;
  if(cold_case==15)R->RawPayloadBytes-=4;
  if(cold_case==16)R->RawPayloadCrc32^=1;
  if(cold_case==17)R->RawPayload[0]^=1;
  if(cold_case==18)R->Smem.PayloadAddress=PIANO_SMEM_BASE+PIANO_SMEM_BYTES-4;
  if(cold_case==19)R->Smem.PayloadAddress=MAX_UINT64-3;
  if(cold_case==20)R->RawPayloadCoherent=2;
  if(cold_case==21)R->RawPayload[R->RawPayloadBytes]=1;
  if(cold_case==22)R->RawDescriptorBytes=2049;
  if(cold_case==23)R->RawDescriptorCrc32^=1;
  if(cold_case==24)R->Descriptor.Prefix[0]^=1;
  if(cold_case==25)R->Descriptor.Address+=4;
  if(cold_case==26){R->RawDescriptor[0]^=1;UpdateDescriptorCrc(R);}
  if(cold_case==27)R->RawReserved[0]=1;
  if(cold_case==28)R->Descriptor.Reserved=1;
  if(cold_case==29)R->Descriptor.RepeatedEqual=FALSE;
  if(cold_case==30)R->Descriptor.Status=EFI_WARN_UNKNOWN_GLYPH;
  if(cold_case==31)R->Descriptor.SnapshotBytes-=4;
  if(cold_case==32)early_hob.Header.Header.HobLength-=8;
  if(cold_case==33)R->Version=1;
  if(cold_case==34)R->HighDdrPublished=TRUE;
  if(cold_case==35)early_case=4;
  if(cold_case==36)early_copy_change=TRUE;
  if(cold_case==39)R->Smem.CookieStatus=EFI_DEVICE_ERROR;
  if(cold_case==40)R->RawDescriptor[R->RawDescriptorBytes]=1;
  if(cold_case==41)R->Descriptor.SmemBase^=4;
  if(cold_case==42){p16(R->RawDescriptor+22,32);UpdateDescriptorCrc(R);}
  if(cold_case==43){R->Descriptor.TlvCount=65;p16(R->RawDescriptor+18,65);UpdateDescriptorCrc(R);}
  if(cold_case==44)R->Descriptor.HostInfoBytes=8;
  if(cold_case==45){R->Status=EFI_COMPROMISED_DATA;R->Smem.Status=R->Status;R->Smem.Parsed=FALSE;}
  if(cold_case==46)R->Descriptor.RegionMatchesKnownWindow=FALSE;
  if(cold_case==47)R->RawPayloadCoherent=FALSE;
  if(cold_case==48)R->RawDescriptorCoherent=FALSE;
  if(cold_case==49)R->ColdStateVerified=FALSE;
  R->ReportCrc32=PianoEarlyMemoryReportCrc32(R);
}
static VOID CheckRaw(BOOLEAN Accepted){
  if(!Accepted){assert(!raw_payload_logged&&!raw_descriptor_logged);return;}
  assert(raw_payload_logged==frozen.RawPayloadBytes&&raw_descriptor_logged==frozen.RawDescriptorBytes);
  assert(!memcmp(raw_payload_log,frozen.RawPayload,frozen.RawPayloadBytes));
  assert(!memcmp(raw_descriptor_log,frozen.RawDescriptor,frozen.RawDescriptorBytes));
}
int main(int Argc,char **Argv){
  assert(Argc==2);cold_case=(UINT32)strtoul(Argv[1],NULL,10);assert(cold_case<50);PrepareCold();
  EFI_STATUS Observed=PianoEarlyMemoryObserveCold();CONST PIANO_EARLY_MEMORY_REPORT *R=PianoEarlyMemoryReport();
  assert(cold_loads==R->LoadCount&&R->ColdStateVerified&&R->Finished&&
    !R->MemoryOwnershipGranted&&!R->HighDdrPublished&&R->Version==2);
  if(cold_case==1)assert(Observed==EFI_COMPROMISED_DATA&&R->Smem.Reason==PianoSmemReasonRamRange&&
    !R->Smem.Parsed&&!R->Smem.BankCount&&!R->Smem.PreloadedCount&&R->RawPayloadCoherent&&R->RawDescriptorCoherent);
  if(cold_case==2||cold_case==3||cold_case==11||cold_case==38)assert(!R->RawPayloadCoherent&&!R->RawPayloadBytes&&!R->RawPayloadCrc32);
  if(cold_case==4||cold_case==5||cold_case==6||cold_case==10||cold_case==12||cold_case==13)
    assert(!R->RawDescriptorCoherent&&!R->RawDescriptorBytes&&!R->RawDescriptorCrc32);
  if(cold_case==7)assert(R->RawPayloadBytes==8192&&R->RawDescriptorBytes==2048);
  if(cold_case==8)assert(R->Descriptor.Status==EFI_COMPROMISED_DATA&&R->RawDescriptorCoherent);
  if(cold_case==9)assert(R->RawDescriptorCoherent&&!R->Descriptor.RegionMatchesKnownWindow);
  UINT32 ColdBefore=cold_loads;EFI_STATUS Published=PianoEarlyMemoryPublishHob();
  assert(Published==(cold_case==37?EFI_OUT_OF_RESOURCES:EFI_SUCCESS)&&cold_loads==ColdBefore);
  if(cold_case==37)early_case=0;
  else{assert(!memcmp(&early_hob.Report,R,sizeof(*R)));frozen=early_hob.Report;Tamper(&early_hob.Report);}
  assert(!calls&&!loads);assert(PianoProductObserveSmem()==EFI_NOT_READY&&!PianoProductSmemRetained());
  BOOLEAN Accepted=cold_case<14||cold_case==38;
  EFI_STATUS Expected=cold_case==37?EFI_NOT_FOUND:Accepted?EFI_SUCCESS:EFI_COMPROMISED_DATA;
  assert(PianoProductEarlySmemStatus()==Expected&&early_log_status==Expected);CheckRaw(Accepted);
  UINTN HobGets=early_gets;memset(&early_hob,0,sizeof(early_hob));memset(smem,0x5a,sizeof(smem));
  // Lost early log prefix and overwritten source/HOB cannot lose the already
  // validated DXE snapshot. Replay must reproduce every original byte.
  for(UINTN I=0;I<3;++I){
    memset(raw_payload_log,0,sizeof(raw_payload_log));memset(raw_descriptor_log,0,sizeof(raw_descriptor_log));
    raw_payload_logged=raw_descriptor_logged=0;Replay(EFI_SUCCESS);CheckRaw(Accepted);
    assert(early_gets==HobGets&&cold_loads==ColdBefore&&PianoProductEarlySmemStatus()==Expected);
  }
  printf("Actual SEC -> revision2 HOB -> DXE raw replay case%u passed; HOB=%lu bytes, no target rereads or DDR authority\n",
    cold_case,(unsigned long)sizeof(early_hob));return 0;
}
