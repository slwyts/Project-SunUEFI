// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded SPI identity probe, not a touchscreen or flash updater.
// Native SPI v1 ABI checked against the original piano PE thunks at
// 0x26E8 (Open), 0x279C (Transfer), 0x272C (Close). No This parameter.
// Novatek page/read framing also checked against nt36532_touch.ko.
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/EFIClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/UefiLib.h>

typedef UINT32 (EFIAPI *SPI_OPEN)(UINT32 Instance,VOID **Handle);
typedef UINT32 (EFIAPI *SPI_TRANSFER)(VOID *Handle,CONST UINT32 *Config,
  CONST UINT8 *Tx,UINT32 TxBytes,UINT8 *Rx,UINT32 RxBytes);
typedef UINT32 (EFIAPI *SPI_CLOSE)(VOID *Handle);
typedef struct {UINT64 Revision;SPI_OPEN Open;SPI_TRANSFER Transfer;SPI_CLOSE Close;} PIANO_SPI;
STATIC EFI_GUID mSpiGuid={0x4C7FFD28,0x6A06,0x4425,{0x9E,0xE2,0x67,0x6E,0xBC,0x08,0x96,0x83}};
STATIC BOOLEAN mPrepared;
STATIC EFI_CLOCK_PROTOCOL *mClock;
STATIC UINTN mClockIds[5],mClocksHeld;
#define TOUCH_SE_BASE 0xA88000U

STATIC VOID ReleaseFifo(VOID) {
  while(mClocksHeld!=0) {
    EFI_STATUS Status=mClock->DisableClock(mClock,mClockIds[--mClocksHeld]);
    if(EFI_ERROR(Status))DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_CLOCK_RELEASE %r\n",Status));
  }
  mClock=NULL;
}

