// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef NULL
#include "PianoHighRamProbe.h"

#define ROOT_TABLE 0xD7FFF000ULL
#define L1_TABLE 0xD7FFE000ULL
#define L2_TABLE 0xD7FFD000ULL
#define L3_TABLE 0xD7FFC000ULL
#define ADDR_MASK 0x0000FFFFFFFFF000ULL
#define LEAF_ATTR ((3ULL<<2)|(3ULL<<8)|BIT10|BIT53|BIT54)
typedef struct {
  PIANO_HIGH_RAM_CPU Cpu;
  UINT32 StateCalls,MapCalls,GcdCalls,AttrCalls,AtCalls,AtWriteCalls,TableCalls,LogCalls,ReadCalls,WriteCalls,CleanCalls;
  UINT32 LeafLevel,MapMode,AtMode,WarningSource;
  BOOLEAN ChangeState,CorruptTable,HierRo,TableRo,BadMair,FailTable,NoGcdOwner,BadGcdRange;
  BOOLEAN FailFirstWrite,FailRestoreWrite,CorruptPatternRead,FailClean;
  BOOLEAN AttrNoMapping,AttrReadProtected;
  BOOLEAN WideLeaf,WideNextTable;
  UINT8 Target[4096],Original[4096];
  union {UINT64 Align;UINT8 Bytes[16384];} Scratch;
  CHAR8 LogText[131072];UINTN LogLength;
} MOCK;
STATIC MOCK *Active;

