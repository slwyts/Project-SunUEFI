// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the real RAM DT edits and SPI framing on a host with mocked hardware.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef NULL
#include "../../uefi/core/PianoTouchProbe.c"

extern int fdt_check_header(const void *);
extern int fdt_path_offset(const void *,const char *);
extern const void *fdt_getprop(const void *,int,const char *,int *);
extern int fdt_node_offset_by_phandle(const void *,unsigned int);

EFI_BOOT_SERVICES *gBS;
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_CLOCK_PROTOCOL Clock;
STATIC UINT32 EngineActive,EngineRevision=0x111,Control=0x85;
STATIC BOOLEAN MirrorStuck;
STATIC UINTN ControlWrites,Enables,Disables;
STATIC UINTN SmcCalls,IoReads;
STATIC UINT32 ReadSupport=1;
STATIC UINTN ScmError,Interrupts;
STATIC UINT32 LastFunction;
STATIC INTN ClockFailAt=-1;
STATIC EFI_STATUS EFIAPI GetClockMock(EFI_CLOCK_PROTOCOL *This,CONST CHAR8 *Name,UINTN *Id) {
  STATIC CONST CHAR8 *Names[]={"gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_core_2x_clk",
    "gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk","gcc_qupv3_wrap1_s2_clk"};
  assert(This==&Clock);
  for(UINTN I=0;I<5;++I)if(strcmp(Name,Names[I])==0) {*Id=I;return EFI_SUCCESS;}
  assert(!"Unknown clock requested");return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI EnableClockMock(EFI_CLOCK_PROTOCOL *This,UINTN Id) {
  assert(This==&Clock && Id<5);
  if((INTN)Id==ClockFailAt)return EFI_DEVICE_ERROR;
  ++Enables;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI DisableClockMock(EFI_CLOCK_PROTOCOL *This,UINTN Id) {
  assert(This==&Clock && Id<5);++Disables;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI LocateMock(EFI_GUID *Guid,VOID *Registration,VOID **Interface) {
  STATIC EFI_GUID Expected=EFI_CLOCK_PROTOCOL_GUID;
  assert(memcmp(Guid,&Expected,sizeof(Expected))==0);*Interface=&Clock;return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address==TOUCH_SE_BASE+0x40)return EngineActive;
  if(Address==TOUCH_SE_BASE+0x68)return EngineRevision;
  if(Address==TOUCH_SE_BASE+0x64)return MirrorStuck?1:(Control&1);
  assert(!"Unapproved MMIO read");return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(!"This diagnostic must never write MMIO");return Value;
}
VOID EFIAPI ArmCallSmc(ARM_SMC_ARGS *Args) {
  assert(Args->Arg1==1 && Args->Arg3==0 && Args->Arg4==0 && Args->Arg5==0 && Args->Arg7==0);
  assert(Args->Arg8==0 && Args->Arg9==0 && Args->Arg10==0 && Args->Arg11==0);
  assert(Args->Arg12==0 && Args->Arg13==0 && Args->Arg14==0 && Args->Arg15==0 && Args->Arg16==0 && Args->Arg17==0);
  ++SmcCalls;
  if(Args->Arg0!=1)LastFunction=(UINT32)Args->Arg0;
  else assert(Args->Arg6==0x1234);
  assert(LastFunction==0xC2000601 || LastFunction==0xC2000501);
  assert(Args->Arg2==(LastFunction==0xC2000601?0x02000501:TOUCH_SE_BASE+0x2008));
  if(Interrupts) {--Interrupts;Args->Arg0=1;Args->Arg6=0x1234;return;}
  Args->Arg0=ScmError;
  Args->Arg1=LastFunction==0xC2000601?ReadSupport:Control;
  if(LastFunction==0xC2000501)++IoReads;
}
STATIC EFI_MEMORY_REGION_DESCRIPTOR Region={.Name="XBL_DT",.Address=0x81A00000,.Length=0x40000};
VOID EFIAPI GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **Map,UINT8 *Count) {*Map=&Region;*Count=1;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) {return strcmp(A,B);}
VOID *EFIAPI CopyMem(VOID *To,CONST VOID *From,UINTN Bytes) {return memcpy(To,From,Bytes);}
VOID *EFIAPI SetMem(VOID *To,UINTN Bytes,UINT8 Value) {return memset(To,Value,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes) {return memcmp(A,B,Bytes);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN Bytes) {
  assert((UINTN)P>=Region.Address && Bytes<=Region.Length-((UINTN)P-Region.Address));return P;
}
INT32 EFIAPI FdtCheckHeader(CONST VOID *P) {return fdt_check_header(P);}
INT32 EFIAPI FdtPathOffset(CONST VOID *P,CONST CHAR8 *Path) {return fdt_path_offset(P,Path);}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *P,INT32 Node,CONST CHAR8 *Name,INT32 *Len) {return fdt_getprop(P,Node,Name,Len);}
INT32 EFIAPI FdtNodeOffsetByPhandle(CONST VOID *P,UINT32 Phandle) {return fdt_node_offset_by_phandle(P,Phandle);}
UINT32 EFIAPI Fdt32ToCpu(UINT32 Value) {return __builtin_bswap32(Value);}

STATIC UINTN Transactions;
STATIC UINT32 SpiResult;
STATIC UINT32 EFIAPI SpiMock(VOID *Handle,CONST UINT32 *Config,CONST UINT8 *Tx,UINT32 TxBytes,UINT8 *Rx,UINT32 RxBytes) {
  assert(Handle==(VOID *)1 && Config[5]==1000000 && Config[6]==8);
  for(UINTN I=0;I<16;++I)if(I!=5 && I!=6)assert(Config[I]==0);
  ++Transactions;
  if(TxBytes==3) {assert(Tx[0]==0xFF && Tx[1]==0x3F && Tx[2]==0x62);assert(Rx==NULL && RxBytes==0);}
  else {
    assert(TxBytes==8 && RxBytes==8 && Tx[0]==4);
    for(UINTN I=1;I<8;++I)assert(Tx[I]==0);
    STATIC CONST UINT8 Response[8]={0xAA,0xBB,0x0E,0,4,0x32,0x65,3};CopyMem(Rx,Response,8);
  }
  return SpiResult;
}

int main(int argc,char **argv) {
  Clock.GetClockID=GetClockMock;Clock.EnableClock=EnableClockMock;Clock.DisableClock=DisableClockMock;
  Services.LocateProtocol=LocateMock;gBS=&Services;
  assert(argc==2);FILE *File=fopen(argv[1],"rb");assert(File);
  UINT8 *Area=mmap((VOID *)(UINTN)Region.Address,Region.Length,PROT_READ|PROT_WRITE,
    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Area!=MAP_FAILED);
  UINT8 *Original=malloc(Region.Length);assert(Original);
  size_t Size=fread(Original,1,Region.Length,File);fclose(File);assert(Size>40 && Size<Region.Length-0x1000);
  UINT8 *Fdt=Area+0x1000;CopyMem(Fdt,Original,Size);
  assert(PianoPrepareTouch()==EFI_SUCCESS);
  UINT8 *Expected=malloc(Size);assert(Expected);CopyMem(Expected,Original,Size);
  INT32 Node=FdtPathOffset(Expected,"/soc/TOP_QUP_1/TOP_QUP_1_SE_2"),Len;
  UINT8 *Status=(UINT8 *)(UINTN)FdtGetProp(Expected,Node,"status",&Len);assert(Len==9);
  SetMem(Status,9,0);CopyMem(Status,"okay",5);
  for(int I=2;I<=3;++I) {
    const char *Name=I==2?"pinctrl-2":"pinctrl-3";
    CONST UINT8 *Ref=FdtGetProp(Expected,Node,Name,&Len);assert(Len==4);
    INT32 Pins=FdtNodeOffsetByPhandle(Expected,Be32(Ref));
    UINT8 *Data=(UINT8 *)(UINTN)FdtGetProp(Expected,Pins,"config",&Len);assert(Len==32);
    for(UINTN J=0;J<4;++J)PutBe32(Data+8*J,40+(UINT32)J);
  }
  // The entire blob must equal the explicit expected edit; no unrelated
  // regulator, storage, GPIO or other controller property may change.
  assert(memcmp(Fdt,Expected,Size)==0);
  CopyMem(Fdt,Original,Size);
  Node=FdtPathOffset(Fdt,"/soc/TOP_QUP_1/TOP_QUP_1_SE_2");
  UINT8 *Bad=(UINT8 *)(UINTN)FdtGetProp(Fdt,Node,"core_offset",&Len);PutBe32(Bad,0xC000);
  UINT8 *Before=malloc(Size);assert(Before);CopyMem(Before,Fdt,Size);
  assert(PianoPrepareTouch()==EFI_NOT_FOUND && memcmp(Fdt,Before,Size)==0);
  CopyMem(Fdt,Original,Size);
  CONST UINT8 *Ref=FdtGetProp(Fdt,Node,"pinctrl-3",&Len);
  INT32 Pins=FdtNodeOffsetByPhandle(Fdt,Be32(Ref));Bad=(UINT8 *)(UINTN)FdtGetProp(Fdt,Pins,"config",&Len);
  PutBe32(Bad,29);CopyMem(Before,Fdt,Size);
  assert(PianoPrepareTouch()==EFI_COMPROMISED_DATA && memcmp(Fdt,Before,Size)==0);

  PIANO_SPI Spi={.Revision=0x10000,.Transfer=SpiMock};UINT8 Id[6];
  assert(ReadIdentityByteRange(&Spi,(VOID *)1,0x1FB104,Id,6)==EFI_SUCCESS);
  STATIC CONST UINT8 ExpectedId[6]={0x0E,0,4,0x32,0x65,3};assert(memcmp(Id,ExpectedId,6)==0 && Transactions==2);
  assert(ReadIdentityByteRange(&Spi,(VOID *)1,0x1FB43E,Id,1)==EFI_ACCESS_DENIED && Transactions==2);
  SpiResult=2;assert(ReadIdentityByteRange(&Spi,(VOID *)1,0x1FB104,Id,6)==EFI_DEVICE_ERROR && Transactions==3);
  assert(SelectFifo()==EFI_UNSUPPORTED && Control==0x85 && ControlWrites==0 && Enables==5 && Disables==5 && IoReads==1);
  Control=0x84;assert(SelectFifo()==EFI_SUCCESS && Control==0x84 && ControlWrites==0);ReleaseFifo();assert(Disables==10);
  UINTN BeforeSmc=SmcCalls;
  EngineActive=1;assert(SelectFifo()==EFI_ACCESS_DENIED && ControlWrites==0 && SmcCalls==BeforeSmc);
  EngineActive=0x1000;assert(SelectFifo()==EFI_ACCESS_DENIED && SmcCalls==BeforeSmc);
  EngineActive=0;EngineRevision=0x311;assert(SelectFifo()==EFI_ACCESS_DENIED && SmcCalls==BeforeSmc);
  EngineRevision=0x111;ReadSupport=0;UINTN BeforeReads=IoReads;
  assert(SelectFifo()==EFI_UNSUPPORTED && IoReads==BeforeReads);ReadSupport=1;
  ScmError=(UINTN)-2;assert(SelectFifo()==EFI_UNSUPPORTED && IoReads==BeforeReads);ScmError=0;
  UINT32 ReadValue;Interrupts=2;assert(ReadFifoViaScm(&ReadValue)==EFI_SUCCESS && ReadValue==0x84);
  BeforeSmc=SmcCalls;assert(ScmReadCall(0xC2000502,TOUCH_SE_BASE+0x2008,&ReadValue)==EFI_ACCESS_DENIED && SmcCalls==BeforeSmc);
  assert(ScmReadCall(0xC2000501,0xA80000,&ReadValue)==EFI_ACCESS_DENIED && SmcCalls==BeforeSmc);
  Interrupts=9;assert(ScmReadCall(0xC2000501,TOUCH_SE_BASE+0x2008,&ReadValue)==EFI_TIMEOUT && SmcCalls-BeforeSmc==8);Interrupts=0;
  MirrorStuck=FALSE;ClockFailAt=2;UINTN BeforeEnable=Enables,BeforeDisable=Disables;
  assert(SelectFifo()==EFI_DEVICE_ERROR && ControlWrites==0 && Enables-BeforeEnable==2 && Disables-BeforeDisable==2);
  assert(mClocksHeld==0 && mClock==NULL);
  puts("RAM DT edits, SPI framing, read-only SCM whitelist/retries, no direct protected access and clock rollback passed.");
  munmap(Area,Region.Length);free(Original);free(Expected);free(Before);return 0;
}