STATIC EFI_STATUS ScmReadCall(UINT32 Function,UINTN Scalar,UINT32 *Value) {
  if(Value==NULL || !((Function==0xC2000601 && Scalar==0x02000501) ||
     (Function==0xC2000501 && Scalar==TOUCH_SE_BASE+0x2008)))return EFI_ACCESS_DENIED;
  // Same atomic ARM64 SIP convention as qcom_scm_io_readl: all parameters
  // are scalar values, no shared-memory pointer or extended argument buffer.
  UINTN Call=Function,Token=0;
  for(UINTN Retry=0;Retry<8;++Retry) {
    ARM_SMC_ARGS Args={0};Args.Arg0=Call;Args.Arg1=1;Args.Arg2=Scalar;Args.Arg6=Token;
    DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SCM_CALL function=0x%x attempt=%u\n",Function,(UINT32)Retry));
    ArmCallSmc(&Args);
    DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SCM_RESULT status=0x%lx value=0x%lx\n",Args.Arg0,Args.Arg1));
    if(Args.Arg0==1) {Call=1;Token=Args.Arg6;continue;}
    if(Args.Arg0!=0)return EFI_UNSUPPORTED;
    *Value=(UINT32)Args.Arg1;return EFI_SUCCESS;
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS ReadFifoViaScm(UINT32 *Value) {
  UINT32 Available=0;
  EFI_STATUS Status=ScmReadCall(0xC2000601,0x02000501,&Available);
  if(EFI_ERROR(Status) || Available!=1)return EFI_UNSUPPORTED;
  return ScmReadCall(0xC2000501,TOUCH_SE_BASE+0x2008,Value);
}

STATIC EFI_STATUS EnableTouchClocks(VOID) {
  STATIC EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;
  STATIC CONST CHAR8 *Names[]={"gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_core_2x_clk",
    "gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk","gcc_qupv3_wrap1_s2_clk"};
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&mClock);
  if(EFI_ERROR(Status) || mClock==NULL)return EFI_NOT_FOUND;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    Status=mClock->GetClockID(mClock,Names[I],&mClockIds[I]);
    if(!EFI_ERROR(Status))Status=mClock->EnableClock(mClock,mClockIds[I]);
    DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_CLOCK %a %r\n",Names[I],Status));
    if(EFI_ERROR(Status)) {ReleaseFifo();return Status;}
    ++mClocksHeld;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS SelectFifo(VOID) {
  EFI_STATUS Status=EnableTouchClocks();
  if(EFI_ERROR(Status))return Status;
  UINT32 Active=MmioRead32(TOUCH_SE_BASE+0x40),Revision=MmioRead32(TOUCH_SE_BASE+0x68);
  UINT32 Interface=MmioRead32(TOUCH_SE_BASE+0x64);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_ENGINE active=0x%x revision=0x%x if_disable=0x%x\n",Active,Revision,Interface));
  UINT32 Protocol=(Revision>>8)&0xFF;
  if((Active&0x1001)!=0 || (Protocol!=0 && Protocol!=1)) {ReleaseFifo();return EFI_ACCESS_DENIED;}
  // Test 26 proved direct access to +0x2008 aborts before a write. Query the
  // standard IO-read service only. Never repeat the direct MMIO access or
  // introduce an IO-write call while this control window is unverified.
  UINT32 Control=0;Status=ReadFifoViaScm(&Control);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_FIFO_SCM_READ %r value=0x%x\n",Status,Control));
  if(EFI_ERROR(Status) || (Interface&1)!=0) {ReleaseFifo();return EFI_UNSUPPORTED;}
  return EFI_SUCCESS;
}

#ifdef PIANO_GPI_PROBE
STATIC VOID ProbeGpiLibrary(VOID) {
  STATIC EFI_GUID Guid={0x569EA0DE,0xB557,0x4043,{0x84,0xCF,0x01,0x10,0x3F,0xE5,0x16,0xE5}};
  EFI_STATUS Status=EnableTouchClocks();
  DEBUG((DEBUG_WARN,"SUNUEFI_GPI_CLOCK_SETUP %r\n",Status));
  if(EFI_ERROR(Status))return;
  VOID *Source=NULL;UINTN Bytes=0;EFI_HANDLE Image=NULL;
  Status=GetSectionFromAnyFv(&Guid,EFI_SECTION_PE32,0,&Source,&Bytes);
  if(!EFI_ERROR(Status)) {
    Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Source,Bytes,&Image);FreePool(Source);
    if(!EFI_ERROR(Status)) {
      DEBUG((DEBUG_WARN,"SUNUEFI_GPI_DRIVER_START\n"));
      Status=gBS->StartImage(Image,NULL,NULL);
      DEBUG((DEBUG_WARN,"SUNUEFI_GPI_DRIVER_RESULT %r\n",Status));
    }
  }
  if(!EFI_ERROR(Status) && Image!=NULL) {
    EFI_LOADED_IMAGE_PROTOCOL *Loaded;VOID *Interface=NULL;
    Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
    if(!EFI_ERROR(Status) && Loaded->ImageSize>=0xD649 &&
       !EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,&Interface)) &&
       (UINTN)Interface==(UINTN)Loaded->ImageBase+0xD148) {
      UINT64 *Words=Interface;
      STATIC CONST UINTN Functions[]={0x1528,0x1540,0x1558,0x1570,0x1588,0x15A0,0x15B8,0x15D0};
      BOOLEAN Match=Words[0]==0x10002;
      for(UINTN I=0;I<ARRAY_SIZE(Functions);++I)if(Words[I+1]!=(UINTN)Loaded->ImageBase+Functions[I])Match=FALSE;
      if(Match) {
        // Register-interface routine checks this software flag before it
        // accepts an interface. Read it, but do not call unknown DMA APIs.
        UINT8 Ready=*((CONST volatile UINT8 *)Loaded->ImageBase+0xD648);
        DEBUG((DEBUG_WARN,"SUNUEFI_GPI_PROTOCOL revision=0x%lx software_ready=%u dma_channels_registered=0\n",Words[0],Ready));
      } else DEBUG((DEBUG_WARN,"SUNUEFI_GPI_INTERFACE_LAYOUT_MISMATCH\n"));
    }
  }
  ReleaseFifo();
}
#endif

STATIC UINT32 Be32(CONST UINT8 *P) {
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}
STATIC VOID PutBe32(UINT8 *P,UINT32 Value) {
  P[0]=(UINT8)(Value>>24);P[1]=(UINT8)(Value>>16);P[2]=(UINT8)(Value>>8);P[3]=(UINT8)Value;
}
STATIC BOOLEAN Cell(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,UINT32 Value) {
  INT32 Len;CONST UINT8 *P=FdtGetProp(Fdt,Node,Name,&Len);
  return P!=NULL && Len==4 && Be32(P)==Value;
}
STATIC BOOLEAN ValidatePins(CONST VOID *Fdt,INT32 Node,UINT32 Flags,UINT8 **Data) {
  INT32 Len;CONST UINT8 *P=FdtGetProp(Fdt,Node,"config",&Len);
  if(P==NULL || Len!=32)return FALSE;
  for(UINTN I=0;I<4;++I)if(Be32(P+8*I)!=30+I || Be32(P+8*I+4)!=Flags)return FALSE;
  *Data=(UINT8 *)(UINTN)P;return TRUE;
}

