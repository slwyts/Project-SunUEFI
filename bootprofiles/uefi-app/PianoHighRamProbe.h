// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only high-memory translation diagnostics; no mapping/allocation/DMA.
#ifndef PIANO_HIGH_RAM_PROBE_H
#define PIANO_HIGH_RAM_PROBE_H
#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Protocol/MemoryAttribute.h>

#ifndef PIANO_HIGH_RAM_PROBE_EXPERIMENT
#define PIANO_HIGH_RAM_PROBE_EXPERIMENT 0
#endif
#ifndef PIANO_HIGH_RAM_PATTERN_EXPERIMENT
#define PIANO_HIGH_RAM_PATTERN_EXPERIMENT 0
#endif
#if (PIANO_HIGH_RAM_PROBE_EXPERIMENT != 0 && PIANO_HIGH_RAM_PROBE_EXPERIMENT != 1) || \
    (PIANO_HIGH_RAM_PATTERN_EXPERIMENT != 0 && PIANO_HIGH_RAM_PATTERN_EXPERIMENT != 1)
#error High RAM experimental flags must be exactly 0 or 1
#endif

#define PIANO_HIGH_RAM_BASE 0xA00000000ULL
#define PIANO_HIGH_RAM_END  0xA40000000ULL
#define PIANO_HIGH_RAM_PAGE 4096U
#define PIANO_HIGH_RAM_MAX_ROWS 512U
#define PIANO_HIGH_RAM_TABLE_LOW 0xBD980000ULL // excludes known DT adspslpi overlap at heap start
#define PIANO_HIGH_RAM_TABLE_END 0xD8000000ULL

typedef enum {
  PianoHighDisabled=BIT0, PianoHighCpuUnknown=BIT1, PianoHighMapUnknown=BIT2,
  PianoHighGcdUnknown=BIT3, PianoHighTargetNotOccupied=BIT4,
  PianoHighAtFault=BIT5, PianoHighTableGuardUnknown=BIT6,
  PianoHighTableOwnerUnknown=BIT7, PianoHighTableReadFailed=BIT8,
  PianoHighInvalidPte=BIT9, PianoHighTranslationMismatch=BIT10,
  PianoHighAttrsUnknown=BIT11, PianoHighPhysicalRegimeUnknown=BIT12,
  PianoHighStateChanged=BIT13, PianoHighBudget=BIT14, PianoHighOwnershipUnknown=BIT15,
  PianoHighPatternDisabled=BIT16, PianoHighPatternRestoreFailed=BIT17,
  PianoHighCacheUnknown=BIT18, PianoHighWritePermissionUnknown=BIT19,
  PianoHighMemoryAttributeUnknown=BIT20
} PIANO_HIGH_RAM_REASON;

typedef struct {
  UINT64 CurrentEl; // raw CurrentEL encoding: EL1=4
  UINT64 Sctlr;
  UINT64 Tcr;
  UINT64 Ttbr0;
  UINT64 Mair;
} PIANO_HIGH_RAM_CPU;
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_STATE)(IN VOID *,OUT PIANO_HIGH_RAM_CPU *);
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_AT)(IN VOID *,IN UINT64 Va,OUT UINT64 *Par);
// ONLY a validated low table-word VA is handed to this protected backend.
// It must return a bounded status or use installed fault recovery; no naked
// high target pointer is handed to it. The helper never dereferences tables.
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_TABLE_READ)(IN VOID *,IN UINT64 Va,OUT UINT64 *Word);
typedef VOID (EFIAPI *PIANO_HIGH_RAM_LOG)(IN VOID *,IN CONST CHAR8 *Line);

typedef struct {
  UINT64 PteVa;
  UINT64 TablePar;
  UINT64 Value;
  UINT32 Level;
} PIANO_HIGH_RAM_PTE;
typedef struct {
  UINT64 Va;
  UINT64 End;
  UINT64 Par;
  UINT64 PaFromPar;
  EFI_STATUS AtStatus;
  EFI_STATUS EfiStatus;
  EFI_MEMORY_DESCRIPTOR Efi;
  EFI_STATUS GcdStatus;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Gcd;
  EFI_STATUS MemoryAttributeStatus;
  UINT64 MemoryAttributes; // optional verified protocol, sampled 4K only
  EFI_STATUS WalkStatus;
  UINT64 WalkPa;
  UINT64 Granule;
  UINT64 Leaf;
  UINT64 Hierarchical;
  UINT64 Reasons;
  UINT32 Depth;
  UINT8 AttrIndex;
  UINT8 MairByte;
  UINT8 Shareability;
  UINT8 Ap;
  BOOLEAN Af;
  BOOLEAN Pxn;
  BOOLEAN Uxn;
  BOOLEAN HierReadOnly;
  PIANO_HIGH_RAM_PTE Pte[4];
} PIANO_HIGH_RAM_ROW;

