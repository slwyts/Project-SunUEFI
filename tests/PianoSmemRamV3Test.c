// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual v3 pure observation parser. All ranges are CPU data, never permission.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/early-memory/PianoSmemRam.h"
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static UINT8 Raw[PIANO_SMEM_PAYLOAD_MAX];static PIANO_SMEM_RAM_REPORT Report;static UINT32 Cases;
static VOID P32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static VOID P64(UINT8 *P,UINT64 V){P32(P,(UINT32)V);P32(P+4,(UINT32)(V>>32));}
static UINT64 U64(CONST UINT8 *P){UINT64 V=0;for(UINTN I=0;I<8;++I)V|=(UINT64)P[I]<<(8*I);return V;}
static UINT8 *Entry(UINT32 I){return Raw+24+72*I;}
static VOID Set(UINT32 I,UINT64 Base,UINT64 Declared,UINT64 Current,UINT32 Category,UINT32 Type){
  UINT8 *E=Entry(I);P64(E+16,Base);P64(E+24,Declared);P64(E+64,Current);P32(E+36,Category);P32(E+44,Type);
}
static VOID Setup(VOID){
  memset(Raw,0,sizeof(Raw));P32(Raw,0x9da5e0a8);P32(Raw+4,0xaf9ec4e2);P32(Raw+8,3);P32(Raw+16,6);
  Set(0,0x1000,0x1000,0x200,14,1);Set(1,0x1600,0,0x100,14,1);
  Set(2,0x1100,0x1000,0,14,1);Set(3,0x1900,0x100,0x100,14,2);
  Set(4,0x1000,0x1000,0x1000,4,1);Set(5,0x1100,0x100,0x9999,14,5);
}
static VOID Result(EFI_STATUS Status,PIANO_SMEM_REASON Reason){
  assert(PianoSmemRamParse(Raw,24+72*6,&Report)==Status&&Report.Reason==Reason);
  if(Status!=EFI_SUCCESS)assert(!Report.Parsed&&!Report.BankCount&&!Report.PreloadedCount&&!Report.OtherCategoryCount);
  ++Cases;
}
static UINT32 Crc(CONST UINT8 *P,UINTN N){UINT32 C=MAX_UINT32;for(UINTN I=0;I<N;++I){C^=P[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);}return ~C;}
int main(int Argc,char **Argv){
  assert(Argc==1||Argc==2);Setup();Result(EFI_SUCCESS,PianoSmemReasonNone);
  assert(Report.BankCount==3&&Report.PreloadedCount==1&&Report.OtherCategoryCount==2&&Report.Parsed);
  assert(Report.Banks[1].SourceIndex==1&&!Report.Banks[1].RawSize&&Report.Banks[1].AvailableLength==0x100);
  assert(Report.Banks[2].SourceIndex==2&&!Report.Banks[2].AvailableLength&&Report.Banks[2].RawSize==0x1000);
  assert(Report.Preloaded[0].SourceIndex==5&&Report.Preloaded[0].RawSize==0x100&&!Report.Preloaded[0].AvailableLength);
  Setup();P64(Entry(0)+64,0);P64(Entry(1)+64,0);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.BankCount==3);
  Setup();P64(Entry(0)+24,0);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(!Report.Banks[0].RawSize);
  Setup();P64(Entry(0)+24,1);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.Banks[0].AvailableLength>Report.Banks[0].RawSize);
  Setup();P64(Entry(0)+24,MAX_UINT64);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.Banks[0].RawSize==MAX_UINT64);
  Setup();P64(Entry(0)+64,MAX_UINT64);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
  Setup();P64(Entry(1)+16,0x1100);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamOverlap);
  Setup();P64(Entry(1)+16,0x1200);Result(EFI_SUCCESS,PianoSmemReasonNone);
  Setup();P64(Entry(1)+16,0x1000);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamOverlap);
  Setup();P64(Entry(2)+16,MAX_UINT64);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(!Report.Banks[2].AvailableLength);
  Setup();P64(Entry(0)+16,MAX_UINT64-0x100);P64(Entry(0)+64,0x100);Result(EFI_SUCCESS,PianoSmemReasonNone);
  Setup();P64(Entry(0)+16,MAX_UINT64-0xff);P64(Entry(0)+64,0x100);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
  Setup();Set(3,MAX_UINT64,MAX_UINT64,MAX_UINT64,14,2);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.OtherCategoryCount==2);
  Setup();P32(Entry(3)+44,99);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.OtherCategoryCount==2);
  Setup();P32(Entry(0)+44,2);P32(Entry(1)+44,2);P32(Entry(2)+44,2);Result(EFI_SUCCESS,PianoSmemReasonNone);
  assert(!Report.BankCount&&Report.PreloadedCount==1&&Report.OtherCategoryCount==5&&Report.Parsed);
  Setup();P64(Entry(5)+24,0);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
  Setup();P64(Entry(5)+16,MAX_UINT64-3);P64(Entry(5)+24,4);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
  Setup();P32(Entry(5)+44,9);Result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.Preloaded[0].RawType==9);
  Setup();P32(Raw+8,2);Result(EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
  Setup();P32(Raw+8,4);Result(EFI_UNSUPPORTED,PianoSmemReasonRamVersion);
  if(Argc==2){
    FILE *F=fopen(Argv[1],"rb");assert(F);UINTN Bytes=fread(Raw,1,sizeof(Raw),F);assert(!ferror(F)&&feof(F)&&!fclose(F));
    assert(Bytes==2328&&Crc(Raw,Bytes)==0x7c271814);
    assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS&&Report.RamVersion==3&&Report.RawEntryCount==15);
    assert(Report.BankCount==12&&!Report.PreloadedCount&&Report.OtherCategoryCount==3);
    CONST UINT32 Indices[]={0,1,2,3,4,5,6,7,11,12,13,14};UINT32 Positive=0;
    UINT64 DeclaredSum=0,CurrentSum=0;
    for(UINT32 I=0;I<12;++I){PIANO_SMEM_RAM_ENTRY *R=&Report.Banks[I];assert(R->SourceIndex==Indices[I]&&R->RawType==1);
      assert(R->Base==U64(Entry(Indices[I])+16)&&R->RawSize==U64(Entry(Indices[I])+24)&&R->AvailableLength==U64(Entry(Indices[I])+64));
      Positive+=R->AvailableLength!=0;DeclaredSum+=R->RawSize;CurrentSum+=R->AvailableLength;
    }
    assert(Positive==11&&DeclaredSum==0x400000000ULL&&CurrentSum==0x3ea85d000ULL);
    assert(Report.Banks[3].Base==0xd8600000&&!Report.Banks[3].AvailableLength);
    assert(!Report.Banks[9].RawSize&&!Report.Banks[10].RawSize&&!Report.Banks[11].RawSize);
    ++Cases;puts("Physical test95 CRC7C271814 actual C observation: 12 native current records, 11 positive, 3 unknown/ignored; zero allocation/map authority");
  }
  printf("Actual v3 current-slice observation parser: %u synthetic/physical cases, legacy v1/v2 gates and future-version refusal retained\n",Cases);return 0;
}