EFI_STATUS PianoPrepareTouch(VOID) {
  mPrepared=FALSE;
  EFI_MEMORY_REGION_DESCRIPTOR *Map;UINT8 Count;VOID *Found=NULL;UINTN Matches=0;
  GetMemoryMap(&Map,&Count);
  for(UINTN I=0;I<Count;++I) {
    if(AsciiStrCmp(Map[I].Name,"XBL_DT")!=0)continue;
    if(Map[I].Address!=0x81A00000 || Map[I].Length!=0x40000)return EFI_UNSUPPORTED;
    for(UINTN Offset=0;Offset+40<=Map[I].Length;Offset+=4) {
      UINT8 *P=(UINT8 *)(UINTN)(Map[I].Address+Offset);
      if(Be32(P)!=0xD00DFEED)continue;
      UINTN Total=Be32(P+4),Struct=Be32(P+8),Strings=Be32(P+12);
      UINTN StringsSize=Be32(P+32),StructSize=Be32(P+36);
      if(Total<40 || Total>Map[I].Length-Offset || Struct>Total || Strings>Total ||
        StructSize>Total-Struct || StringsSize>Total-Strings || FdtCheckHeader(P))continue;
      INT32 Controller=FdtPathOffset(P,"/soc/TOP_QUP_1");
      INT32 Se=FdtPathOffset(P,"/soc/TOP_QUP_1/TOP_QUP_1_SE_2");
      if(Controller<0 || Se<0)continue;
      if(!Cell(P,Controller,"se_wrapper_base_addr",0xA80000) ||
         !Cell(P,Se,"core_offset",0x8000))continue;
      Found=P;++Matches;
    }
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_CONFIG_SCAN matches=%u\n",(UINT32)Matches));
  if(Matches!=1)return EFI_NOT_FOUND;
  INT32 Se=FdtPathOffset(Found,"/soc/TOP_QUP_1/TOP_QUP_1_SE_2"),Len;
  CONST UINT8 *Status=FdtGetProp(Found,Se,"status",&Len);
  if(Status==NULL || Len!=9 || CompareMem(Status,"disabled",9))return EFI_COMPROMISED_DATA;
  CONST UINT8 *Refs=FdtGetProp(Found,Se,"pinctrl-2",&Len);
  if(Refs==NULL || Len!=4)return EFI_COMPROMISED_DATA;
  INT32 Active=FdtNodeOffsetByPhandle(Found,Be32(Refs));
  Refs=FdtGetProp(Found,Se,"pinctrl-3",&Len);
  if(Refs==NULL || Len!=4)return EFI_COMPROMISED_DATA;
  INT32 Sleep=FdtNodeOffsetByPhandle(Found,Be32(Refs));
  UINT8 *ActiveData,*SleepData;
  if(Active<0 || Sleep<0 || !ValidatePins(Found,Active,0x2580011,&ActiveData) ||
     !ValidatePins(Found,Sleep,0xC80021,&SleepData))return EFI_COMPROMISED_DATA;
  // In-place edits of the known RAM DT only. Lengths and all other cells stay
  // fixed; both active and sleep paths must use the actual GPIO40..43 pins.
  for(UINTN I=0;I<4;++I) {PutBe32(ActiveData+8*I,40+(UINT32)I);PutBe32(SleepData+8*I,40+(UINT32)I);}
  SetMem((VOID *)(UINTN)Status,9,0);CopyMem((VOID *)(UINTN)Status,"okay",5);
  WriteBackDataCacheRange(Found,FdtTotalSize(Found));
  mPrepared=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_CONFIG_RAM_ONLY dt=0x%lx se=0xA88000 pins=40,41,42,43\n",(UINTN)Found));
  return EFI_SUCCESS;
}

STATIC BOOLEAN VerifiedInterface(PIANO_SPI *Spi) {
  EFI_HANDLE *Handles=NULL;UINTN Count=0;BOOLEAN Valid=FALSE;
  if(Spi==NULL || Spi->Revision!=0x10000)return FALSE;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status))return FALSE;
  for(UINTN I=0;I<Count;++I) {
    EFI_LOADED_IMAGE_PROTOCOL *Loaded;
    Status=gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
    if(EFI_ERROR(Status) || Loaded->ImageSize<0xC108)continue;
    UINTN Base=(UINTN)Loaded->ImageBase;
    if((UINTN)Spi==Base+0xC0E8 && (UINTN)Spi->Open==Base+0x26E8 &&
       (UINTN)Spi->Transfer==Base+0x279C && (UINTN)Spi->Close==Base+0x272C)Valid=TRUE;
  }
  FreePool(Handles);return Valid;
}

