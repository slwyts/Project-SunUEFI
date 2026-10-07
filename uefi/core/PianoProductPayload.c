// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductPayload.h"
#include "PianoFastbootBoot.h"
#ifndef PIANO_PRODUCT_PAYLOAD_DIGEST_HEADER
#define PIANO_PRODUCT_PAYLOAD_DIGEST_HEADER "PianoProductSimpleInitDigest.h"
#endif
#include PIANO_PRODUCT_PAYLOAD_DIGEST_HEADER
#include <Library/MemoryMapLib.h>
#include <Library/FdtLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
#pragma pack(1)
typedef struct {UINT8 Magic[16];UINT32 Version,HeaderBytes;UINT64 AppBytes;UINT8 AppHash[32];} APP_HEADER;
typedef struct {UINT8 Magic[16];UINT32 Version,HeaderBytes;UINT64 AppStart,AppSpan,ImageBase,ImageBytes,BootTarget;} EMBEDDED_HEADER;
#pragma pack()
STATIC CONST UINT8 mMagic[16]="SUNUEFI-APPv1";
STATIC UINTN mGeneration;
STATIC BOOLEAN mLoaned;
STATIC PIANO_PRODUCT_PAYLOAD_VIEW mView;
STATIC CONST VOID *mFdt;
STATIC UINT64 mInitrdStart,mInitrdEnd;
STATIC UINT32 mBootTarget;
#ifdef PIANO_PRODUCT_PAYLOAD_HOST_TEST
extern UINTN PianoProductHostHandoffAddress;
#define HANDOFF_ADDRESS PianoProductHostHandoffAddress
#else
#define HANDOFF_ADDRESS 0xA7FFF000U
#endif
STATIC BOOLEAN Known(UINT64 Address,UINT64 Bytes,BOOLEAN Handoff) {
  EFI_MEMORY_REGION_DESCRIPTOR *Map=NULL;UINT8 Count=0;GetMemoryMap(&Map,&Count);
  if(Map==NULL || !Bytes || Address>MAX_UINT64-Bytes)return FALSE;
  for(UINTN I=0;I<Count;++I) {
    BOOLEAN Name=Handoff?!AsciiStrCmp(Map[I].Name,"BootHandoff"):
      (!AsciiStrCmp(Map[I].Name,"Kernel") || !AsciiStrCmp(Map[I].Name,"DXE_Heap") ||
       !AsciiStrCmp(Map[I].Name,"DXE_Heap_Upper"));
    if(Name && Address>=Map[I].Address && Address-Map[I].Address<Map[I].Length &&
       Bytes<=Map[I].Length-(Address-Map[I].Address))return TRUE;
  }
  return FALSE;
}
STATIC UINT64 Prop(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name) {
  INT32 Length=0;CONST UINT8 *P=FdtGetProp(Fdt,Node,Name,&Length);UINT64 Value=0;
  if(P==NULL || (Length!=4 && Length!=8))return 0;
  for(INT32 I=0;I<Length;++I)Value=(Value<<8)|P[I];
  return Value;
}
STATIC BOOLEAN HeaderMatches(CONST APP_HEADER *Header,UINT64 Available) {
  return Available>=sizeof(*Header) && !CompareMem(Header->Magic,mMagic,sizeof(mMagic)) &&
    Header->Version==1 && Header->HeaderBytes==sizeof(*Header) &&
    Header->AppBytes==PIANO_PRODUCT_SIMPLEINIT_BYTES && Header->AppBytes>=4096 &&
    Header->AppBytes<=0x4000000 && Header->AppBytes<=Available-sizeof(*Header) &&
    !CompareMem(Header->AppHash,mPianoProductSimpleInitSha256,32);
}
STATIC EFI_STATUS Embedded(PIANO_PRODUCT_PAYLOAD_VIEW *View,UINT64 *Start,UINT64 *End,UINT32 *Target) {
  UINT64 Address=HANDOFF_ADDRESS+0x100U;
  if(!Known(Address,sizeof(EMBEDDED_HEADER),TRUE))return EFI_NOT_FOUND;
  EMBEDDED_HEADER Record;CopyMem(&Record,(CONST VOID *)(UINTN)Address,sizeof(Record));
  STATIC CONST UINT8 Magic[16]="SUNUEFI-EMBEDv1";
  if(CompareMem(Record.Magic,Magic,sizeof(Magic)))return EFI_NOT_FOUND;
  // The handoff record only selects a candidate. It cannot extend the fixed
  // memory map or authorize arbitrary EFI code: the compiled app pin wins.
  if((Record.Version!=1 && Record.Version!=2) || Record.HeaderBytes!=sizeof(Record) ||
     (Record.Version==1 ? Record.BootTarget!=0 : Record.BootTarget<1 || Record.BootTarget>3) ||
     !Known(Record.ImageBase,Record.ImageBytes,FALSE) || Record.AppStart<Record.ImageBase ||
     Record.AppSpan!=sizeof(APP_HEADER)+(UINT64)PIANO_PRODUCT_SIMPLEINIT_BYTES ||
     Record.AppSpan>Record.ImageBytes || Record.AppStart-Record.ImageBase>Record.ImageBytes-Record.AppSpan ||
     !Known(Record.AppStart,Record.AppSpan,FALSE))return EFI_SECURITY_VIOLATION;
  APP_HEADER Header;CopyMem(&Header,(CONST VOID *)(UINTN)Record.AppStart,sizeof(Header));
  if(!HeaderMatches(&Header,Record.AppSpan))return EFI_SECURITY_VIOLATION;
  CONST UINT8 *Image=(CONST UINT8 *)(UINTN)Record.AppStart+sizeof(Header);UINT8 Hash[32];
  if(!Sha256HashAll(Image,(UINTN)Header.AppBytes,Hash) ||
     CompareMem(Hash,mPianoProductSimpleInitSha256,32))return EFI_SECURITY_VIOLATION;
  View->Image=Image;View->Bytes=(UINTN)PIANO_PRODUCT_SIMPLEINIT_BYTES;CopyMem(View->Sha256,Hash,sizeof(Hash));
  *Start=Record.AppStart;*End=Record.AppStart+Record.AppSpan;
  *Target=(UINT32)Record.BootTarget;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_PAYLOAD_EMBEDDED app=%lx bytes=%lu container=%lx..%lx\n",
    (UINT64)(UINTN)Image,Header.AppBytes,Record.ImageBase,Record.ImageBase+Record.ImageBytes));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Reader(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  CONST PIANO_PRODUCT_PAYLOAD_VIEW *V=Context;
  if(Buffer==NULL || Offset>V->Bytes || Bytes>V->Bytes-Offset)return EFI_BAD_BUFFER_SIZE;
  CopyMem(Buffer,(CONST UINT8 *)V->Image+(UINTN)Offset,Bytes);return EFI_SUCCESS;
}
STATIC EFI_STATUS Resolve(PIANO_PRODUCT_PAYLOAD_VIEW *View,CONST VOID **Dtb,UINT64 *Start,UINT64 *End,UINT32 *Target) {
  *Target=0;
  if(!Known(HANDOFF_ADDRESS,16,TRUE))return EFI_ACCESS_DENIED;
  CONST volatile UINT64 *Record=(CONST volatile UINT64 *)(UINTN)HANDOFF_ADDRESS;
  UINT64 Magic=Record[0],Address=Record[1];
  if(Magic!=0x534E554546494448ULL || !Known(Address,40,FALSE))return EFI_NOT_FOUND;
  CONST VOID *Fdt=(CONST VOID *)(UINTN)Address;
  if(FdtCheckHeader(Fdt)!=0)return EFI_COMPROMISED_DATA;
  UINT32 DtbBytes=FdtTotalSize(Fdt);
  if(DtbBytes<40 || DtbBytes>0x200000 || !Known(Address,DtbBytes,FALSE))return EFI_COMPROMISED_DATA;
  EFI_STATUS EmbeddedStatus=Embedded(View,Start,End,Target);
  if(EmbeddedStatus!=EFI_NOT_FOUND) {
    if(EmbeddedStatus!=EFI_SUCCESS)return EmbeddedStatus;
    PIANO_BOOT_SOURCE Source={View,Reader,View->Bytes};PIANO_BOOT_IMAGE Parsed;
    EFI_STATUS Status=PianoFastbootBootParse(&Source,&Parsed);
    if(Status!=EFI_SUCCESS || Parsed.Kind!=PianoBootArm64Pe || Parsed.Pe.Machine!=0xaa64 || Parsed.Pe.Subsystem!=10)
      return Status==EFI_SUCCESS?EFI_UNSUPPORTED:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
    *Dtb=Fdt;return EFI_SUCCESS;
  }
  INT32 Chosen=FdtPathOffset(Fdt,"/chosen");if(Chosen<0)return EFI_NOT_FOUND;
  *Start=Prop(Fdt,Chosen,"linux,initrd-start");*End=Prop(Fdt,Chosen,"linux,initrd-end");
  if(*End<=*Start || *End-*Start>0x10000000 || !Known(*Start,*End-*Start,FALSE) || *End-*Start<sizeof(APP_HEADER))return EFI_BAD_BUFFER_SIZE;
  // Android v3/v4 ABL concatenates selected vendor ramdisks before the generic
  // ramdisk and may append bootconfig. The FDT span is the combined initrd,
  // not the boot.img ramdisk's start. Search ONLY this fully validated mapped
  // input span; a marker never grants authority without the compiled pin.
  UINT64 Span=*End-*Start,Found=0;UINTN Matches=0;APP_HEADER Header;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_PAYLOAD_HANDOFF dtb=%lx initrd=%lx..%lx bytes=%lu expected_app=%lu\n",
    Address,*Start,*End,Span,(UINT64)PIANO_PRODUCT_SIMPLEINIT_BYTES));
  CONST UINT8 *Input=(CONST UINT8 *)(UINTN)*Start;
  for(UINT64 Offset=0;Offset<=Span-sizeof(Header);++Offset) {
    if(Input[Offset]!=mMagic[0] || CompareMem(Input+(UINTN)Offset,mMagic,sizeof(mMagic)))continue;
    CopyMem(&Header,Input+(UINTN)Offset,sizeof(Header));
    if(!HeaderMatches(&Header,Span-Offset))continue;
    UINT8 Hash[32];CONST VOID *Image=Input+(UINTN)Offset+sizeof(Header);
    if(!Sha256HashAll(Image,(UINTN)Header.AppBytes,Hash) || CompareMem(Hash,mPianoProductSimpleInitSha256,32))continue;
    if(++Matches!=1) {
      DEBUG((DEBUG_ERROR,"PIANO_PRODUCT_PAYLOAD_REJECT duplicate_pinned_app=1\n"));
      return EFI_SECURITY_VIOLATION;
    }
    Found=Offset;CopyMem(View->Sha256,Hash,sizeof(Hash));
  }
  if(Matches!=1) {
    APP_HEADER First;CopyMem(&First,Input,sizeof(First));
    DEBUG((DEBUG_ERROR,"PIANO_PRODUCT_PAYLOAD_REJECT pinned_app_not_found=1 head=%02x%02x%02x%02x version=%u header=%u app_bytes=%lu\n",
      First.Magic[0],First.Magic[1],First.Magic[2],First.Magic[3],First.Version,First.HeaderBytes,First.AppBytes));
    return EFI_SECURITY_VIOLATION;
  }
  View->Image=Input+(UINTN)Found+sizeof(Header);View->Bytes=(UINTN)PIANO_PRODUCT_SIMPLEINIT_BYTES;
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_PAYLOAD_PINNED offset=%lu app=%lx bytes=%lu\n",
    Found,(UINT64)(UINTN)View->Image,(UINT64)View->Bytes));
  PIANO_BOOT_SOURCE Source={View,Reader,View->Bytes};PIANO_BOOT_IMAGE Parsed;
  EFI_STATUS Status=PianoFastbootBootParse(&Source,&Parsed);
  if(Status!=EFI_SUCCESS || Parsed.Kind!=PianoBootArm64Pe || Parsed.Pe.Machine!=0xaa64 || Parsed.Pe.Subsystem!=10)
    return Status==EFI_SUCCESS?EFI_UNSUPPORTED:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
  *Dtb=Fdt;return EFI_SUCCESS;
}
EFI_STATUS PianoProductAcquireSimpleInit(PIANO_PRODUCT_PAYLOAD_VIEW *View) {
  if(View==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(View,sizeof(*View));
  if(mLoaned)return EFI_ALREADY_STARTED;
  mBootTarget=0;
  PIANO_PRODUCT_PAYLOAD_VIEW Candidate={0};CONST VOID *Fdt=NULL;UINT64 Start=0,End=0;
  UINT32 Target=0;
  EFI_STATUS Status=Resolve(&Candidate,&Fdt,&Start,&End,&Target);if(Status!=EFI_SUCCESS)return Status;
  if(mGeneration==MAX_UINTN)return EFI_OUT_OF_RESOURCES;
  Candidate.Lease=(VOID *)++mGeneration;mView=Candidate;mFdt=Fdt;mInitrdStart=Start;mInitrdEnd=End;mLoaned=TRUE;
  mBootTarget=Target;
  *View=mView;return EFI_SUCCESS;
}
EFI_STATUS PianoProductValidateSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View) {
  if(View==NULL || !mLoaned || View->Lease==NULL || View->Lease!=mView.Lease || View->Image!=mView.Image ||
     View->Bytes!=mView.Bytes || CompareMem(View->Sha256,mView.Sha256,32))return EFI_ACCESS_DENIED;
  PIANO_PRODUCT_PAYLOAD_VIEW Fresh={0};CONST VOID *Fdt=NULL;UINT64 Start=0,End=0;
  UINT32 Target=0;
  EFI_STATUS Status=Resolve(&Fresh,&Fdt,&Start,&End,&Target);if(Status!=EFI_SUCCESS)return Status;
  return Fdt==mFdt && Start==mInitrdStart && End==mInitrdEnd && Fresh.Image==View->Image && Fresh.Bytes==View->Bytes &&
    Target==mBootTarget && !CompareMem(Fresh.Sha256,View->Sha256,32)?EFI_SUCCESS:EFI_MEDIA_CHANGED;
}
EFI_STATUS PianoProductReleaseSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View) {
  if(View==NULL || !mLoaned || View->Lease!=mView.Lease || View->Image!=mView.Image || View->Bytes!=mView.Bytes ||
     CompareMem(View->Sha256,mView.Sha256,32))return EFI_ACCESS_DENIED;
  // The BootShim Kernel reservation remains owned by the parent product image;
  // releasing a loan never frees/zeroes a source still used by another loader.
  mLoaned=FALSE;ZeroMem(&mView,sizeof(mView));mFdt=NULL;mInitrdStart=mInitrdEnd=0;return EFI_SUCCESS;
}
EFI_STATUS PianoProductPayloadGetFdt(CONST PIANO_PRODUCT_PAYLOAD_VIEW *View,CONST VOID **Fdt) {
  if(Fdt==NULL)return EFI_INVALID_PARAMETER;
  *Fdt=NULL;
  EFI_STATUS Status=PianoProductValidateSimpleInit(View);
  if(Status==EFI_SUCCESS)*Fdt=mFdt;
  return Status;
}
UINT32 PianoProductPayloadBootTarget(VOID) {return mBootTarget;}