typedef struct {
  EFI_BOOT_SERVICES *Boot;
  EFI_DXE_SERVICES *Dxe;
  EFI_MEMORY_ATTRIBUTE_PROTOCOL *MemoryAttribute;
  BOOLEAN MemoryAttributeProviderVerified;
  // Preallocated scratch only. GetMemoryMap is attempted once; no allocate,
  // resize/retry loop, SetMemorySpaceAttributes, HOB or GCD mutation.
  VOID *MapBuffer;
  UINTN MapCapacity;
  PIANO_HIGH_RAM_STATE State;
  PIANO_HIGH_RAM_AT AtRead;
  PIANO_HIGH_RAM_AT AtWrite; // never used by the read-only probe
  PIANO_HIGH_RAM_TABLE_READ ReadTableWord;
  PIANO_HIGH_RAM_LOG Log;
  VOID *Context;
  UINT32 RowBudget; // 1..512, counts segments; exhausting it is partial/unknown
  BOOLEAN ExplicitEnable;
  BOOLEAN ClassicEl1Stage1PhysicalContractVerified;
  BOOLEAN LowTablePagesAndRecoveryVerified;
} PIANO_HIGH_RAM_ENV;
typedef struct {
  PIANO_HIGH_RAM_CPU Cpu;
  EFI_STATUS MapStatus;
  UINTN MapBytes;
  UINTN DescriptorBytes;
  UINT32 DescriptorVersion;
  UINT32 Rows;
  UINT64 CoveredEnd;
  UINT64 Reasons;
  BOOLEAN Complete;
  BOOLEAN TranslationMetadataConsistent;
  BOOLEAN OwnershipVerified; // always FALSE for this read-only helper
  BOOLEAN PatternPermitted;  // always FALSE; separate fresh contract is needed
  BOOLEAN TargetMemoryRead;
  BOOLEAN TargetMemoryWritten;
} PIANO_HIGH_RAM_RESULT;

EFI_STATUS PianoHighRamProbe(IN CONST PIANO_HIGH_RAM_ENV *,OUT PIANO_HIGH_RAM_RESULT *);
// Architecture wrappers are opt-in, EL1-only. PAR is saved/restored; AT is not
// a memory read/write or ownership proof. Other arches return UNSUPPORTED.
EFI_STATUS EFIAPI PianoHighRamArchitectureState(IN VOID *,OUT PIANO_HIGH_RAM_CPU *);
EFI_STATUS EFIAPI PianoHighRamArchitectureAtRead(IN VOID *,IN UINT64 Va,OUT UINT64 *Par);
EFI_STATUS EFIAPI PianoHighRamArchitectureAtWrite(IN VOID *,IN UINT64 Va,OUT UINT64 *Par);

// Future bounded 4K pattern transaction. No backend is supplied here; pattern
// code/default profile is OFF. Every contract below and fresh R/W translation,
// EFI Type2 and matching allocated GCD owner must pass before saving a page.
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_READ_BYTES)(IN VOID *,IN UINT64 Va,OUT UINT8 *,IN UINTN);
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_WRITE_BYTES)(IN VOID *,IN UINT64 Va,IN CONST UINT8 *,IN UINTN);
typedef EFI_STATUS (EFIAPI *PIANO_HIGH_RAM_CLEAN)(IN VOID *,IN UINT64 Va,IN UINTN);
typedef struct {
  BOOLEAN ExplicitEnable;
  BOOLEAN ExclusivePhaseOwnershipVerified;
  BOOLEAN DmaAndOtherCpuQuiescedVerified;
  BOOLEAN CacheAndAliasContractVerified;
  BOOLEAN FaultAndRestorationContractVerified;
  BOOLEAN SnapshotBufferLowOwnedVerified;
  EFI_HANDLE GcdOwner;
  UINT8 *SavedPage; // separate known-low, owned, nonaliasing 4096-byte scratch
  UINT8 *WorkPage;  // another known-low, owned, nonaliasing 4096-byte scratch
  UINTN BufferBytes;
  PIANO_HIGH_RAM_READ_BYTES Read;
  PIANO_HIGH_RAM_WRITE_BYTES Write;
  PIANO_HIGH_RAM_CLEAN CleanToPoc;
} PIANO_HIGH_RAM_PATTERN;
typedef struct {
  EFI_STATUS Status;
  EFI_STATUS RawStatus;
  EFI_STATUS RestoreStatus;
  EFI_STATUS RestoreRawStatus;
  UINT64 Reasons;
  BOOLEAN SaveCompleted;
  BOOLEAN WriteAttempted;
  BOOLEAN PatternCompared;
  BOOLEAN RestoreAttempted;
  BOOLEAN RestoreCompared;
} PIANO_HIGH_RAM_PATTERN_RESULT;
EFI_STATUS PianoHighRamPattern4K(IN CONST PIANO_HIGH_RAM_ENV *,IN CONST PIANO_HIGH_RAM_PATTERN *,
                                IN UINT64 Va,OUT PIANO_HIGH_RAM_PATTERN_RESULT *);
#endif
