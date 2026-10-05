// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmemRam.h"
#include <Library/BaseMemoryLib.h>

typedef struct {
  CONST PIANO_SMEM_READER *Reader;
  PIANO_SMEM_RAM_WORK *Work;
  PIANO_SMEM_RAM_REPORT *Report;
  UINT32 TraceUsed, TraceCursor;
  BOOLEAN Compare;
} READ_STATE;
typedef struct { UINT64 Address; UINT32 Bytes, Version; } LOCATION;

static UINT16 U16(CONST UINT8 *P) { return (UINT16)(P[0] | ((UINT16)P[1] << 8)); }
static UINT32 U32(CONST UINT8 *P) { return (UINT32)P[0] | ((UINT32)P[1]<<8) | ((UINT32)P[2]<<16) | ((UINT32)P[3]<<24); }
static UINT64 U64(CONST UINT8 *P) { return U32(P) | ((UINT64)U32(P+4)<<32); }
static VOID Put32(UINT8 *P, UINT32 V) { UINTN I; for (I=0;I<4;++I) P[I]=(UINT8)(V>>(8*I)); }
static VOID Put64(UINT8 *P, UINT64 V) { UINTN I; for (I=0;I<8;++I) P[I]=(UINT8)(V>>(8*I)); }
static BOOLEAN Span(UINT64 Offset, UINT64 Bytes, UINT64 Limit) { return Offset<=Limit && Bytes<=Limit-Offset; }
static BOOLEAN Alias(CONST VOID *A, UINTN An, CONST VOID *B, UINTN Bn) {
  UINTN Av=(UINTN)A, Bv=(UINTN)B;
  if (An>MAX_UINTN-Av || Bn>MAX_UINTN-Bv) return TRUE;
  return Av<Bv+Bn && Bv<Av+An;
}
static EFI_STATUS Fail(PIANO_SMEM_RAM_REPORT *R, EFI_STATUS Status, PIANO_SMEM_REASON Reason) {
  R->Status=Status; R->Reason=Reason; return Status;
}
static VOID ClearParsed(PIANO_SMEM_RAM_REPORT *R) {
  R->Parsed=FALSE; R->BankCount=R->PreloadedCount=R->OtherCategoryCount=0;
  ZeroMem(R->Banks,sizeof(R->Banks)); ZeroMem(R->Preloaded,sizeof(R->Preloaded));
}
static UINT32 Crc(CONST UINT8 *P, UINTN Bytes) {
  UINT32 C=0xffffffffU; UINTN I; UINT32 J;
  for(I=0;I<Bytes;++I) { C^=P[I]; for(J=0;J<8;++J) C=(C>>1)^((C&1)?0xedb88320U:0); }
  return ~C;
}
static EFI_STATUS Parse(CONST UINT8 *P, UINTN Bytes, PIANO_SMEM_RAM_REPORT *R) {
  UINT32 Version, Count, Stride, I, J, Type, Category; UINT64 Base, Size, Available;
  PIANO_SMEM_RAM_ENTRY *Out;
  if(Bytes<24 || Bytes>PIANO_SMEM_PAYLOAD_MAX) return Fail(R,EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  if(U32(P)!=0x9da5e0a8U || U32(P+4)!=0xaf9ec4e2U) return Fail(R,EFI_COMPROMISED_DATA,PianoSmemReasonRamMagic);
  Version=U32(P+8); R->RamVersion=Version;
  if(Version!=1 && Version!=2) return Fail(R,EFI_UNSUPPORTED,PianoSmemReasonRamVersion);
  Count=U32(P+16); R->RawEntryCount=Count; Stride=Version==1?64:72;
  if(!Count || Count>PIANO_SMEM_RAM_MAX || Count>(Bytes-24)/Stride) return Fail(R,EFI_BAD_BUFFER_SIZE,PianoSmemReasonRamCount);
  for(I=0;I<Count;++I) {
    CONST UINT8 *E=P+24+(UINTN)I*Stride;
    Category=U32(E+0x24); Type=U32(E+0x2c);
    if(Category!=14) { ++R->OtherCategoryCount; continue; }
    Base=U64(E+0x10); Size=U64(E+0x18); Available=Version==1?Size:U64(E+0x40);
    if(!Size || Base>MAX_UINT64-Size || (Type==1 && (!Available || Available>Size || Base>MAX_UINT64-Available)))
      return Fail(R,EFI_COMPROMISED_DATA,PianoSmemReasonRamRange);
    if(Type==1) {
      // Check raw physical bank claims, not preloaded ranges: a preloaded
      // reservation may legitimately lie inside a bank. End sums are already
      // checked above (and were checked when earlier banks were accepted).
      for(J=0;J<R->BankCount;++J)
        if(Base<R->Banks[J].Base+R->Banks[J].RawSize && R->Banks[J].Base<Base+Size)
          return Fail(R,EFI_COMPROMISED_DATA,PianoSmemReasonRamOverlap);
      Out=&R->Banks[R->BankCount++];
    }
    else if(Type>=5 && Type<=(Version==1?8U:9U)) Out=&R->Preloaded[R->PreloadedCount++];
    else return Fail(R,EFI_UNSUPPORTED,PianoSmemReasonRamType);
    Out->Base=Base; Out->RawSize=Size; Out->AvailableLength=Type==1?Available:0;
    Out->RawType=Type; Out->SourceIndex=I;
  }
  if(!R->BankCount) return Fail(R,EFI_NOT_FOUND,PianoSmemReasonMissing);
  R->Parsed=TRUE; R->Status=EFI_SUCCESS; R->Reason=PianoSmemReasonNone;
  return EFI_SUCCESS;
}
EFI_STATUS PianoSmemRamParse(CONST VOID *Payload, UINTN Bytes, PIANO_SMEM_RAM_REPORT *R) {
  EFI_STATUS Status;
  if(!Payload || !R || Bytes>MAX_UINTN-(UINTN)Payload || Alias(Payload,Bytes,R,sizeof(*R))) return EFI_INVALID_PARAMETER;
  ZeroMem(R,sizeof(*R)); R->CookieStatus=EFI_NOT_READY;
  Status=Parse(Payload,Bytes,R); if(Status!=EFI_SUCCESS) ClearParsed(R);
  return Status;
}
static EFI_STATUS Read(READ_STATE *S, UINT64 Address, UINTN Bytes, VOID *Dest) {
  EFI_STATUS Status; PIANO_SMEM_RAM_REPORT *R=S->Report;
  if(!Bytes || Bytes>PIANO_SMEM_READ_MAX ||
    !((Address>=PIANO_SMEM_BASE && Span(Address-PIANO_SMEM_BASE,Bytes,PIANO_SMEM_BYTES)) ||
      ((Address==PIANO_SMEM_COOKIE_LOW || Address==PIANO_SMEM_COOKIE_HIGH) && Bytes==4)))
    return Fail(R,EFI_ACCESS_DENIED,PianoSmemReasonBounds);
  if(R->ReadCalls>=S->Reader->MaxReadCalls || Bytes>S->Reader->MaxReadBytes-R->ReadBytes)
    return Fail(R,EFI_TIMEOUT,PianoSmemReasonBudget);
  ++R->ReadCalls; R->ReadBytes+=(UINT32)Bytes;
  Status=S->Reader->TryRead(S->Reader->Context,Address,Bytes,Dest);
  if(Status!=EFI_SUCCESS) { R->CallbackStatus=Status; return Fail(R,EFI_DEVICE_ERROR,PianoSmemReasonRead); }
  return EFI_SUCCESS;
}
static EFI_STATUS Meta(READ_STATE *S, UINT64 Address, UINTN Bytes, UINT8 *Dest) {
  EFI_STATUS Status; UINT8 Prefix[12]; UINT32 Amount=(UINT32)Bytes+12;
  if(!Bytes || Bytes>64) return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  Status=Read(S,Address,Bytes,Dest); if(Status!=EFI_SUCCESS) return Status;
  Put64(Prefix,Address); Put32(Prefix+8,(UINT32)Bytes);
  if(S->Compare) {
    if(!Span(S->TraceCursor,Amount,S->TraceUsed) ||
       CompareMem(S->Work->Metadata+S->TraceCursor,Prefix,12) ||
       CompareMem(S->Work->Metadata+S->TraceCursor+12,Dest,Bytes))
      return Fail(S->Report,EFI_MEDIA_CHANGED,PianoSmemReasonUnstable);
    S->TraceCursor+=Amount;
  } else {
    if(!Span(S->TraceUsed,Amount,PIANO_SMEM_TRACE_MAX)) return Fail(S->Report,EFI_OUT_OF_RESOURCES,PianoSmemReasonBudget);
    CopyMem(S->Work->Metadata+S->TraceUsed,Prefix,12);
    CopyMem(S->Work->Metadata+S->TraceUsed+12,Dest,Bytes); S->TraceUsed+=Amount;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS Payload(READ_STATE *S, LOCATION *L, UINT8 *Dest) {
  UINT32 Offset=0, Bytes; EFI_STATUS Status;
  while(Offset<L->Bytes) {
    Bytes=L->Bytes-Offset; if(Bytes>PIANO_SMEM_READ_MAX) Bytes=PIANO_SMEM_READ_MAX;
    Status=Read(S,L->Address+Offset,Bytes,Dest+Offset); if(Status!=EFI_SUCCESS) return Status;
    Offset+=Bytes;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS Found(READ_STATE *S, LOCATION *L, UINT64 Address, UINT32 Bytes) {
  if(L->Address) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonDuplicate);
  if(Bytes<24 || Bytes>PIANO_SMEM_PAYLOAD_MAX || Address<PIANO_SMEM_BASE || !Span(Address-PIANO_SMEM_BASE,Bytes,PIANO_SMEM_BYTES))
    return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  L->Address=Address; L->Bytes=Bytes; return EFI_SUCCESS;
}
static EFI_STATUS Legacy(READ_STATE *S, LOCATION *L, UINT32 Free) {
  UINT8 E[16]; UINT32 Offset, Size, Aux; EFI_STATUS Status;
  Status=Meta(S,PIANO_SMEM_BASE+0xd0+16*PIANO_SMEM_RAM_ITEM,16,E); if(Status!=EFI_SUCCESS) return Status;
  if(!U32(E)) return Fail(S->Report,EFI_NOT_FOUND,PianoSmemReasonMissing);
  if(U32(E)!=1) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  Offset=U32(E+4); Size=U32(E+8); Aux=U32(E+12)&0xfffffffcU;
  if(Aux && Aux!=(UINT32)PIANO_SMEM_BASE) return Fail(S->Report,EFI_UNSUPPORTED,PianoSmemReasonAuxRegion);
  if((Offset&7) || (Size&7) || Offset<0x20d0 || !Span(Offset,Size,Free)) return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
  return Found(S,L,PIANO_SMEM_BASE+Offset,Size);
}
static EFI_STATUS Partition(READ_STATE *S, LOCATION *L, UINT32 Offset, UINT32 Size, UINT32 Line) {
  UINT8 H[32], E[16]; UINT32 Uncached, Cached, At, Next, Data, DataSize, Pad, HeaderSize, Items=0;
  EFI_STATUS Status;
  if(!Offset || Offset<0xd0 || Size<32 || !Span(Offset,Size,PIANO_SMEM_BYTES-4096) || !Line || (Line&(Line-1)) || Line<8 || Line>4096)
    return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  Status=Meta(S,PIANO_SMEM_BASE+Offset,32,H); if(Status!=EFI_SUCCESS) return Status;
  if(U32(H)!=0x54525024U || U16(H+4)!=0xfffe || U16(H+6)!=0xfffe || U32(H+8)!=Size)
    return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  Uncached=U32(H+12); Cached=U32(H+16); HeaderSize=(16+Line-1)&~(Line-1);
  if(Uncached<32 || Uncached>Cached || Cached>Size || (Uncached&7) || (Cached&(Line-1)) || (Size&(Line-1)))
    return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonPartition);
  At=32;
  while(At<Uncached) {
    if(++Items>512 || !Span(At,16,Uncached)) return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonEntry);
    Status=Meta(S,PIANO_SMEM_BASE+Offset+At,16,E); if(Status!=EFI_SUCCESS) return Status;
    DataSize=U32(E+4); Pad=U16(E+8); Data=At+16+U16(E+10);
    if(U16(E)!=0xa5a5 || !DataSize || Pad>DataSize || (DataSize&7) || !Span(Data,DataSize,Uncached))
      return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
    Next=Data+DataSize; if(Next<=At) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
    if(U16(E+2)==PIANO_SMEM_RAM_ITEM) { Status=Found(S,L,PIANO_SMEM_BASE+Offset+Data,DataSize-Pad); if(Status!=EFI_SUCCESS) return Status; }
    At=Next;
  }
  if(At!=Uncached) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  // Linux cached headers are at high addresses; payload precedes the header.
  At=Size;
  while(At>Cached) {
    if(++Items>512 || At-Cached<HeaderSize) return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonEntry);
    At-=HeaderSize;
    Status=Meta(S,PIANO_SMEM_BASE+Offset+At,16,E); if(Status!=EFI_SUCCESS) return Status;
    DataSize=U32(E+4); Pad=U16(E+8);
    if(U16(E)!=0xa5a5 || !DataSize || Pad>DataSize || (DataSize&(Line-1)) || U16(E+10)>HeaderSize-16 || DataSize>At-Cached)
      return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
    Data=At-DataSize;
    if(U16(E+2)==PIANO_SMEM_RAM_ITEM) { Status=Found(S,L,PIANO_SMEM_BASE+Offset+Data,DataSize-Pad); if(Status!=EFI_SUCCESS) return Status; }
    At=Data;
  }
  if(At!=Cached) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonEntry);
  return L->Address?EFI_SUCCESS:Fail(S->Report,EFI_NOT_FOUND,PianoSmemReasonMissing);
}
static EFI_STATUS Global(READ_STATE *S, LOCATION *L) {
  UINT8 H[32], E[48]; UINT32 Count, I, Offset=0, Size=0, Line=0; EFI_STATUS Status;
  UINT64 Table=PIANO_SMEM_BASE+PIANO_SMEM_BYTES-4096;
  Status=Meta(S,Table,32,H); if(Status!=EFI_SUCCESS) return Status;
  Count=U32(H+8);
  if(U32(H)!=0x434f5424U || U32(H+4)!=1 || !Count || Count>(4096-32)/48)
    return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonTable);
  for(I=0;I<Count;++I) {
    Status=Meta(S,Table+32+(UINT64)I*48,48,E); if(Status!=EFI_SUCCESS) return Status;
    if(U16(E+12)!=0xfffe || U16(E+14)!=0xfffe || !U32(E) || !U32(E+4)) continue;
    if(Offset) return Fail(S->Report,EFI_COMPROMISED_DATA,PianoSmemReasonDuplicate);
    Offset=U32(E); Size=U32(E+4); Line=U32(E+16);
  }
  if(!Offset) return Fail(S->Report,EFI_NOT_FOUND,PianoSmemReasonMissing);
  return Partition(S,L,Offset,Size,Line);
}
static EFI_STATUS Locate(READ_STATE *S, LOCATION *L) {
  UINT8 V[4], H[16]; UINT32 Major, Free, Available; EFI_STATUS Status;
  ZeroMem(L,sizeof(*L));
  Status=Meta(S,PIANO_SMEM_BASE+0x5c,4,V); if(Status!=EFI_SUCCESS) return Status;
  Status=Meta(S,PIANO_SMEM_BASE+0xc0,16,H); if(Status!=EFI_SUCCESS) return Status;
  L->Version=U32(V); Major=L->Version>>16;
  if(U32(H)!=1 || U32(H+12)) return Fail(S->Report,EFI_NOT_READY,PianoSmemReasonUninitialized);
  if(Major==11) {
    Free=U32(H+4); Available=U32(H+8);
    if(Free<0x20d0 || !Span(Free,Available,PIANO_SMEM_BYTES)) return Fail(S->Report,EFI_BAD_BUFFER_SIZE,PianoSmemReasonBounds);
    return Legacy(S,L,Free);
  }
  if(Major==12) return Global(S,L);
  return Fail(S->Report,EFI_UNSUPPORTED,PianoSmemReasonMajor);
}
static EFI_STATUS Cookie(READ_STATE *S, UINT64 *Value) {
  UINT8 Low[4], High[4]; EFI_STATUS Status;
  Status=Read(S,PIANO_SMEM_COOKIE_LOW,4,Low); if(Status!=EFI_SUCCESS) return Status;
  Status=Read(S,PIANO_SMEM_COOKIE_HIGH,4,High); if(Status!=EFI_SUCCESS) return Status;
  *Value=U32(Low)|((UINT64)U32(High)<<32); return EFI_SUCCESS;
}
EFI_STATUS PianoSmemRamCollect(CONST PIANO_SMEM_READER *Reader, PIANO_SMEM_RAM_WORK *Work, PIANO_SMEM_RAM_REPORT *R) {
  READ_STATE S; LOCATION A, B; UINT64 CookieAgain=0; EFI_STATUS Status, CookieFirst;
  if(!Reader || !Work || !R || !Reader->TryRead || !Reader->MaxReadCalls || Reader->MaxReadCalls>PIANO_SMEM_CALLS_MAX ||
     !Reader->MaxReadBytes || Reader->MaxReadBytes>PIANO_SMEM_TOTAL_MAX ||
     Alias(Reader,sizeof(*Reader),Work,sizeof(*Work)) || Alias(Reader,sizeof(*Reader),R,sizeof(*R)) || Alias(Work,sizeof(*Work),R,sizeof(*R))) return EFI_INVALID_PARAMETER;
  if(Work->Busy) return EFI_ALREADY_STARTED;
  Work->Busy=TRUE; ZeroMem(R,sizeof(*R)); ZeroMem(&S,sizeof(S));
  S.Reader=Reader; S.Work=Work; S.Report=R;
  // Cookie is an optional diagnostic observation, not an address source.
  CookieFirst=Cookie(&S,&R->CookieValue); R->CookieStatus=CookieFirst;
  if(CookieFirst!=EFI_SUCCESS && R->Reason==PianoSmemReasonBudget) { Status=CookieFirst; goto Done; }
  R->Status=EFI_SUCCESS; R->Reason=PianoSmemReasonNone;
  Status=Locate(&S,&A); R->SmemVersion=A.Version; if(Status!=EFI_SUCCESS) goto Done;
  R->PayloadAddress=A.Address; R->PayloadBytes=A.Bytes;
  Status=Payload(&S,&A,Work->Payload[0]); if(Status!=EFI_SUCCESS) goto Done;
  R->PayloadCrc32=Crc(Work->Payload[0],A.Bytes); R->MetadataBytes=S.TraceUsed;
  S.Compare=TRUE;
  Status=Locate(&S,&B); if(Status!=EFI_SUCCESS) goto Done;
  if(S.TraceCursor!=S.TraceUsed || A.Address!=B.Address || A.Bytes!=B.Bytes || A.Version!=B.Version) { Status=Fail(R,EFI_MEDIA_CHANGED,PianoSmemReasonUnstable); goto Done; }
  R->RepeatedMetadataEqual=TRUE;
  Status=Payload(&S,&B,Work->Payload[1]); if(Status!=EFI_SUCCESS) goto Done;
  if(CompareMem(Work->Payload[0],Work->Payload[1],A.Bytes)) { Status=Fail(R,EFI_MEDIA_CHANGED,PianoSmemReasonUnstable); goto Done; }
  R->RepeatedPayloadEqual=TRUE;
  if(CookieFirst==EFI_SUCCESS) {
    R->CookieStatus=Cookie(&S,&CookieAgain);
    if(R->CookieStatus!=EFI_SUCCESS && R->Reason==PianoSmemReasonBudget) { Status=R->CookieStatus; goto Done; }
    if(R->CookieStatus==EFI_SUCCESS) R->CookieRepeatedEqual=R->CookieValue==CookieAgain;
  }
  Status=Parse(Work->Payload[0],A.Bytes,R);
Done:
  if(Status!=EFI_SUCCESS) { ClearParsed(R); R->Status=Status; }
  Work->Busy=FALSE; return Status;
}
