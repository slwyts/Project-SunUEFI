// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual OwnedSmmu source; no hardware, native HAL or device access.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoOwnedSmmu.c"

static PIANO_SMMU_SNAPSHOT observed;
static EFI_STATUS capture_status;
static BOOLEAN debug_enabled=TRUE;
static UINT32 native_detach_status,native_destroy_status;
static UINT32 live_smr[2],live_s2cr[2],expected_index;
static unsigned reads,detaches,destroys,frees,records;
static char lines[96][768];
static UINTN api[15];
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return debug_enabled;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return debug_enabled;}
VOID EFIAPI MemoryFence(VOID){ }
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Size){return memset(Buffer,0,Size);}

// Faithful adapter for the limited ASCII/unsigned formats used by the actual
// diagnostic. Formatting is mocked; CRC, sequence, record contents and source
// rejection/free decisions execute in the production implementation.
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Size,CONST CHAR8 *Format,...){
  char native[1024];size_t used=0;
  for(size_t i=0;Format[i];++i){
    assert(used+2<sizeof(native));
    if(Format[i]=='%' && Format[i+1]=='a'){native[used++]='%';native[used++]='s';++i;}
    else native[used++]=Format[i];
  }
  native[used]=0;va_list marker;va_start(marker,Format);
  int n=vsnprintf(Buffer,Size,native,marker);va_end(marker);
  assert(n>=0 && (size_t)n<Size);return (UINTN)n;
}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  if(strncmp(Format,"SUNUEFI_SMMU_OWNED_DIAG",23))return;
  assert(records<ARRAY_SIZE(lines));
  va_list marker;va_start(marker,Format);
  const char *body=va_arg(marker,const char *);UINT32 crc=va_arg(marker,UINT32);va_end(marker);
  assert(strlen(body)+strlen("SUNUEFI_SMMU_OWNED_DIAG_COPY ")+strlen(" crc32=12345678\n")<256);
  const char *prefix=strstr(Format,"_COPY ")?"COPY":"PRIMARY";
  int n=snprintf(lines[records++],sizeof(lines[0]),"%s %s crc32=%08X",prefix,body,crc);
  assert(n>0 && (size_t)n<sizeof(lines[0]));
}
UINT32 EFIAPI MmioRead32(UINTN Address){
  assert(reads<4);
  UINTN offset=Address-0x15000000U;
  UINT32 result;
  if((reads&1)==0){assert(offset==0x800+4*expected_index);result=live_smr[reads/2];}
  else {assert(offset==0xC00+4*expected_index);result=live_s2cr[reads/2];}
  ++reads;return result;
}
static UINT32 native_detach(VOID *Domain,CONST CHAR8 *Name,UINT32 Arid,UINT32 Flags){
  assert(Domain==(VOID *)123 && !strcmp(Name,"UFS_MEM") && !Arid && !Flags);
  ++detaches;return native_detach_status;
}
static UINT32 native_destroy(VOID *Domain){assert(Domain==(VOID *)123);++destroys;return native_destroy_status;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer){
  assert(!Buffer->Quarantined);++frees;Buffer->Signature=0;return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Snapshot){
  assert(!strcmp(Phase,"owned-detached"));*Snapshot=observed;return capture_status;
}
static PIANO_OWNED_SMMU *initial(void){
  reads=detaches=destroys=frees=records=0;memset(lines,0,sizeof(lines));
  native_detach_status=native_destroy_status=0;capture_status=EFI_SUCCESS;debug_enabled=TRUE;
  memset(api,0,sizeof(api));api[3]=(UINTN)native_detach;api[1]=(UINTN)native_destroy;
  PIANO_OWNED_SMMU *c=calloc(1,sizeof(*c));assert(c);
  c->Domain=(VOID *)123;c->Api=api;c->Attached=c->Verified=TRUE;c->ResourceName="UFS_MEM";
  c->TableMemory.Signature=123;c->Before.Valid=TRUE;c->Before.Groups=127;
  c->Before.Base=0x15000000;c->Before.Window=0x100000;c->Before.Banks=83;
  c->Before.RawSmr[0]=0x60;c->Before.RawS2cr[0]=0x20000;
  c->Before.RawSmr[1]=0x540;c->Before.RawS2cr[1]=0x20001;c->Before.RawS2cr[113]=0x0300006e;
  c->Before.Device[1].Sid=0x40;
  c->After=c->Before;c->After.Device[0]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Sid=0x60,.StreamIndex=0};
  observed=c->Before;expected_index=1;
  live_smr[0]=0;live_smr[1]=0x540;live_s2cr[0]=0;live_s2cr[1]=0x20001;
  return c;
}
static UINT32 independent_crc(const char *s,size_t n){
  UINT32 c=0xffffffff;
  for(size_t i=0;i<n;++i){c^=(unsigned char)s[i];for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320:0);}
  return ~c;
}
static void verify_records(void){
  assert(records && !(records&1));UINT32 previous=0;BOOLEAN have=FALSE;
  for(unsigned i=0;i<records;i+=2){
    assert(!strncmp(lines[i],"PRIMARY ",8) && !strncmp(lines[i+1],"COPY ",5));
    assert(!strcmp(lines[i]+8,lines[i+1]+5));
    const char *body=lines[i]+8,*crc=strstr(body," crc32=");assert(crc);
    unsigned value=0,seq=0;assert(sscanf(crc+7,"%x",&value)==1);
    assert(value==independent_crc(body,(size_t)(crc-body)));
    const char *q=strstr(body," seq=");assert(q && sscanf(q+5,"%u",&seq)==1);
    if(have)assert(seq==previous+1);
    previous=seq;have=TRUE;
  }
}
static const char *record_with(const char *needle){
  for(unsigned i=0;i<records;i+=2)if(strstr(lines[i],needle))return lines[i];
  assert(!"missing diagnostic");return NULL;
}
static void rejected(PIANO_OWNED_SMMU *c,UINT32 index,const char *reason){
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA);
  assert(c->TableMemory.Quarantined && !c->Attached && !c->Verified && c->Domain==(VOID *)123);
  assert(detaches==1 && !destroys && !frees && c->CloseDiagnostic.Valid);
  assert(c->CloseDiagnostic.Index==index && !strcmp(c->CloseDiagnostic.Reason,reason));
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED && detaches==1 && !destroys && !frees);
  verify_records();
}
int main(void){
  assert(DiagnosticCrc("123456789",9)==0xcbf43926 && DiagnosticCrc("",0)==0);
  PIANO_OWNED_SMMU *c=initial();
  observed.RawSmr[1]=observed.RawS2cr[1]=0;
  rejected(c,1,"other-raw");assert(reads==4 && c->CloseDiagnostic.LiveRead);
  const char *line=record_with("phase=close-rejected");
  assert(strstr(line,"idx=1 before_valid=1 after_valid=1") && strstr(line,"strict=1 retained=1"));
  line=record_with("phase=close-before");assert(strstr(line,"idx=1 smr=00000540 s2cr=00020001"));
  line=record_with("phase=close-after");assert(strstr(line,"idx=1 smr=00000000 s2cr=00000000"));
  line=record_with("sample=1 live_read=1");assert(strstr(line,"smr=00000000 s2cr=00000000"));
  line=record_with("sample=2 live_read=1");assert(strstr(line,"smr=00000540 s2cr=00020001"));
  // A second direct read that exactly matches baseline cannot turn failure into success.
  assert(c->Before.RawSmr[1]==0x540 && c->Before.RawS2cr[1]==0x20001);
  line=record_with("phase=peer-final");assert(strstr(line,"expected_sid=40") && strstr(line,"present=0 idx=65535") && strstr(line,"identity_proven=0"));
  record_with("phase=baseline-final");unsigned old_reads=reads;records=0;
  PianoOwnedSmmuReport(c);assert(reads==old_reads && !destroys && !frees);verify_records();free(c);

  c=initial();expected_index=113;observed.RawS2cr[113]=0x0300006d;
  live_smr[0]=live_smr[1]=0;live_s2cr[0]=0x0300006d;live_s2cr[1]=0x0300006e;
  rejected(c,113,"other-raw");assert(reads==4 && c->CloseDiagnostic.BeforeS2cr==0x0300006e);free(c);

  c=initial();observed.RawSmr[2]=BIT31|0x800;expected_index=2;
  rejected(c,2,"other-raw");assert(reads==4);free(c);

  c=initial();observed.Device[0].Present=TRUE;expected_index=0;
  rejected(c,0,"owned-still-present");assert(reads==4);free(c);

  c=initial();observed.Groups=126;expected_index=0;
  rejected(c,0,"group-count");assert(reads==4);free(c);

  c=initial();capture_status=EFI_DEVICE_ERROR;observed.Valid=FALSE;expected_index=0;
  rejected(c,0,"capture-error");assert(reads==4 && c->CloseDiagnostic.CaptureStatus==EFI_DEVICE_ERROR);free(c);

  c=initial();c->Before.Base=0x16000000;observed.RawSmr[1]=0;
  rejected(c,1,"other-raw");assert(!reads && !c->CloseDiagnostic.LiveRead);free(c);
  c=initial();c->Before.Window=0x200000;observed.RawSmr[1]=0;
  rejected(c,1,"other-raw");assert(!reads);free(c);
  c=initial();c->Before.Valid=FALSE;observed.RawSmr[1]=0;
  rejected(c,1,"other-raw");assert(!reads && !c->BaselineSaved);free(c);

  c=initial();observed.RawSmr[0]=0;observed.RawS2cr[0]=0;
  assert(PianoOwnedSmmuClose(c)==EFI_SUCCESS && detaches==1 && destroys==1 && frees==1 && !reads);
  assert(!c->CloseDiagnostic.Valid && !c->TableMemory.Quarantined);verify_records();free(c);

  c=initial();native_detach_status=1;
  assert(PianoOwnedSmmuClose(c)==EFI_DEVICE_ERROR && c->Attached && !reads && !destroys && !frees);verify_records();free(c);
  c=initial();native_destroy_status=1;
  assert(PianoOwnedSmmuClose(c)==EFI_DEVICE_ERROR && destroys==1 && !frees && !reads);verify_records();free(c);

  c=initial();c->After.Device[0].StreamIndex=7;
  c->Before.Device[1]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Sid=0x40,.StreamIndex=9,.Smr=BIT31|0x40,.S2cr=1};
  c->After.Device[1]=c->Before.Device[1];c->After.Device[1].StreamIndex=10;
  RememberBaseline(c);assert(c->BaselineSlotCount==6 && c->DiagnosticOwnedSlot==7);
  BaselineReport(c,"baseline-open");verify_records();
  record_with("idx=7 before_valid=");record_with("idx=9 before_valid=");record_with("idx=10 before_valid=");
  records=0;PianoOwnedSmmuReport(c);verify_records();line=record_with("phase=peer-final");assert(strstr(line,"idx=10") && strstr(line,"identity_proven=1"));free(c);

  c=initial();observed.RawSmr[1]=BIT31|0x40;observed.RawS2cr[1]=1;
  observed.Device[1]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Sid=0x40,.StreamIndex=1,.Smr=BIT31|0x40,.S2cr=1,.Type=0,.ContextBank=1};
  rejected(c,1,"other-raw");line=record_with("phase=peer-final");
  assert(strstr(line,"present=1 idx=1 sid=40") && strstr(line,"type=0 cb=1") && strstr(line,"identity_proven=1"));free(c);

  c=initial();c->Before.Groups=2;RememberBaseline(c);assert(c->BaselineSlotCount==2);free(c);
  c=initial();debug_enabled=FALSE;observed.RawSmr[1]=0;
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && !records && reads==4 && !destroys && !frees);free(c);
  puts("Owned SMMU diagnostics: strict rejection, actual failure index, double read, saved baseline/peer, CRC/mirror and no-MMIO reemit passed (17 cases).");
  return 0;
}
