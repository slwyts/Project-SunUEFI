// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual source, injected recoverable byte reads. No device or native calls.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#undef NULL
#include "PianoSmemRam.h"
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static UINT8 Memory[PIANO_SMEM_BYTES], Raw[PIANO_SMEM_PAYLOAD_MAX];
static PIANO_SMEM_RAM_WORK Work;
static PIANO_SMEM_RAM_REPORT Report, NestedReport;
static PIANO_SMEM_READER Reader;
static UINTN Calls, DataReads, PayloadOffset;
static UINT64 FailureAddress, MaximumAddress;
static EFI_STATUS FailureStatus;
static UINT32 Change, CookieReads;
static BOOLEAN Reenter;
static UINTN Cases;
static void p16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
static void p32(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static void p64(UINT8 *P,UINT64 V){p32(P,(UINT32)V);p32(P+4,(UINT32)(V>>32));}
static EFI_STATUS EFIAPI TryRead(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Dest){
  (void)Context; ++Calls; assert(Bytes && Bytes<=PIANO_SMEM_READ_MAX);
  if(Reenter){Reenter=FALSE;assert(PianoSmemRamCollect(&Reader,&Work,&NestedReport)==EFI_ALREADY_STARTED);}
  if(Address==FailureAddress)return FailureStatus;
  if(Address==PIANO_SMEM_COOKIE_LOW || Address==PIANO_SMEM_COOKIE_HIGH){
    assert(Bytes==4);++CookieReads;p32(Dest,Address==PIANO_SMEM_COOKIE_LOW?0x12345678:0x87654321);
    if(Change==3 && CookieReads>2 && Address==PIANO_SMEM_COOKIE_LOW)p32(Dest,0x87654321);
    return EFI_SUCCESS;
  }
  assert(Address>=PIANO_SMEM_BASE && Address-PIANO_SMEM_BASE<=PIANO_SMEM_BYTES && Bytes<=PIANO_SMEM_BYTES-(Address-PIANO_SMEM_BASE));
  if(Address+Bytes>MaximumAddress)MaximumAddress=Address+Bytes;
  if(Address==PIANO_SMEM_BASE+PayloadOffset && ++DataReads==2 && Change==1)Memory[PayloadOffset+32]^=1;
  memcpy(Dest,Memory+(Address-PIANO_SMEM_BASE),Bytes);
  if(Change==2 && Address==PIANO_SMEM_BASE+0x5c && DataReads)p32(Dest,0xb0002);
  return EFI_SUCCESS;
}
static UINTN payload(UINT32 Version){
  UINT32 Stride=Version==1?64:72;memset(Raw,0,sizeof(Raw));
  p32(Raw,0x9da5e0a8);p32(Raw+4,0xaf9ec4e2);p32(Raw+8,Version);p32(Raw+16,3);
  UINT8 *E=Raw+24;p64(E+16,0x80000000);p64(E+24,0x200000000ULL);p32(E+36,14);p32(E+44,1);if(Version==2)p64(E+64,0x1ff000000ULL);
  E+=Stride;p64(E+16,0xa0000000);p64(E+24,0x100000);p32(E+36,14);p32(E+44,5);
  E+=Stride;p32(E+36,13);p32(E+44,99);
  return 24+Stride*3;
}
static void reset(void){
  memset(Memory,0,sizeof(Memory));memset(&Work,0,sizeof(Work));memset(&Report,0xcc,sizeof(Report));
  Reader=(PIANO_SMEM_READER){NULL,TryRead,PIANO_SMEM_CALLS_MAX,PIANO_SMEM_TOTAL_MAX};
  Calls=DataReads=CookieReads=0;FailureAddress=MaximumAddress=0;FailureStatus=EFI_DEVICE_ERROR;Change=0;Reenter=FALSE;PayloadOffset=0x3000;
  p32(Memory+0xc0,1);p32(Memory+0xc4,0x100000);p32(Memory+0xc8,0x100000);
}
static void legacy(UINT32 Version){
  reset();UINTN Bytes=payload(Version);p32(Memory+0x5c,0xb0001);UINT8 *E=Memory+0xd0+402*16;
  p32(E,1);p32(E+4,(UINT32)PayloadOffset);p32(E+8,(UINT32)Bytes);memcpy(Memory+PayloadOffset,Raw,Bytes);
}
static void part(BOOLEAN Cached){
  reset();UINTN Bytes=payload(2);p32(Memory+0x5c,0xc0001);
  UINT8 *T=Memory+PIANO_SMEM_BYTES-4096;p32(T,0x434f5424);p32(T+4,1);p32(T+8,1);
  UINT8 *E=T+32;p32(E,0x4000);p32(E+4,0x4000);p16(E+12,0xfffe);p16(E+14,0xfffe);p32(E+16,64);
  UINT8 *H=Memory+0x4000;p32(H,0x54525024);p16(H+4,0xfffe);p16(H+6,0xfffe);p32(H+8,0x4000);
  if(!Cached){E=H+32;p16(E,0xa5a5);p16(E+2,402);p32(E+4,(UINT32)Bytes);p32(H+12,(UINT32)(32+16+Bytes));p32(H+16,0x4000);PayloadOffset=0x4000+48;}
  else {UINT32 Rounded=((UINT32)Bytes+63)&~63U;E=H+0x4000-64;p16(E,0xa5a5);p16(E+2,402);p32(E+4,Rounded);p16(E+8,(UINT16)(Rounded-Bytes));p32(H+12,32);p32(H+16,0x4000-64-Rounded);PayloadOffset=0x4000+0x4000-64-Rounded;}
  memcpy(Memory+PayloadOffset,Raw,Bytes);
}
static void result(EFI_STATUS Status,PIANO_SMEM_REASON Reason){
  assert(PianoSmemRamCollect(&Reader,&Work,&Report)==Status);assert(Report.Status==Status && Report.Reason==Reason && !Work.Busy);
  if(Status!=EFI_SUCCESS){assert(!Report.Parsed && !Report.BankCount && !Report.PreloadedCount && !Report.OtherCategoryCount);}
  assert(Calls==Report.ReadCalls && Report.ReadBytes<=Reader.MaxReadBytes);++Cases;
}
static void parsed(UINT32 Version){
  assert(Report.Parsed && Report.RamVersion==Version && Report.BankCount==1 && Report.PreloadedCount==1 && Report.OtherCategoryCount==1);
  assert(Report.Banks[0].Base==0x80000000 && Report.Banks[0].RawSize==0x200000000ULL);
  assert(Report.Banks[0].AvailableLength==(Version==1?0x200000000ULL:0x1ff000000ULL));
  assert(Report.Preloaded[0].RawType==5 && Report.Preloaded[0].RawSize==0x100000 && !Report.Preloaded[0].AvailableLength);
}
int main(void){
  UINTN Bytes=payload(1);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS);parsed(1);++Cases;
  Bytes=payload(2);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS);parsed(2);++Cases;
  p32(Raw+8,3);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS&&Report.RamVersion==3);parsed(3);++Cases;
  p32(Raw+8,4);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_UNSUPPORTED && Report.Reason==PianoSmemReasonRamVersion && !Report.Parsed);++Cases;
  Bytes=payload(1);p32(Raw,0);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA);++Cases;
  Bytes=payload(1);p32(Raw+16,65);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_BAD_BUFFER_SIZE);++Cases;
  p32(Raw+16,0);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_BAD_BUFFER_SIZE);++Cases;
  Bytes=payload(1);assert(PianoSmemRamParse(Raw,Bytes-1,&Report)==EFI_BAD_BUFFER_SIZE);++Cases;
  p32(Raw+24+44,2);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_UNSUPPORTED && !Report.BankCount && !Report.PreloadedCount);++Cases;
  Bytes=payload(1);p64(Raw+24+16,MAX_UINT64-15);p64(Raw+24+24,16);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA);++Cases;
  Bytes=payload(2);p64(Raw+24+64,0x200000001ULL);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA);++Cases;
  Bytes=payload(2);p64(Raw+24+64,0);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA);++Cases;
  Bytes=payload(1);p32(Raw+24+64+44,9);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_UNSUPPORTED);++Cases;
  Bytes=payload(2);p32(Raw+24+72+44,9);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS && Report.Preloaded[0].RawType==9);++Cases;
  Bytes=payload(1);p32(Raw+16,0xffffffff);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_BAD_BUFFER_SIZE && !Report.BankCount);++Cases;
  Bytes=payload(1);p64(Raw+24+24,MAX_UINT64);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA && !Report.BankCount);++Cases;
  Bytes=payload(1);p32(Raw+24+64+44,1);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA && Report.Reason==PianoSmemReasonRamOverlap && !Report.OtherCategoryCount && !Report.BankCount);++Cases;
  Bytes=payload(1);memcpy(Raw+24+64,Raw+24,64);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_COMPROMISED_DATA && Report.Reason==PianoSmemReasonRamOverlap && !Report.BankCount);++Cases;
  Bytes=payload(1);p32(Raw+24+64+44,1);p64(Raw+24+64+16,0x280000000ULL);assert(PianoSmemRamParse(Raw,Bytes,&Report)==EFI_SUCCESS && Report.BankCount==2);++Cases;
  assert(PianoSmemRamParse((VOID *)(UINTN)(MAX_UINTN-15),24,&Report)==EFI_INVALID_PARAMETER);++Cases;
  assert(PianoSmemRamParse(&Report,sizeof(Report),&Report)==EFI_INVALID_PARAMETER);++Cases;
  legacy(1);Reenter=TRUE;result(EFI_SUCCESS,PianoSmemReasonNone);parsed(1);assert(Report.RepeatedMetadataEqual && Report.RepeatedPayloadEqual && Report.CookieRepeatedEqual && Report.CookieValue==0x8765432112345678ULL && MaximumAddress<PIANO_SMEM_BASE+PIANO_SMEM_BYTES);++Cases;
  legacy(2);result(EFI_SUCCESS,PianoSmemReasonNone);parsed(2);
  legacy(1);p32(Memory+0x5c,0xd0001);result(EFI_UNSUPPORTED,PianoSmemReasonMajor);
  legacy(1);p32(Memory+0xc0,0);result(EFI_NOT_READY,PianoSmemReasonUninitialized);
  legacy(1);p32(Memory+0xcc,1);result(EFI_NOT_READY,PianoSmemReasonUninitialized);
  legacy(1);p32(Memory+0xd0+402*16,0);result(EFI_NOT_FOUND,PianoSmemReasonMissing);
  legacy(1);p32(Memory+0xd0+402*16+12,0x90000000);result(EFI_UNSUPPORTED,PianoSmemReasonAuxRegion);
  legacy(1);p32(Memory+0xd0+402*16+12,(UINT32)PIANO_SMEM_BASE|3);result(EFI_SUCCESS,PianoSmemReasonNone);
  legacy(1);p32(Memory+0xd0+402*16+4,0xffffff00);result(EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  legacy(1);p32(Memory+0xd0+402*16+8,PIANO_SMEM_PAYLOAD_MAX+8);result(EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  legacy(1);FailureAddress=PIANO_SMEM_BASE+PayloadOffset;result(EFI_DEVICE_ERROR,PianoSmemReasonRead);assert(Report.CallbackStatus==EFI_DEVICE_ERROR);
  legacy(1);FailureAddress=PIANO_SMEM_BASE+0x5c;FailureStatus=EFI_WARN_STALE_DATA;result(EFI_DEVICE_ERROR,PianoSmemReasonRead);assert(Report.CallbackStatus==EFI_WARN_STALE_DATA);
  legacy(1);Reader.MaxReadCalls=2;result(EFI_TIMEOUT,PianoSmemReasonBudget);assert(Calls==2);
  legacy(1);Reader.MaxReadBytes=8;result(EFI_TIMEOUT,PianoSmemReasonBudget);assert(Report.ReadBytes==8);
  legacy(1);Change=1;result(EFI_MEDIA_CHANGED,PianoSmemReasonUnstable);assert(Report.RepeatedMetadataEqual && !Report.RepeatedPayloadEqual);
  legacy(1);Change=2;result(EFI_MEDIA_CHANGED,PianoSmemReasonUnstable);assert(!Report.RepeatedMetadataEqual);
  legacy(1);Change=3;result(EFI_SUCCESS,PianoSmemReasonNone);assert(!Report.CookieRepeatedEqual);
  legacy(1);FailureAddress=PIANO_SMEM_COOKIE_LOW;result(EFI_SUCCESS,PianoSmemReasonNone);assert(Report.CookieStatus==EFI_DEVICE_ERROR && !Report.CookieRepeatedEqual);
  part(FALSE);result(EFI_SUCCESS,PianoSmemReasonNone);parsed(2);assert(Report.SmemVersion==0xc0001 && Report.PayloadAddress==PIANO_SMEM_BASE+PayloadOffset);
  part(TRUE);result(EFI_SUCCESS,PianoSmemReasonNone);parsed(2);
  part(FALSE);p32(Memory+PIANO_SMEM_BYTES-4096+4,2);result(EFI_COMPROMISED_DATA,PianoSmemReasonTable);
  part(FALSE);p32(Memory+PIANO_SMEM_BYTES-4096+8,0xffffffff);result(EFI_COMPROMISED_DATA,PianoSmemReasonTable);
  part(FALSE);p32(Memory+PIANO_SMEM_BYTES-4096+32,0xffffff00);result(EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  part(FALSE);p32(Memory+PIANO_SMEM_BYTES-4096+32+16,3);result(EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  part(FALSE);p16(Memory+0x4000+4,0);result(EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  part(FALSE);p32(Memory+0x4000+12,0x4001);result(EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  part(FALSE);p16(Memory+0x4000+32,0);result(EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  part(FALSE);p16(Memory+0x4000+32+8,0xffff);result(EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  part(FALSE);p32(Memory+0x4000+32+4,0xffffffff);result(EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  part(FALSE);p16(Memory+0x4000+32+2,401);result(EFI_NOT_FOUND,PianoSmemReasonMissing);
  part(TRUE);p32(Memory+0x4000+0x4000-64+4,0xffffffc0);result(EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  part(FALSE);UINT8 *T=Memory+PIANO_SMEM_BYTES-4096;p32(T+8,2);memcpy(T+80,T+32,48);result(EFI_COMPROMISED_DATA,PianoSmemReasonDuplicate);
  // A duplicate item in one global partition is rejected, including mixed lists.
  part(FALSE);UINT32 Size=256;UINT8 *E=Memory+0x4000+0x4000-64;p16(E,0xa5a5);p16(E+2,402);p32(E+4,Size);p32(Memory+0x4000+16,0x4000-64-Size);result(EFI_COMPROMISED_DATA,PianoSmemReasonDuplicate);
  legacy(1);Reader.MaxReadCalls=0;assert(PianoSmemRamCollect(&Reader,&Work,&Report)==EFI_INVALID_PARAMETER && !Calls);++Cases;
  legacy(1);Work.Busy=TRUE;assert(PianoSmemRamCollect(&Reader,&Work,&Report)==EFI_ALREADY_STARTED && !Calls);++Cases;
  legacy(1);assert(PianoSmemRamCollect(&Reader,&Work,(PIANO_SMEM_RAM_REPORT *)(void *)Work.Payload)==EFI_INVALID_PARAMETER && !Calls);++Cases;
  printf("Actual SMEM RAM402: %llu cases; strict v1/v2/v3, legacy11/global12 cached+uncached, fixed read bounds/budgets, exact repeated metadata/payload, no allocation/write/native/fallback/memory authority.\n",(unsigned long long)Cases);
  return 0;
}
