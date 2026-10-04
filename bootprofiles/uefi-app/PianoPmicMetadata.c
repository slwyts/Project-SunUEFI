// SPDX-License-Identifier: GPL-2.0-only
// Read-only PMIC metadata. Observer layout: MiCode piano-w-oss spmi-pmic-arb.c.
// Protocol ABI/cache layout verified against captured PmicDxe/UsbConfigDxe.
// This driver provides no PMIC register write or power-control method.
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#define CORE 0x0C400000U
#define CFG 0x0C42D000U
#define OBS 0x0C440000U
typedef struct {UINT32 Model,AllLayer,Metal,SlaveCount;} PM_INFO;
typedef struct {
  UINT64 Revision;
  EFI_STATUS(EFIAPI *GetInfo)(UINT32 Index,PM_INFO *Info);
  EFI_STATUS(EFIAPI *GetPrimary)(UINT32 *Index);
} PM_VERSION;
STATIC EFI_GUID mGuid={0x4684800A,0x2755,0x4EDC,{0xB4,0x43,0x7F,0x8C,0xEB,0x32,0x39,0xD3}};
STATIC PM_INFO mInfo[14];STATIC UINT16 mValid;
STATIC EFI_HANDLE mHandle;
STATIC EFI_STATUS EFIAPI GetInfo(UINT32 Index,PM_INFO *Out) {
  if(Out==NULL)return EFI_INVALID_PARAMETER;
  if(Index>=14 || !(mValid&(1U<<Index)))return EFI_NOT_FOUND;
  CopyMem(Out,&mInfo[Index],sizeof(*Out));return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetPrimary(UINT32 *Index) {
  if(Index==NULL)return EFI_INVALID_PARAMETER;
  if(!(mValid&1))return EFI_NOT_FOUND;
  *Index=0;return EFI_SUCCESS;
}
// Captured native protocol is revision 0x10006 with these two callbacks.
// Do not use the older reference header's additional method without an ABI.
STATIC PM_VERSION mProtocol={0x10006,GetInfo,GetPrimary};
STATIC UINT32 Be32(CONST UINT8 *P) {
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}
STATIC BOOLEAN KnownRam(UINT64 Address,UINT64 Bytes) {
  EFI_MEMORY_REGION_DESCRIPTOR *Map;UINT8 Count;GetMemoryMap(&Map,&Count);
  for(UINT8 I=0;I<Count;++I) {
    if(AsciiStrCmp(Map[I].Name,"Kernel") && AsciiStrCmp(Map[I].Name,"DXE_Heap"))continue;
    if(Address>=Map[I].Address && Address-Map[I].Address<Map[I].Length &&
       Bytes<=Map[I].Length-(Address-Map[I].Address))return TRUE;
  }
  return FALSE;
}
STATIC BOOLEAN Controller(CONST VOID *Fdt) {
  STATIC CONST UINT32 Reg[]={CFG,0x4000,CORE,0x3000,0x0C500000,0x400000,OBS,0x80000,0x0C4C0000,0x10000};
  INT32 N=FdtPathOffset(Fdt,"/soc/qcom,spmi@c42d000"),Len;
  if(N<0)return FALSE;
  CONST UINT8 *P=FdtGetProp(Fdt,N,"reg",&Len);
  if(P==NULL || Len!=sizeof(Reg))return FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(Reg);++I)if(Be32(P+4*I)!=Reg[I])return FALSE;
  P=FdtGetProp(Fdt,N,"qcom,ee",&Len);if(P==NULL || Len!=4 || Be32(P)!=0)return FALSE;
  P=FdtGetProp(Fdt,N,"qcom,bus-id",&Len);return P!=NULL && Len==4 && Be32(P)==0;
}
STATIC EFI_STATUS ReadRevision(UINT8 Sid,UINTN Count,UINT8 Data[7]) {
  if(!(Sid==0 || Sid==2 || Sid==4 || Sid==6 || Sid==7) || Count==0 || Count>1024)return EFI_ACCESS_DENIED;
  UINT16 Ppid=((UINT16)Sid<<8)|1;UINTN Apid=0;BOOLEAN Found=FALSE;
  for(UINTN I=0;I<Count;++I) {
    UINT32 Map=MmioRead32(CORE+0x2000+4*I);
    if(Map==0 || ((Map>>8)&0xFFF)!=Ppid)continue;
    UINT32 Owner=MmioRead32(CFG+4*I)&7;
    if(!Found || Owner==0){Apid=I;Found=TRUE;}
    if(Owner==0)break;
  }
  if(!Found)return EFI_NOT_FOUND;
  UINTN Channel=OBS+0x20*Apid;
  // EXT_READL, fixed address 0x100, exactly seven revision bytes. EE stays 0.
  MmioWrite32(Channel,(1U<<27)|6U);
  for(UINTN Us=0;Us<1000;++Us) {
    UINT32 Status=MmioRead32(Channel+8);
    if(Status&1) {
      if(Status&0xE)return EFI_DEVICE_ERROR;
      UINT32 Lo=MmioRead32(Channel+0x18),Hi=MmioRead32(Channel+0x1c);
      for(UINTN I=0;I<7;++I)Data[I]=(UINT8)((I<4?Lo:Hi)>>((I&3)*8));
      return EFI_SUCCESS;
    }
    gBS->Stall(1);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Decode(CONST UINT8 Data[7],PM_INFO *Info) {
  if(Data[4]!=0x51 || Data[5]==0 || Data[6]>4)return EFI_COMPROMISED_DATA;
  Info->Model=Data[5];Info->AllLayer=Data[3];Info->Metal=Data[2];
  UINT32 Model=Data[5];
  Info->SlaveCount=Data[6]?Data[6]:
    ((Model>=0x35 && Model<=0x54 && ((1U<<(Model-0x35))&0x80000011U))?1:2);
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI PianoPmicMetadataEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  VOID *Existing=NULL;if(!EFI_ERROR(gBS->LocateProtocol(&mGuid,NULL,&Existing)))return EFI_ALREADY_STARTED;
  CONST volatile UINT64 *Record=(CONST volatile UINT64 *)(UINTN)0xA7FFF000;
  if(Record[0]!=0x534E554546494448ULL || !KnownRam(Record[1],40))return EFI_NOT_FOUND;
  CONST VOID *Fdt=(CONST VOID *)(UINTN)Record[1];
  if(FdtCheckHeader(Fdt) || FdtTotalSize(Fdt)>0x200000 || !KnownRam(Record[1],FdtTotalSize(Fdt)) || !Controller(Fdt))return EFI_UNSUPPORTED;
  UINT32 Version=MmioRead32(CORE),Count=MmioRead32(CORE+4)&0x7FF;
  if(Version<0x70000000 || Version>=0x80000000 || Count==0 || Count>1024)return EFI_UNSUPPORTED;
  STATIC CONST UINT8 Sids[]={0,2,4,6,7};
  for(UINTN I=0;I<ARRAY_SIZE(Sids);++I) {
    UINT8 Data[7]={0};UINT8 Sid=Sids[I];EFI_STATUS Status=ReadRevision(Sid,Count,Data);
    if(!EFI_ERROR(Status))Status=Decode(Data,&mInfo[Sid]);
    if(!EFI_ERROR(Status))mValid|=(UINT16)(1U<<Sid);
    DEBUG((DEBUG_WARN,"SUNUEFI_PMIC_METADATA sid=%u status=%r model=%02x major=%u minor=%u slaves=%u readonly=1\n",
      Sid,Status,mInfo[Sid].Model,mInfo[Sid].AllLayer,mInfo[Sid].Metal,mInfo[Sid].SlaveCount));
  }
  // Both the real primary PMIC and USB PMIC must be readable. No placeholder.
  if((mValid&0x81)!=0x81)return EFI_NOT_READY;
  EFI_STATUS Status=gBS->InstallMultipleProtocolInterfaces(&mHandle,&mGuid,&mProtocol,NULL);
  DEBUG((DEBUG_WARN,"SUNUEFI_PMIC_METADATA_PROTOCOL %r revision=10006 valid_mask=%x\n",Status,mValid));
  return Status;
}
