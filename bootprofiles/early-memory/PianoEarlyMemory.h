// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoSmemRam.h"

#define PIANO_EARLY_MEMORY_VERSION 2U
#define PIANO_EARLY_SIII_RAW_MAX 2048U
#define PIANO_EARLY_MEMORY_HOB_GUID \
  {0x495ec035,0x44a5,0x4f17,{0x87,0x50,0x50,0x49,0x41,0x4e,0x4f,0x31}}

// The assembly layout is fixed and checked by compile-time assertions in C.
typedef struct {
  volatile UINT64 Armed,Faulted,Fatal;
  UINT64 Address,Elr,Esr,Far,Spsr,Resume,OldVbar,OldDaif;
} PIANO_SEC_READ_STATE;
// Serialized diagnostic metadata, independent of the descriptor collector's
// implementation header. Neither the embedded SIII base nor its cookie grants
// a new read window or any DDR authority.
typedef struct {
  EFI_STATUS Status;
  UINT64 Address,SmemBase;
  UINT32 SmemBytes;
  UINT16 ItemCount,TlvCount,HostInfoBytes,Reserved;
  UINT32 SnapshotBytes,Crc32;
  BOOLEAN RepeatedEqual,RegionMatchesKnownWindow;
  UINT8 ReservedFlags[6],Prefix[64];
} PIANO_EARLY_SIII_DIAGNOSTIC;
typedef struct {
  UINT32 Version,Bytes;
  UINT32 ReportCrc32,Reserved;
  EFI_STATUS Status,PublishStatus;
  UINT64 EntryEl,EntrySctlr,EntryVbar,EntryDaif,EntrySpSel;
  UINT32 LoadCount,RecoveredFaults;
  BOOLEAN Attempted,Finished,Published,ColdStateVerified;
  // Observation is NOT a full DDR owner ledger or publication authority.
  BOOLEAN MemoryOwnershipGranted,HighDdrPublished;
  PIANO_SEC_READ_STATE LastRead,LastFault;
  PIANO_SMEM_RAM_REPORT Smem;
  PIANO_EARLY_SIII_DIAGNOSTIC Descriptor;
  UINT32 RawPayloadBytes,RawPayloadCrc32,RawDescriptorBytes,RawDescriptorCrc32;
  BOOLEAN RawPayloadCoherent,RawDescriptorCoherent;
  UINT8 RawReserved[6];
  // Exact complete matching reads, including semantically rejected RAM402.
  // Unused bytes remain zero. These are evidence, never usable memory claims.
  UINT8 RawPayload[PIANO_SMEM_PAYLOAD_MAX];
  UINT8 RawDescriptor[PIANO_EARLY_SIII_RAW_MAX];
} PIANO_EARLY_MEMORY_REPORT;

STATIC inline UINT32 PianoEarlyMemoryBytesCrc32(CONST VOID *Bytes,UINTN Length){
  CONST UINT8 *P=Bytes;UINT32 C=0xffffffffU;
  for(UINTN I=0;I<Length;++I){C^=P[I];for(UINT32 J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);}
  return ~C;
}

// Integrity of the exact serialized diagnostic record, with its CRC field
// treated as zero. This is not authentication or hardware ownership proof.
STATIC inline UINT32 PianoEarlyMemoryReportCrc32(CONST PIANO_EARLY_MEMORY_REPORT *Report){
  CONST UINT8 *P=(CONST UINT8 *)Report;UINT32 C=0xffffffffU;
  for(UINTN I=0;I<sizeof(*Report);++I){
    UINT8 V=(I>=OFFSET_OF(PIANO_EARLY_MEMORY_REPORT,ReportCrc32)&&
      I<OFFSET_OF(PIANO_EARLY_MEMORY_REPORT,ReportCrc32)+sizeof(Report->ReportCrc32))?0:P[I];
    C^=V;for(UINT32 J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);
  }
  return ~C;
}

// Product SEC only: invoke at the start of InitializeMemory, before creating
// the first PHIT or calling MemoryPeim. No caller supplied address permission.
EFI_STATUS PianoEarlyMemoryObserveCold(VOID);
// Invoke after the one HobConstructor / PrePeiSetHobList, before MemoryPeim.
// Copies frozen data into a GUID HOB; never reads SMEM or calls MemoryPeim.
EFI_STATUS PianoEarlyMemoryPublishHob(VOID);
CONST PIANO_EARLY_MEMORY_REPORT *PianoEarlyMemoryReport(VOID);

// Exact single LDR + EL1 exception fixup. EL1, MMU and D-cache off required.
// Returns 0 complete, 1 recovered precise read abort, 2 unsupported CPU state.
UINTN EFIAPI PianoSecRead32(UINT64 Address,UINT32 *Value,
  PIANO_SEC_READ_STATE *State);
// Unknown exception / SError never resumes. Log and attempt PSCI cold reset;
// if firmware returns, retain the guard vector and stop permanently.
VOID EFIAPI PianoSecReadFatal(PIANO_SEC_READ_STATE *State);