STATIC EFI_STATUS EFIAPI State(VOID *Context,PIANO_HIGH_RAM_CPU *Cpu) {
  MOCK *M=Context;M->StateCalls++;*Cpu=M->Cpu;
  if(M->ChangeState && M->StateCalls>1)Cpu->Ttbr0-=4096;
  return M->WarningSource==1?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Map(UINTN *Bytes,EFI_MEMORY_DESCRIPTOR *Buffer,UINTN *Key,UINTN *Stride,UINT32 *Version) {
  MOCK *M=Active;M->MapCalls++;*Stride=48;*Version=1;*Key=77;
  if(M->MapMode==4) { *Bytes=32768;return EFI_BUFFER_TOO_SMALL; }
  assert(*Bytes>=3*48);memset(Buffer,0,3*48);
  EFI_MEMORY_DESCRIPTOR D={0};D.Type=EfiBootServicesData;D.PhysicalStart=PIANO_HIGH_RAM_TABLE_LOW;
  if(M->MapMode==6)D.Type=EfiConventionalMemory;
  D.NumberOfPages=(PIANO_HIGH_RAM_TABLE_END-PIANO_HIGH_RAM_TABLE_LOW)/4096;D.Attribute=EFI_MEMORY_WB;
  memcpy(Buffer,&D,sizeof(D));*Bytes=48;
  if(M->MapMode!=1) {
    D.Type=M->MapMode==7?EfiConventionalMemory:EfiLoaderData;D.PhysicalStart=PIANO_HIGH_RAM_BASE;
    D.NumberOfPages=(PIANO_HIGH_RAM_END-PIANO_HIGH_RAM_BASE)/4096;
    if(M->MapMode==2) { D.PhysicalStart=ROOT_TABLE;D.NumberOfPages=1; }
    if(M->MapMode==5) { D.PhysicalStart+=0x20000000;D.NumberOfPages=0x20000000/4096; }
    memcpy((UINT8 *)Buffer+48,&D,sizeof(D));*Bytes=96;
  }
  if(M->MapMode==3)*Stride=39;
  return M->WarningSource==2?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Gcd(UINT64 Pa,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  MOCK *M=Active;M->GcdCalls++;assert(Pa>=PIANO_HIGH_RAM_BASE && Pa<PIANO_HIGH_RAM_END);
  memset(D,0,sizeof(*D));D->BaseAddress=PIANO_HIGH_RAM_BASE;D->Length=PIANO_HIGH_RAM_END-PIANO_HIGH_RAM_BASE;
  D->Capabilities=EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB;
  D->Attributes=EFI_MEMORY_WB|EFI_MEMORY_XP;D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;
  D->ImageHandle=M->NoGcdOwner?NULL:(VOID *)0xbeef;
  if(M->BadGcdRange)D->Length=2048;
  return M->WarningSource==5?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI At(VOID *Context,UINT64 Va,UINT64 *Par) {
  MOCK *M=Context;M->AtCalls++;*Par=(0xffULL<<56)|(3ULL<<7)|(Va&ADDR_MASK);
  BOOLEAN High=Va>=PIANO_HIGH_RAM_BASE && Va<PIANO_HIGH_RAM_END;
  if(M->AtMode==1 && High)*Par=1|(5<<1); // translation fault level1
  if(M->AtMode==2 && High)*Par^=4096;
  if(M->AtMode==3 && !High)*Par=1;
  if(M->AtMode==4 && High)*Par|=BIT44;
  return M->WarningSource==3?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Attributes(EFI_MEMORY_ATTRIBUTE_PROTOCOL *This,UINT64 Address,UINT64 Bytes,UINT64 *Attrs) {
  MOCK *M=Active;(void)This;M->AttrCalls++;assert(Address>=PIANO_HIGH_RAM_BASE && Address<PIANO_HIGH_RAM_END && Bytes==4096);
  *Attrs=EFI_MEMORY_XP|(M->AttrReadProtected?EFI_MEMORY_RP:0);
  return M->AttrNoMapping?EFI_NO_MAPPING:M->WarningSource==6?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ForbiddenAttributeWrite(EFI_MEMORY_ATTRIBUTE_PROTOCOL *This,UINT64 Address,UINT64 Bytes,UINT64 Attrs) {
  (void)This;(void)Address;(void)Bytes;(void)Attrs;assert(!"No EFI attribute mutation allowed");return EFI_ABORTED;
}
STATIC EFI_STATUS EFIAPI AtWrite(VOID *Context,UINT64 Va,UINT64 *Par) {
  MOCK *M=Context;M->AtWriteCalls++;
  return At(Context,Va,Par);
}
STATIC EFI_STATUS EFIAPI Table(VOID *Context,UINT64 Va,UINT64 *Word) {
  MOCK *M=Context;M->TableCalls++;
  assert(Va>=PIANO_HIGH_RAM_TABLE_LOW && Va<PIANO_HIGH_RAM_TABLE_END && Va<PIANO_HIGH_RAM_BASE);
  if(M->FailTable)return EFI_DEVICE_ERROR;
  EFI_STATUS Status=M->WarningSource==4?EFI_WARN_STALE_DATA:EFI_SUCCESS;
  UINT64 Attr=LEAF_ATTR|(M->TableRo?(2ULL<<6):0);
  if(M->BadMair)Attr &= ~(7ULL<<2);
  if(Va==ROOT_TABLE) { *Word=L1_TABLE|3|(M->HierRo?BIT62:0)|(M->WideNextTable?BIT44:0);return Status; }
  if(Va>=L1_TABLE && Va<L1_TABLE+4096) {
    if(M->CorruptTable) { *Word=0;return Status; }
    *Word=M->LeafLevel==1?(PIANO_HIGH_RAM_BASE|Attr|1|(M->WideLeaf?BIT44:0)):(L2_TABLE|3);return Status;
  }
  if(Va>=L2_TABLE && Va<L2_TABLE+4096) {
    UINT64 Index=(Va-L2_TABLE)/8;
    *Word=M->LeafLevel==2?((PIANO_HIGH_RAM_BASE+Index*0x200000)|Attr|1):(L3_TABLE|3);return Status;
  }
  if(Va>=L3_TABLE && Va<L3_TABLE+4096) {
    *Word=(PIANO_HIGH_RAM_BASE+(Va-L3_TABLE)/8*4096)|Attr|3;return Status;
  }
  assert(!"Unexpected table page");return EFI_DEVICE_ERROR;
}
STATIC VOID EFIAPI Log(VOID *Context,CONST CHAR8 *Line) {
  MOCK *M=Context;UINTN N=strlen(Line);M->LogCalls++;assert(N<256 && N && Line[N-1]=='\n');
  if(M->LogLength+N+1<sizeof(M->LogText)) { memcpy(M->LogText+M->LogLength,Line,N+1);M->LogLength+=N; }
}
STATIC EFI_STATUS EFIAPI Read(VOID *Context,UINT64 Va,UINT8 *Buffer,UINTN Bytes) {
  MOCK *M=Context;M->ReadCalls++;assert(Va==PIANO_HIGH_RAM_BASE && Bytes==4096);
  memcpy(Buffer,M->Target,Bytes);if(M->CorruptPatternRead && M->ReadCalls==2)Buffer[0]^=1;
  return M->WarningSource==7 && M->ReadCalls==1?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write(VOID *Context,UINT64 Va,CONST UINT8 *Buffer,UINTN Bytes) {
  MOCK *M=Context;M->WriteCalls++;assert(Va==PIANO_HIGH_RAM_BASE && Bytes==4096);
  if((M->FailFirstWrite && M->WriteCalls==1)||(M->FailRestoreWrite && M->WriteCalls==2)) {
    memcpy(M->Target,Buffer,512);return EFI_DEVICE_ERROR;
  }
  memcpy(M->Target,Buffer,Bytes);
  return (M->WarningSource==8 && M->WriteCalls==1)||(M->WarningSource==10 && M->WriteCalls==2)?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Clean(VOID *Context,UINT64 Va,UINTN Bytes) {
  MOCK *M=Context;M->CleanCalls++;assert(Va==PIANO_HIGH_RAM_BASE && Bytes==4096);
  return M->FailClean && M->CleanCalls==1?EFI_DEVICE_ERROR:M->WarningSource==9 && M->CleanCalls==1?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC PIANO_HIGH_RAM_ENV Setup(MOCK *M) {
  STATIC EFI_BOOT_SERVICES Boot;STATIC EFI_DXE_SERVICES Dxe;STATIC EFI_MEMORY_ATTRIBUTE_PROTOCOL Attr;
  memset(M,0,sizeof(*M));memset(&Boot,0,sizeof(Boot));memset(&Dxe,0,sizeof(Dxe));Active=M;
  Boot.GetMemoryMap=Map;Dxe.GetMemorySpaceDescriptor=Gcd;M->LeafLevel=1;
  Attr.GetMemoryAttributes=Attributes;Attr.SetMemoryAttributes=ForbiddenAttributeWrite;Attr.ClearMemoryAttributes=ForbiddenAttributeWrite;
  M->Cpu.CurrentEl=4;M->Cpu.Sctlr=1;M->Cpu.Tcr=0x480803514ULL;
  M->Cpu.Ttbr0=ROOT_TABLE;M->Cpu.Mair=0xffbb4400;
  for(UINTN I=0;I<4096;I++)M->Original[I]=M->Target[I]=(UINT8)(I*7+3);
  PIANO_HIGH_RAM_ENV E={0};E.Boot=&Boot;E.Dxe=&Dxe;E.MapBuffer=M->Scratch.Bytes;E.MapCapacity=sizeof(M->Scratch.Bytes);
  E.MemoryAttribute=&Attr;E.MemoryAttributeProviderVerified=TRUE;
  E.State=State;E.AtRead=At;E.AtWrite=AtWrite;E.ReadTableWord=Table;E.Log=Log;E.Context=M;E.RowBudget=4;
  E.ExplicitEnable=TRUE;E.ClassicEl1Stage1PhysicalContractVerified=TRUE;E.LowTablePagesAndRecoveryVerified=TRUE;
  return E;
}
STATIC VOID DisabledAndMetadata(VOID) {
  MOCK M;PIANO_HIGH_RAM_ENV E=Setup(&M);PIANO_HIGH_RAM_RESULT R;
  E.ExplicitEnable=FALSE;assert(PianoHighRamProbe(&E,&R)==EFI_UNSUPPORTED);
  assert(R.Reasons&PianoHighDisabled);assert(!M.StateCalls&&!M.MapCalls&&!M.AtCalls&&!M.GcdCalls&&!M.TableCalls);
  E.ExplicitEnable=TRUE;
  if(!PIANO_HIGH_RAM_PROBE_EXPERIMENT) {
    assert(PianoHighRamProbe(&E,&R)==EFI_UNSUPPORTED && !M.StateCalls);
    PIANO_HIGH_RAM_PATTERN_RESULT P;
    assert(PianoHighRamPattern4K(&E,(VOID *)1,PIANO_HIGH_RAM_BASE,&P)==EFI_UNSUPPORTED);
    return;
  }
  assert(PianoHighRamProbe(&E,&R)==EFI_SUCCESS && R.Complete && R.Rows==1 && R.CoveredEnd==PIANO_HIGH_RAM_END);
  assert(R.TranslationMetadataConsistent && !R.OwnershipVerified && !R.PatternPermitted);
  assert(R.Reasons==PianoHighOwnershipUnknown && !R.TargetMemoryRead && !R.TargetMemoryWritten);
  assert(M.AtCalls==3 && M.TableCalls==2 && M.MapCalls==1 && M.GcdCalls==1);
  assert(!M.AtWriteCalls&&!M.ReadCalls&&!M.WriteCalls&&!M.CleanCalls);
  assert(strstr(M.LogText,"PIANO_HIGH_RAM_AT") && strstr(M.LogText,"block_bytes=0x0000000040000000"));
  assert(strstr(M.LogText,"PIANO_HIGH_RAM_EFI") && strstr(M.LogText,"PIANO_HIGH_RAM_GCD") && strstr(M.LogText,"physical_phase_ownership_not_proven"));
  E=Setup(&M);M.LeafLevel=2;E.RowBudget=512;
  assert(PianoHighRamProbe(&E,&R)==EFI_SUCCESS && R.Rows==512 && R.Complete && !M.ReadCalls&&!M.WriteCalls);
  E=Setup(&M);M.LeafLevel=3;E.RowBudget=1;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !R.Complete && R.Rows==1 && R.CoveredEnd==PIANO_HIGH_RAM_BASE+4096);
  assert(strstr(M.LogText,"block_bytes=0x0000000000001000") && (R.Reasons&PianoHighBudget));
}
STATIC VOID Refusals(VOID) {
  MOCK M;PIANO_HIGH_RAM_ENV E;PIANO_HIGH_RAM_RESULT R;
  if(!PIANO_HIGH_RAM_PROBE_EXPERIMENT)return;
  for(UINT32 Mode=1;Mode<=7;Mode++) {
    E=Setup(&M);M.MapMode=Mode;E.RowBudget=2;
    assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !M.ReadCalls&&!M.WriteCalls);
    if(Mode==2 || Mode==3 || Mode==4)assert(!M.TableCalls && (R.Reasons&PianoHighMapUnknown));
    if(Mode==5)assert(R.Complete && R.Rows==2); // EFI hole split at later start
    if(Mode==7)assert(R.Reasons&PianoHighTargetNotOccupied);
  }
  E=Setup(&M);E.ClassicEl1Stage1PhysicalContractVerified=FALSE;E.RowBudget=1;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !M.TableCalls && (R.Reasons&PianoHighPhysicalRegimeUnknown));
  E=Setup(&M);E.LowTablePagesAndRecoveryVerified=FALSE;E.RowBudget=1;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !M.TableCalls && (R.Reasons&PianoHighTableGuardUnknown));
  for(UINT32 Mode=1;Mode<=3;Mode++) {
    E=Setup(&M);M.AtMode=Mode;
    assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY);
    assert(R.Reasons&(Mode==1?PianoHighAtFault:Mode==2?PianoHighTranslationMismatch:PianoHighTableOwnerUnknown));
    if(Mode==3)assert(!M.TableCalls);
  }
  E=Setup(&M);M.CorruptTable=TRUE;assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighInvalidPte));
  E=Setup(&M);M.HierRo=TRUE;assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighWritePermissionUnknown));
  E=Setup(&M);M.BadMair=TRUE;assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighAttrsUnknown));
  E=Setup(&M);M.FailTable=TRUE;E.RowBudget=1;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && M.TableCalls==1 && (R.Reasons&PianoHighTableReadFailed));
  E=Setup(&M);M.NoGcdOwner=TRUE;assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighTargetNotOccupied));
  E=Setup(&M);E.Dxe=NULL;assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighGcdUnknown));
  E=Setup(&M);E.MemoryAttributeProviderVerified=FALSE;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !M.AttrCalls && (R.Reasons&PianoHighMemoryAttributeUnknown));
  E=Setup(&M);M.AttrNoMapping=TRUE;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighMemoryAttributeUnknown));
  E=Setup(&M);M.AttrReadProtected=TRUE;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighAttrsUnknown));
  E=Setup(&M);M.Cpu.Ttbr0=0xD4E23000;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && !M.TableCalls && (R.Reasons&PianoHighTableOwnerUnknown));
  E=Setup(&M);M.ChangeState=TRUE;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY && (R.Reasons&PianoHighStateChanged));
  for(UINT32 Source=1;Source<=6;Source++) {
    E=Setup(&M);M.WarningSource=Source;E.RowBudget=1;
    EFI_STATUS S=PianoHighRamProbe(&E,&R);
    assert(S==(Source==1?EFI_UNSUPPORTED:EFI_NOT_READY) && !R.TranslationMetadataConsistent);
    assert(!M.ReadCalls&&!M.WriteCalls&&!M.CleanCalls&&!R.OwnershipVerified&&!R.PatternPermitted);
    if(Source==1)assert(!M.MapCalls&&!M.AtCalls);
    if(Source==2 || Source==3)assert(!M.TableCalls);
    if(Source==4)assert(M.TableCalls==1 && (R.Reasons&PianoHighTableReadFailed));
    if(Source==5)assert(R.Reasons&PianoHighGcdUnknown);
    if(Source==6)assert(R.Reasons&PianoHighMemoryAttributeUnknown);
  }
  E=Setup(&M);M.Cpu.Ttbr0|=BIT44;
  assert(PianoHighRamProbe(&E,&R)==EFI_UNSUPPORTED&&!M.AtCalls&&!M.TableCalls);
  E=Setup(&M);M.WideNextTable=TRUE;E.RowBudget=1;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY&&M.TableCalls==1&&(R.Reasons&PianoHighInvalidPte));
  E=Setup(&M);M.WideLeaf=TRUE;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY&&(R.Reasons&PianoHighInvalidPte));
  E=Setup(&M);M.AtMode=4;
  assert(PianoHighRamProbe(&E,&R)==EFI_NOT_READY&&(R.Reasons&PianoHighAtFault));
  for(UINTN I=0;I<4;I++) {
    E=Setup(&M);
    if(I==0)M.Cpu.CurrentEl=8;
    if(I==1)M.Cpu.Tcr|=BIT39;
    if(I==2)M.Cpu.Tcr|=BIT59;
    if(I==3)M.Cpu.Tcr|=BIT14;
    assert(PianoHighRamProbe(&E,&R)==EFI_UNSUPPORTED && !M.MapCalls&&!M.AtCalls&&!M.TableCalls);
  }
}
STATIC VOID Pattern(VOID) {
  MOCK M;PIANO_HIGH_RAM_ENV E=Setup(&M);PIANO_HIGH_RAM_PATTERN P={0};PIANO_HIGH_RAM_PATTERN_RESULT R;
  if(!PIANO_HIGH_RAM_PROBE_EXPERIMENT || !PIANO_HIGH_RAM_PATTERN_EXPERIMENT) {
    assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_UNSUPPORTED && !M.StateCalls&&!M.ReadCalls&&!M.WriteCalls);
    assert(R.Reasons&PianoHighPatternDisabled);return;
  }
  UINT8 *Buffers=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(Buffers!=MAP_FAILED && (UINTN)Buffers+8192<PIANO_HIGH_RAM_BASE);
  P.SavedPage=Buffers;P.WorkPage=Buffers+4096;P.BufferBytes=4096;P.GcdOwner=(VOID *)0xbeef;
  P.Read=Read;P.Write=Write;P.CleanToPoc=Clean;P.ExplicitEnable=TRUE;
  P.ExclusivePhaseOwnershipVerified=TRUE;P.DmaAndOtherCpuQuiescedVerified=TRUE;
  P.CacheAndAliasContractVerified=TRUE;P.FaultAndRestorationContractVerified=TRUE;P.SnapshotBufferLowOwnedVerified=TRUE;
  BOOLEAN *Gates[]={&P.ExclusivePhaseOwnershipVerified,&P.DmaAndOtherCpuQuiescedVerified,
    &P.CacheAndAliasContractVerified,&P.FaultAndRestorationContractVerified,&P.SnapshotBufferLowOwnedVerified};
  for(UINTN I=0;I<sizeof(Gates)/sizeof(Gates[0]);I++) {
    *Gates[I]=FALSE;
    assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_NOT_READY && !M.ReadCalls&&!M.WriteCalls);
    *Gates[I]=2;
    assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_NOT_READY && !M.ReadCalls&&!M.WriteCalls);
    *Gates[I]=TRUE;
  }
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_SUCCESS);
  assert(R.SaveCompleted&&R.WriteAttempted&&R.PatternCompared&&R.RestoreAttempted&&R.RestoreCompared);
  assert(M.ReadCalls==3&&M.WriteCalls==2&&M.CleanCalls==2&&M.AtWriteCalls==1);
  assert(!memcmp(M.Target,M.Original,4096));
  for(UINTN I=0;I<8192;I++)assert(Buffers[I]==0);
  for(UINT32 Mode=0;Mode<4;Mode++) {
    E=Setup(&M);
    if(Mode==0)M.FailFirstWrite=TRUE;
    if(Mode==1)M.CorruptPatternRead=TRUE;
    if(Mode==2)M.FailClean=TRUE;
    if(Mode==3)M.FailRestoreWrite=TRUE;
    EFI_STATUS S=PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R);
    assert(EFI_ERROR(S) && R.SaveCompleted && R.RestoreAttempted && M.WriteCalls==2);
    if(Mode!=3) {assert(R.RestoreCompared&&!memcmp(M.Target,M.Original,4096));}
    else {assert(S==EFI_ABORTED&&!R.RestoreCompared&&(R.Reasons&PianoHighPatternRestoreFailed));assert(!memcmp(P.SavedPage,M.Original,4096));}
  }
  for(UINT32 Warning=7;Warning<=10;Warning++) {
    E=Setup(&M);M.WarningSource=Warning;
    EFI_STATUS S=PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R);
    assert(EFI_ERROR(S) && EFI_ERROR(R.Status) && !memcmp(M.Target,M.Original,4096));
    if(Warning==7) {assert(!R.SaveCompleted&&!M.WriteCalls&&R.RawStatus==EFI_WARN_STALE_DATA);}
    else if(Warning==10) {
      assert(S==EFI_ABORTED && !R.RestoreCompared && EFI_ERROR(R.RestoreStatus));
      assert(R.RestoreRawStatus==EFI_WARN_STALE_DATA && !memcmp(P.SavedPage,M.Original,4096));
    } else {assert(R.RestoreCompared && R.RawStatus==EFI_WARN_STALE_DATA && R.Status==EFI_DEVICE_ERROR);}
  }
  E=Setup(&M);M.WarningSource=1;
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_DEVICE_ERROR && R.RawStatus==EFI_WARN_STALE_DATA && !M.ReadCalls&&!M.WriteCalls);
  E=Setup(&M);P.GcdOwner=(VOID *)1;
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_NOT_READY&&!M.ReadCalls&&!M.WriteCalls);P.GcdOwner=(VOID *)0xbeef;
  E=Setup(&M);M.BadGcdRange=TRUE;
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_NOT_READY&&!M.ReadCalls&&!M.WriteCalls);
  E=Setup(&M);P.WorkPage=P.SavedPage;
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_BASE,&R)==EFI_INVALID_PARAMETER&&!M.ReadCalls&&!M.WriteCalls);
  P.WorkPage=Buffers+4096;
  assert(PianoHighRamPattern4K(&E,&P,PIANO_HIGH_RAM_END,&R)==EFI_INVALID_PARAMETER);
  assert(munmap(Buffers,8192)==0);
}
int main(void) {
  DisabledAndMetadata();Refusals();Pattern();
  puts("PianoHighRamProbe actual C PASS: default-off/AT-only-target/EFI+GCD/4K-L1-L2-L3 walk/identity+attrs+owner refusal/partial bounds; gated 4K synthetic save-pattern-clean-compare-restore/error paths. No hardware validation.");
  return 0;
}