STATIC UINT32 Transfer(PIANO_SPI *Spi,VOID *Handle,CONST UINT8 *Tx,UINTN TxBytes,UINT8 *Rx,UINTN RxBytes) {
  UINT32 Config[16]={0};
  // Original v1 thunk reads frequency at +0x14 and word length at +0x18.
  // CPOL/CPHA, slave index, loopback and delay fields are zero for mode 0.
  Config[5]=1000000;Config[6]=8;
  if(Tx==NULL || TxBytes==0 || TxBytes>258 || RxBytes>258 || (RxBytes!=0 && Rx==NULL))return 2;
  return Spi->Transfer(Handle,Config,Tx,(UINT32)TxBytes,Rx,(UINT32)RxBytes);
}

STATIC EFI_STATUS ReadIdentityByteRange(PIANO_SPI *Spi,VOID *Handle,UINT32 Address,UINT8 *Out,UINTN Bytes) {
  if(!((Address==0x1FB104 && Bytes==6) || (Address==0x11C478 && Bytes==24)))return EFI_ACCESS_DENIED;
  UINT8 Page[3]={0xFF,(UINT8)(Address>>15),(UINT8)(Address>>7)};
  UINT32 Result=Transfer(Spi,Handle,Page,sizeof(Page),NULL,0);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_PAGE addr=0x%x result=%u\n",Address,Result));
  if(Result!=0)return EFI_DEVICE_ERROR;
  UINT8 Tx[26]={0},Rx[26]={0};Tx[0]=(UINT8)(Address&0x7F);
  Result=Transfer(Spi,Handle,Tx,Bytes+2,Rx,Bytes+2);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_READ addr=0x%x result=%u\n",Address,Result));
  if(Result!=0)return EFI_DEVICE_ERROR;
  CopyMem(Out,Rx+2,Bytes);return EFI_SUCCESS;
}

VOID PianoProbeTouch(VOID) {
  if(!mPrepared)return;
#ifdef PIANO_GPI_PROBE
  ProbeGpiLibrary();return;
#endif
  PIANO_SPI *Spi=NULL;VOID *Handle=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&mSpiGuid,NULL,(VOID **)&Spi);
  if(EFI_ERROR(Status) || !VerifiedInterface(Spi)) {
    DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SPI_INTERFACE_UNAVAILABLE %r\n",Status));return;
  }
  Status=SelectFifo();
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_FIFO_SELECT %r\n",Status));
  if(EFI_ERROR(Status))return;
  // The checked enum mapper counts TOP_QUP_0's 10 engines before TOP_QUP_1;
  // instance 13 therefore selects QUP1 / SE2. Never probe other bus instances.
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SPI_OPEN instance=13\n"));
  UINT32 Result=Spi->Open(13,&Handle);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SPI_OPEN_RESULT code=%u handle=0x%lx\n",Result,(UINTN)Handle));
  if(Result!=0 || Handle==NULL) {ReleaseFifo();return;}
  UINT8 Id[6]={0};Status=ReadIdentityByteRange(Spi,Handle,0x1FB104,Id,6);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_ID status=%r id=%02x%02x%02x%02x%02x%02x\n",Status,Id[0],Id[1],Id[2],Id[3],Id[4],Id[5]));
  STATIC CONST UINT8 Expected[6]={0x0E,0,4,0x32,0x65,3};
  if(!EFI_ERROR(Status) && CompareMem(Id,Expected,6)==0) {
    UINT8 Info[24]={0};Status=ReadIdentityByteRange(Spi,Handle,0x11C478,Info,sizeof(Info));
    DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_FW_INFO status=%r version=0x%x complement=0x%x x=%u y=%u\n",Status,Info[0],Info[1],((UINT32)Info[4]<<8)|Info[5],((UINT32)Info[6]<<8)|Info[7]));
  }
  Result=Spi->Close(Handle);
  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_SPI_CLOSE code=%u\n",Result));
  ReleaseFifo();
}
