// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoRamBootProbe.c"
static EFI_SYSTEM_TABLE st;
static EFI_BOOT_SERVICES bs;
static EFI_RUNTIME_SERVICES rt;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con;
static EFI_LOADED_IMAGE_PROTOCOL loaded;
static EFI_STATUS handle_status,get_status,set_status,console_status;
static unsigned handles,gets,sets,prints;
static BOOLEAN null_loaded;
static PIANO_RAM_BOOT_PROBE_RECORD saved;
static EFI_STATUS EFIAPI handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Result){
  assert(Image==(VOID *)9 && Guid->Data1==0x5b1b31a1);handles++;
  *Result=null_loaded?NULL:&loaded;return handle_status;
}
static EFI_STATUS EFIAPI get(CHAR16 *Name,EFI_GUID *Guid,UINT32 *Attr,UINTN *Bytes,VOID *Data){
  assert(!memcmp(Name,PIANO_RAM_BOOT_PROBE_NAME,sizeof(PIANO_RAM_BOOT_PROBE_NAME)) && Guid->Data1==0x97ed2b41);
  assert(Attr && Bytes && *Bytes==0 && !Data);gets++;return get_status;
}
static EFI_STATUS EFIAPI set(CHAR16 *Name,EFI_GUID *Guid,UINT32 Attr,UINTN Bytes,VOID *Data){
  assert(!memcmp(Name,PIANO_RAM_BOOT_PROBE_NAME,sizeof(PIANO_RAM_BOOT_PROBE_NAME)) && Guid->Data1==0x97ed2b41);
  assert(Attr==EFI_VARIABLE_BOOTSERVICE_ACCESS && !(Attr&(EFI_VARIABLE_NON_VOLATILE|EFI_VARIABLE_RUNTIME_ACCESS)));
  assert(Bytes==sizeof(saved) && Data);memcpy(&saved,Data,Bytes);sets++;return set_status;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text){assert(This==&con && Text[0]=='S');prints++;return console_status;}
static void reset(void){
  handles=gets=sets=prints=0;null_loaded=FALSE;handle_status=set_status=console_status=EFI_SUCCESS;get_status=EFI_NOT_FOUND;
  memset(&saved,0,sizeof(saved));
  loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=(VOID *)0x100000,.ImageSize=20480,.ParentHandle=(VOID *)7};
  bs=(EFI_BOOT_SERVICES){.Hdr={.Signature=EFI_BOOT_SERVICES_SIGNATURE,.Revision=0x20000,.HeaderSize=sizeof(bs)},.HandleProtocol=handle};
  rt=(EFI_RUNTIME_SERVICES){.Hdr={.Signature=EFI_RUNTIME_SERVICES_SIGNATURE,.HeaderSize=sizeof(rt)},.GetVariable=get,.SetVariable=set};
  con=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.OutputString=output};
  st=(EFI_SYSTEM_TABLE){.Hdr={.Signature=EFI_SYSTEM_TABLE_SIGNATURE,.Revision=0x20000,.HeaderSize=sizeof(st)},.BootServices=&bs,.RuntimeServices=&rt,.ConOut=&con};
}
static UINT32 independent_crc(const unsigned char *bytes,size_t count){unsigned value=0xffffffff;for(size_t i=0;i<count;i++){value^=bytes[i];for(unsigned j=0;j<8;j++)value=(value>>1)^((value&1)?0xedb88320:0);}return ~value;}
int main(void){
  reset();assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_SUCCESS);
  assert(handles==1 && gets==1 && sets==1 && prints==1 && saved.Signature==PIANO_RAM_BOOT_PROBE_SIGNATURE && saved.Bytes==sizeof(saved));
  assert(saved.ImageBase==0x100000 && saved.ImageBytes==20480 && saved.ImageHandle==9 && saved.ParentHandle==7);
  assert((saved.Flags&(BIT0|BIT2|BIT3))==(BIT0|BIT2|BIT3));
  UINT32 crc=saved.Crc32;saved.Crc32=0;assert(independent_crc((unsigned char *)&saved,sizeof(saved))==crc);
  reset();st.ConOut=NULL;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_SUCCESS && !prints && sets==1 && saved.ConsoleStatus==EFI_NOT_FOUND);
  reset();console_status=EFI_DEVICE_ERROR;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_SUCCESS && saved.ConsoleStatus==EFI_DEVICE_ERROR && !(saved.Flags&BIT3));
  reset();handle_status=EFI_WARN_UNKNOWN_GLYPH;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_DEVICE_ERROR && !gets && !sets);
  reset();null_loaded=TRUE;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_COMPROMISED_DATA && !gets && !sets);
  reset();loaded.ImageBase=NULL;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_COMPROMISED_DATA && !sets);
  for(unsigned i=0;i<2;i++){reset();get_status=i?EFI_SUCCESS:EFI_BUFFER_TOO_SMALL;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_ALREADY_STARTED && !sets);}
  reset();get_status=EFI_WARN_UNKNOWN_GLYPH;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_DEVICE_ERROR && !sets);
  reset();set_status=EFI_WARN_UNKNOWN_GLYPH;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_DEVICE_ERROR && sets==1);
  reset();set_status=EFI_WRITE_PROTECTED;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_WRITE_PROTECTED && sets==1);
  reset();st.Hdr.Signature=0;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_INVALID_PARAMETER && !handles);
  reset();rt.SetVariable=NULL;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_INVALID_PARAMETER && !handles);
  reset();PianoRamBootProbeRelocation=NULL;assert(PianoRamBootProbeEntry((VOID *)9,&st)==EFI_COMPROMISED_DATA && !handles);
  PianoRamBootProbeRelocation=PianoRamBootProbeEntry;
  puts("Returning EFI probe actual C: volatile-only record/CRC, no overwrite, loaded image/console checks, warnings and relocation validation passed (14 cases).");return 0;
}
