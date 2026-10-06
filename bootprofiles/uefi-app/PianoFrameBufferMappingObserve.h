// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <PiDxe.h>
#define PIANO_FB_MAPPING_PHASES 32U
#define PIANO_FB_MAPPING_BASE 0xfc800000ULL
#define PIANO_FB_MAPPING_BYTES 0x1a13000ULL
typedef BOOLEAN (EFIAPI *PIANO_FB_MAPPING_ALIVE)(VOID);
typedef struct {UINT64 El,Sctlr,Tcr,Ttbr0,Ttbr1,Mair,Daif;} PIANO_FB_CPU_STATE;
typedef struct {
  UINT64 Address,GcdBase,GcdLength,GcdCapabilities,GcdAttributes,Par,PhysicalPage;
  UINT32 GcdType;UINT8 Attribute;
  EFI_STATUS GcdStatus,AtStatus;
  BOOLEAN ReadProtected,ParFault,Identity;
} PIANO_FB_MAPPING_SAMPLE;
typedef struct {
  PIANO_FB_CPU_STATE Before,After;
  PIANO_FB_MAPPING_SAMPLE Sample[3];
  UINTN Gop,BltPc,ModePointer,InfoPointer;
  UINT64 Base,Bytes;UINT32 Width,Height,Stride,Format;
  EFI_STATUS MetadataStatus,CpuStatus;
} PIANO_FB_MAPPING_ROUND;
typedef struct {
  CHAR8 Phase[32];EFI_STATUS Status;
  BOOLEAN Coherent,ServicesLost;
  UINT32 Rounds,SampleCount;
  PIANO_FB_MAPPING_ROUND Round[2];
} PIANO_FB_MAPPING_SNAPSHOT;
typedef struct {UINT32 Revision,Count;BOOLEAN ServicesLost;PIANO_FB_MAPPING_SNAPSHOT Snapshot[PIANO_FB_MAPPING_PHASES];} PIANO_FB_MAPPING_REPORT;
/* Fixed trusted GOP association and six no-target-load AT/GCD observations.
 * Coherent means these three samples agreed twice; never whole-range ready. */
EFI_STATUS PianoFrameBufferMappingObserve(CONST CHAR8 *Phase,PIANO_FB_MAPPING_ALIVE Alive);
EFI_STATUS PianoFrameBufferMappingReemit(PIANO_FB_MAPPING_ALIVE Alive);
BOOLEAN PianoFrameBufferMappingRetained(VOID);
CONST PIANO_FB_MAPPING_REPORT *PianoFrameBufferMappingGetReport(VOID);
