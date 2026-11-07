// SPDX-License-Identifier: BSD-2-Clause-Patent
// WN8030 bootrom/runtime framing follows the public hid-nanosic-wn8030 driver.
// The QUP transport uses the captured Piano I2C v1 ABI, never a PI-I2C cast.
#include "PianoPogoHardware.h"
#include "PianoPogoDxe.h"
#include "../../core/PianoGeniI2cPio.h"
#include <PiDxe.h>
#include <Protocol/EFIClock.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/CacheMaintenanceLib.h>
#include "PianoPogoFirmware.h"

#define SE 0xA98000U
#define TLMM 0xF100000U
typedef struct {UINT32 Frequency,Address,Mode,Stretch,Config1,Config2;} SLAVE;
typedef struct {UINT8 *Buffer;UINT32 Bytes,Flags;} DESCRIPTOR;
typedef struct {
 UINT64 Revision;
 UINT32(EFIAPI *Open)(UINT32,VOID **);
 UINT32(EFIAPI *Read)(VOID *,CONST SLAVE *,UINT16,UINT16,UINT8 *,UINT16,UINT32 *,UINT32);
 UINT32(EFIAPI *Write)(VOID *,CONST SLAVE *,UINT16,UINT16,CONST UINT8 *,UINT16,UINT32 *,UINT32);
 UINT32(EFIAPI *Transfer)(VOID *,CONST SLAVE *,DESCRIPTOR *,UINT16,VOID *,VOID *,UINT32,UINT32 *);
 UINT32(EFIAPI *Close)(VOID *);
} NATIVE_I2C;
STATIC NATIVE_I2C *mI2c;
STATIC VOID *mBus;
STATIC PIANO_POGO_DXE mDxe;
STATIC PIANO_GENI_I2C_PIO mPio;
STATIC BOOLEAN mStarted,mRetained,mOpening,mLastIrq,mStopped;
STATIC UINT64 mLastKeepalive;
STATIC CONST SLAVE mSlave={1000,0x4c,0,0,0,0};
STATIC UINT32 mPinConfig[6],mPinOutput[2];
STATIC BOOLEAN mPinsHeld;
/* Same captured RPMh 1.1 ABI already used by the USB PHY. */
typedef struct {
 UINT64 Revision;VOID *Init;EFI_STATUS(EFIAPI *ValidDrv)(UINT32,BOOLEAN *);
 VOID *TcsCommands,*CreateInternal,*CreateExplicit;
 EFI_STATUS(EFIAPI *Create)(UINT32,CONST CHAR8 *,VOID **);
 VOID *Invalidate,*RegisterCallback;EFI_STATUS(EFIAPI *Delete)(VOID *);
 EFI_STATUS(EFIAPI *Find)(UINT32,VOID **);EFI_STATUS(EFIAPI *Churn)(VOID *,UINT32);
 VOID *Unused[7];EFI_STATUS(EFIAPI *Issue)(VOID *,UINT32,BOOLEAN,UINT32,UINT32,UINT32 *);
} RPMH;
typedef struct {UINT64 Revision;VOID *Query;EFI_STATUS(EFIAPI *Address)(VOID *,CONST CHAR8 *,UINT32 *);} CMD_DB;
typedef struct {UINT32 Data;BOOLEAN Valid;UINT8 Reserved[3];UINT32 Dirty;} VOTE;
typedef struct DRV {UINT32 Drv,State,Priority;VOTE At,Local[3];struct DRV *Next;} DRV;
typedef struct {UINT32 Address;DRV *Votes;VOID *Pdc;} RESOURCE;
STATIC RPMH *mRpmh;STATIC VOID *mPower;
STATIC struct {UINT32 Address,Before,After;BOOLEAN Changed;} mRails[6];
STATIC UINT32 Be32(CONST UINT8 *P){return (UINT32)P[0]<<24|(UINT32)P[1]<<16|(UINT32)P[2]<<8|P[3];}
STATIC UINT32 Le32(CONST UINT8 *P){return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;}
STATIC VOID Put32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
STATIC VOID PutBe32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>((3-I)*8));}
EFI_STATUS PianoPogoHardwarePrepare(VOID){
 /* Correct only this board's I2C pin numbers in the inherited RAM config.
  * The native QUP parser still supplies its clocks, firmware and FIFO setup. */
 VOID *Found=NULL;
 for(UINTN At=0;At+40<=0x40000;At+=4){UINT8 *P=(UINT8 *)(UINTN)(0x81a00000+At);
   if(Be32(P)!=0xd00dfeed)continue;UINT32 Size=Be32(P+4);
   if(Size<40||Size>0x40000-At||FdtCheckHeader(P))continue;
   if(FdtPathOffset(P,"/soc/TOP_QUP_1/TOP_QUP_1_SE_6")>=0){if(Found)return EFI_COMPROMISED_DATA;Found=P;}
 }
 if(!Found)return EFI_NOT_FOUND;
 INT32 Len,Node=FdtPathOffset(Found,"/soc/TOP_QUP_1/TOP_QUP_1_SE_6");
 CONST UINT8 *P=FdtGetProp(Found,Node,"core_offset",&Len);
 if(!P||Len!=4||Be32(P)!=0x18000)return EFI_COMPROMISED_DATA;
 for(UINTN State=0;State<2;++State){
   P=FdtGetProp(Found,Node,State?"pinctrl-1":"pinctrl-0",&Len);
   if(!P||Len!=4)return EFI_COMPROMISED_DATA;
   INT32 Pins=FdtNodeOffsetByPhandle(Found,Be32(P));
   UINT8 *Config=(VOID *)(UINTN)FdtGetProp(Found,Pins,"config",&Len);
   if(!Config||Len!=16||Be32(Config)!=46||Be32(Config+8)!=47)return EFI_COMPROMISED_DATA;
 }
 for(UINTN State=0;State<2;++State){
   P=FdtGetProp(Found,Node,State?"pinctrl-1":"pinctrl-0",&Len);
   INT32 Pins=FdtNodeOffsetByPhandle(Found,Be32(P));UINT8 *Config=(VOID *)(UINTN)FdtGetProp(Found,Pins,"config",&Len);
   PutBe32(Config,56);PutBe32(Config+8,57);
 }
 WriteBackDataCacheRange(Found,FdtTotalSize(Found));return EFI_SUCCESS;
}
STATIC EFI_STATUS Rail(UINT32 Address,UINT32 Value){UINT32 Request=0;EFI_STATUS S=mRpmh->Issue(mPower,0,TRUE,Address,Value,&Request);return S==EFI_SUCCESS?mRpmh->Churn(mPower,Request):S;}
STATIC EFI_STATUS PowerStart(VOID){
 EFI_GUID Rpmh={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
 EFI_GUID DbGuid={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
 CMD_DB *Db=NULL;EFI_STATUS S=gBS->LocateProtocol(&Rpmh,NULL,(VOID **)&mRpmh);
 if(S==EFI_SUCCESS)S=gBS->LocateProtocol(&DbGuid,NULL,(VOID **)&Db);
 if(S!=EFI_SUCCESS||!mRpmh||mRpmh->Revision!=0x10001||!Db||Db->Revision!=0x10000)return EFI_NOT_READY;
 CONST CHAR8 *Names[]={"ldob11","ldob14"};CONST UINT32 Mv[]={1800,3300};
 for(UINTN I=0;I<2;++I){UINT32 Base=0;S=Db->Address(Db,Names[I],&Base);if(S!=EFI_SUCCESS||(Base>>16)!=4||(Base&255))return EFI_UNSUPPORTED;
   for(UINTN J=0;J<3;++J){RESOURCE *R=NULL;S=mRpmh->Find(Base+(UINT32)J*4,(VOID **)&R);if(S!=EFI_SUCCESS)return S;
     UINT32 Before=0;DRV *D=R?R->Votes:NULL;UINTN N=0;
     if(R&&R->Address!=Base+J*4)return EFI_COMPROMISED_DATA;
     for(;D&&N<16;D=D->Next,++N)if(D->Drv==2)break;
     if(D&&N==16)return EFI_COMPROMISED_DATA;if(D&&D->Local[0].Valid)Before=D->Local[0].Data;
     UINTN Index=I*3+J;mRails[Index].Address=Base+(UINT32)J*4;mRails[Index].Before=Before;mRails[Index].After=MAX(Before,J==0?Mv[I]:J==1?1U:7U);
   }
 }
 S=mRpmh->Create(2,"SunUEFI keyboard",&mPower);if(S!=EFI_SUCCESS||!mPower)return EFI_OUT_OF_RESOURCES;
 for(UINTN I=0;I<6;++I){mRails[I].Changed=TRUE;S=Rail(mRails[I].Address,mRails[I].After);if(S!=EFI_SUCCESS)return S;}
 gBS->Stall(2000);return EFI_SUCCESS;
}
STATIC EFI_STATUS PowerStop(VOID){
 if(!mPower)return EFI_SUCCESS;
 for(UINTN I=6;I>0;--I)if(mRails[I-1].Changed){EFI_STATUS S=Rail(mRails[I-1].Address,mRails[I-1].Before);if(S!=EFI_SUCCESS)return S;mRails[I-1].Changed=FALSE;}
 EFI_STATUS S=mRpmh->Delete(mPower);if(S==EFI_SUCCESS)mPower=NULL;return S;
}
STATIC VOID SetPin(UINT32 Gpio,BOOLEAN High){UINTN B=TLMM+Gpio*0x1000;MmioWrite32(B+4,High?2:0);MmioWrite32(B,(MmioRead32(B)&~0x23fU)|BIT9);MemoryFence();}
STATIC VOID PinsStart(VOID){
 CONST UINT32 Gpio[]={3,188,95,97,56,57};
 for(UINTN I=0;I<6;++I)mPinConfig[I]=MmioRead32(TLMM+Gpio[I]*0x1000);
 mPinOutput[0]=MmioRead32(TLMM+3*0x1000+4);mPinOutput[1]=MmioRead32(TLMM+188*0x1000+4);mPinsHeld=TRUE;
 SetPin(3,FALSE);SetPin(188,FALSE);gBS->Stall(2000);SetPin(188,TRUE);
 for(UINTN I=2;I<4;++I){UINTN B=TLMM+Gpio[I]*0x1000;MmioWrite32(B,MmioRead32(B)&~0x23cU);}
 gBS->Stall(500000);SetPin(3,TRUE);gBS->Stall(50000);
}
STATIC VOID PinsStop(VOID){
 if(!mPinsHeld)return;CONST UINT32 Gpio[]={3,188,95,97,56,57};
 for(UINTN I=0;I<2;++I)MmioWrite32(TLMM+Gpio[I]*0x1000+4,mPinOutput[I]);
 for(UINTN I=0;I<6;++I)MmioWrite32(TLMM+Gpio[I]*0x1000,mPinConfig[I]);mPinsHeld=FALSE;
}
STATIC UINT64 EFIAPI Now(VOID *Context){(VOID)Context;return GetTimeInNanoSecond(GetPerformanceCounter())/1000;}
STATIC EFI_STATUS Exact(UINT32 Status,UINT32 Done,UINT32 Bytes){return !Status&&Done==Bytes?EFI_SUCCESS:Status==22?EFI_NO_RESPONSE:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS Write(UINT8 Register,CONST UINT8 *Data,UINT16 Bytes){
 UINT32 Done=0;UINT32 R=mI2c->Write(mBus,&mSlave,Register,1,Data,Bytes,&Done,10);
 if(R||Done!=Bytes)DEBUG((DEBUG_WARN,"PIANO_POGO_WRITE register=%x bytes=%u native=%u done=%u lines=%x\n",Register,Bytes,R,Done,MmioRead32(SE+0x908)));
 return Exact(R,Done,Bytes);
}
STATIC EFI_STATUS Read(UINT8 Register,UINT8 *Data,UINT16 Bytes){
 UINT32 Done=0;UINT32 R=mI2c->Read(mBus,&mSlave,Register,1,Data,Bytes,&Done,10);
 /* This verified native image counts the completed register descriptor again
  * in RVA 2D9C; its Read wrapper removes only one of those address bytes.
  * Require the exact native count, not an arbitrary short/overlong read. */
 if(R||Done!=(UINT32)Bytes+1)DEBUG((DEBUG_WARN,"PIANO_POGO_READ register=%x bytes=%u native=%u done=%u lines=%x\n",Register,Bytes,R,Done,MmioRead32(SE+0x908)));
 return Exact(R,Done,(UINT32)Bytes+1);
}
STATIC EFI_STATUS BootCommand(UINT8 Command){EFI_STATUS S=Write(0x5c,&Command,1);if(S==EFI_SUCCESS)gBS->Stall(3000);return S;}
STATIC EFI_STATUS BootState(UINT8 *State){EFI_STATUS S=BootCommand(5);if(S==EFI_SUCCESS)S=Read(0x5c,State,1);DEBUG((DEBUG_WARN,"PIANO_POGO_BOOT_STATE status=%r value=%x\n",S,*State));return S;}
STATIC EFI_STATUS LoadFirmware(VOID){
 CONST UINT8 *F=mPianoPogoFirmware;
 if(sizeof(mPianoPogoFirmware)<0x8000||CompareMem(F+0x7fc0,"NANO IC",7))return EFI_COMPROMISED_DATA;
 UINT32 Bytes=Le32(F+0x7ff0);
 if(!Bytes||Bytes>sizeof(mPianoPogoFirmware)-0x8000||Bytes>0x20000)return EFI_COMPROMISED_DATA;
 UINT8 Id=0,State=0;EFI_STATUS S=BootCommand(4);if(S==EFI_SUCCESS)S=Read(0x5c,&Id,1);
 DEBUG((DEBUG_WARN,"PIANO_POGO_BOOT_ID status=%r value=%x\n",S,Id));
 if(S!=EFI_SUCCESS||Id!=0xc8)return S==EFI_SUCCESS?EFI_UNSUPPORTED:S;
 S=BootState(&State);if(S!=EFI_SUCCESS||State!=0)return S==EFI_SUCCESS?EFI_NOT_READY:S;
 UINT8 Header[65]={6};CopyMem(Header+1,F+0x7fc0,7);Header[8]=0x40;Header[9]=0x81;
 Put32(Header+10,0x2345);Put32(Header+14,0x40);Put32(Header+18,Bytes);
 CopyMem(Header+22,F+0x7fd5,10);
 UINT32 Sum=0;for(UINT32 I=0;I<Bytes;I+=4){UINT32 V=0;for(UINT32 J=0;J<4&&I+J<Bytes;++J)V|=(UINT32)F[0x8000+I+J]<<(8*J);Sum+=V;}Put32(Header+53,Sum);
 for(UINTN I=1;I<64;++I)Header[64]+=Header[I];
 S=Write(0x5c,Header,sizeof(Header));if(S==EFI_SUCCESS){gBS->Stall(3000);S=BootState(&State);}
 if(S!=EFI_SUCCESS||State!=1)return S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;
 for(UINT32 At=0;At<Bytes;){
   UINT8 Packet[512]={7};UINT32 N=MIN(503U,Bytes-At);
   PutBe32(Packet+1,At);PutBe32(Packet+5,N);CopyMem(Packet+9,F+0x8000+At,N);
   S=Write(0x5c,Packet,sizeof(Packet));if(S!=EFI_SUCCESS)return S;
   gBS->Stall(3000);At+=N;
 }
 S=BootCommand(8);if(S==EFI_SUCCESS){gBS->Stall(10000);S=BootState(&State);}
 if(S!=EFI_SUCCESS||State!=3)return S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;
 S=BootCommand(9);if(S==EFI_SUCCESS)gBS->Stall(50000);
 DEBUG((DEBUG_WARN,"PIANO_POGO_MCU firmware_bytes=%u status=%r ram_download=1 persistent_write=0\n",Bytes,S));return S;
}
STATIC EFI_STATUS RuntimeCommand(UINT8 Command){
 UINT8 Data[66]={0x32,0,0x4e,0x31,0x80,0x38,0,1,1};Data[6]=Command;
 for(UINTN I=2;I<9;++I)Data[9]+=Data[I];return Write(0x4c,Data,sizeof(Data));
}
STATIC BOOLEAN Verified(NATIVE_I2C *I){
 if(!I||I->Revision!=0x10000)return FALSE;
 EFI_HANDLE *Handles=NULL;UINTN Count=0;BOOLEAN Found=FALSE;
 EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
 if(S!=EFI_SUCCESS)return FALSE;
 for(UINTN N=0;N<Count;++N){EFI_LOADED_IMAGE_PROTOCOL *L=NULL;
   if(gBS->HandleProtocol(Handles[N],&gEfiLoadedImageProtocolGuid,(VOID **)&L)!=EFI_SUCCESS||!L||L->ImageSize!=0xf000)continue;
   UINTN B=(UINTN)L->ImageBase;
   if((UINTN)I==B+0xc0e8&&(UINTN)I->Open==B+0x2720&&(UINTN)I->Read==B+0x27d8&&
     (UINTN)I->Write==B+0x2c60&&(UINTN)I->Transfer==B+0x28d4&&(UINTN)I->Close==B+0x2764)Found=TRUE;
 }
 FreePool(Handles);return Found;
}
STATIC EFI_STATUS EFIAPI Ready(VOID *Context){return Context==&mDxe&&mStarted&&!mOpening&&!mRetained&&mBus?EFI_SUCCESS:EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI DataReady(VOID *Context,BOOLEAN *ReadyOut){
 if(!ReadyOut||Ready(Context)!=EFI_SUCCESS)return EFI_NOT_READY;
 BOOLEAN Level=(MmioRead32(TLMM+97*0x1000+4)&1)!=0;
 *ReadyOut=Level&&!mLastIrq;mLastIrq=Level;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PioRead(VOID *Context,UINT32 Offset,UINT32 *Value){
 if(Context!=&mDxe||!Value||Offset>=0x1000||(Offset&3))return EFI_INVALID_PARAMETER;
 *Value=MmioRead32(SE+Offset);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PioWrite(VOID *Context,UINT32 Offset,UINT32 Value){
 if(Context!=&mDxe)return EFI_INVALID_PARAMETER;
 switch(Offset){case 0x614:case 0x618:case 0x80c:case 0x810:case 0x26c:case 0x270:case 0x600:case 0x604:case 0x700:break;default:return EFI_ACCESS_DENIED;}
 MmioWrite32(SE+Offset,Value);MemoryFence();return EFI_SUCCESS;
}
STATIC EFI_STATUS StartPio(VOID){
 PIANO_GENI_PIO_IO Io={SE,0x4000,&mDxe,PioRead,PioWrite,Now};
 PIANO_GENI_PIO_SNAPSHOT Snapshot;EFI_STATUS S=PianoGeniI2cPioCapture(&Io,&Snapshot);if(S!=EFI_SUCCESS)return S;
 DEBUG((DEBUG_WARN,"PIANO_POGO_PIO firmware=%x clock_select=%x master_clock=%x scl=%x status=%x irq=%x secondary_irq=%x tx=%x rx=%x lines=%x events=%x route=%x\n",Snapshot.Firmware,Snapshot.ClockSelect,Snapshot.MasterClock,Snapshot.SclCounters,Snapshot.Status,Snapshot.MasterIrq,Snapshot.SecondaryIrq,Snapshot.TxFifo,Snapshot.RxFifo,Snapshot.IoLines,Snapshot.Events,Snapshot.IrqRoute));
 /* Open/first transfers initialize this exact engine and packing. Validate
  * them again rather than trusting the native success return by itself. */
 EFI_GUID ClockGuid=EFI_CLOCK_PROTOCOL_GUID;EFI_CLOCK_PROTOCOL *Clock=NULL;UINTN ClockId=0;UINT32 Hz=0;
 S=gBS->LocateProtocol(&ClockGuid,NULL,(VOID **)&Clock);
 if(S==EFI_SUCCESS&&Clock)S=Clock->GetClockID(Clock,"gcc_qupv3_wrap1_s6_clk",&ClockId);
 if(S==EFI_SUCCESS&&Clock)S=Clock->GetClockFreqHz(Clock,ClockId,&Hz);
 DEBUG((DEBUG_WARN,"PIANO_POGO_CLOCK status=%r hz=%u\n",S,Hz));
 if(S!=EFI_SUCCESS||Hz!=19200000)return EFI_UNSUPPORTED;
 /* The captured native 19.2MHz/1MHz row is divider1, high3, low9,
  * cycle18 (RVA D178), not Linux's high2/low8 timing. */
 if((Snapshot.Firmware>>8&255)!=3||Snapshot.ClockSelect||Snapshot.MasterClock!=17||
     Snapshot.SclCounters!=((3U<<20)|(9U<<10)|18U))return EFI_UNSUPPORTED;
 /* Native FIFO initialization leaves RX_LAST latched on the secondary
  * status bank. Acknowledge only that known, empty, inactive condition. */
 if(Snapshot.SecondaryIrq==BIT27&&!(Snapshot.Status&0x1001)&&!Snapshot.SecondaryControl&&!(Snapshot.RxFifo&0x1ffffffU)){
   MmioWrite32(SE+0x648,BIT27);MemoryFence();
   S=PianoGeniI2cPioCapture(&Io,&Snapshot);if(S!=EFI_SUCCESS)return S;
 }
 PIANO_GENI_PIO_CONTRACT C={Snapshot.Firmware,Snapshot.ClockSelect,Snapshot.MasterClock,Snapshot.SclCounters,
   TRUE,TRUE,TRUE,TRUE,TRUE,TRUE,TRUE,TRUE};
 return PianoGeniI2cPioInitialize(&mPio,&Io,&C);
}
STATIC EFI_STATUS EFIAPI ReadRuntime(VOID *Context,UINT64 Deadline,UINT8 Frame[68],UINTN *Bytes){
 if(!Bytes||!Frame||Ready(Context)!=EFI_SUCCESS)return EFI_NOT_READY;*Bytes=0;
 EFI_STATUS S=PianoGeniI2cPioReadRuntime(&mPio,Deadline,Frame,Bytes);
 if(S!=EFI_SUCCESS||mPio.Attempts<=8)DEBUG((DEBUG_WARN,"PIANO_POGO_FRAME status=%r bytes=%u attempts=%u received=%u polls=%u cleanup=%r clean=%u lost=%u header=%02x,%02x,%02x report=%02x\n",S,(UINT32)*Bytes,mPio.Attempts,mPio.Received,mPio.Polls,mPio.CleanupStatus,mPio.Clean,mPio.OwnershipLost,Frame[0],Frame[1],Frame[2],Frame[3]));
 return S;
}
STATIC EFI_STATUS EFIAPI Stop(VOID *Context,PIANO_POGO_BACKEND_STOP *Report){
 if(Context!=&mDxe||!Report)return EFI_INVALID_PARAMETER;ZeroMem(Report,sizeof(*Report));
 if((MmioRead32(SE+0x40)&0x1001)||mPio.InRead||(mPio.Signature&&!mPio.Clean)){Report->Retained=mRetained=TRUE;return Report->Status=EFI_NOT_READY;}
 UINT32 S=mBus?mI2c->Close(mBus):0;
 if(S){mRetained=TRUE;Report->Retained=TRUE;return Report->Status=EFI_DEVICE_ERROR;}
 mBus=NULL;mStarted=FALSE;PinsStop();EFI_STATUS Power=PowerStop();
 if(Power!=EFI_SUCCESS){Report->Retained=mRetained=TRUE;return Report->Status=Power;}
 Report->Quiet=Report->Clean=TRUE;mStopped=TRUE;return Report->Status=EFI_SUCCESS;
}
EFI_STATUS PianoPogoHardwareStart(CONST VOID *Fdt){
 if(mStarted||mBus)return EFI_ALREADY_STARTED;
 INT32 Node=FdtPathOffset(Fdt,"/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000"),Len=0;
 CONST UINT8 *Reg=Node<0?NULL:FdtGetProp(Fdt,Node,"reg",&Len);
 if(!Reg||Len!=8||Be32(Reg)!=SE||Be32(Reg+4)!=0x4000)return EFI_UNSUPPORTED;
 EFI_GUID Guid={0xb27ae8b1,0x3e10,0x4d07,{0xab,0x5c,0xeb,0x9a,0x6d,0xc6,0xfa,0x8f}};
 EFI_STATUS S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&mI2c);
 if(S!=EFI_SUCCESS||!Verified(mI2c))return EFI_NOT_FOUND;
 S=PowerStart();if(S!=EFI_SUCCESS){EFI_STATUS R=PowerStop();if(R!=EFI_SUCCESS)mRetained=TRUE;return R==EFI_SUCCESS?S:R;}PinsStart();
 mOpening=TRUE;
 /* TOP_QUP_0 has 10 engines. The audited one-based mapper therefore selects
  * TOP_QUP_1_SE_6 with instance 17, not Android's adapter number 4. */
 UINT32 Native=mI2c->Open(17,&mBus);mOpening=FALSE;
 DEBUG((DEBUG_WARN,"PIANO_POGO_I2C_OPEN instance=17 status=%u handle=%p\n",Native,mBus));
 if(Native||!mBus){mRetained=TRUE;return EFI_DEVICE_ERROR;}
 for(UINT32 Pin=56;Pin<=57;++Pin){UINTN B=TLMM+Pin*0x1000;UINT32 Before=MmioRead32(B);
   /* SM8750 pinctrl mux1, 2mA, no pull, input/open-drain controlled by SE. */
   MmioWrite32(B,(Before&~0x3ffU)|4U);MemoryFence();
   DEBUG((DEBUG_WARN,"PIANO_POGO_PIN gpio=%u before=%x after=%x io=%x\n",Pin,Before,MmioRead32(B),MmioRead32(B+4)));
 }
 DEBUG((DEBUG_WARN,"PIANO_POGO_SE firmware=%x if_disable=%x dma=%x lines=%x\n",MmioRead32(SE+0x68),MmioRead32(SE+0x64),MmioRead32(SE+0x258),MmioRead32(SE+0x908)));
 if((MmioRead32(SE+0x68)>>8&255)!=3||MmioRead32(SE+0x64)&1||MmioRead32(SE+0x258))S=EFI_UNSUPPORTED;
 else S=LoadFirmware();
 if(S==EFI_SUCCESS)S=RuntimeCommand(0x25);
 if(S==EFI_SUCCESS)S=RuntimeCommand(0x21);
 if(S==EFI_SUCCESS)S=StartPio();
 if(S!=EFI_SUCCESS){PIANO_POGO_BACKEND_STOP R;EFI_STATUS StopStatus=Stop(&mDxe,&R);return StopStatus==EFI_SUCCESS?S:StopStatus;}
 mStarted=TRUE;mLastIrq=FALSE;mLastKeepalive=Now(NULL);
 PIANO_POGO_DXE_BACKEND Backend={1,&mDxe,Ready,DataReady,Now,ReadRuntime,Stop};
 EFI_SIMPLE_POINTER_MODE Mode={1,1,1,TRUE,TRUE};
 S=PianoPogoDxeStart(&mDxe,&Backend,&Mode);
 if(S!=EFI_SUCCESS)mRetained=TRUE;
 return S;
}
EFI_STATUS PianoPogoHardwarePump(VOID){
 if(!mStarted)return EFI_NOT_STARTED;
 EFI_STATUS S=PianoPogoDxePump(&mDxe);
 UINT64 Time=Now(NULL);
 if((S==EFI_SUCCESS||S==EFI_NOT_READY)&&Time-mLastKeepalive>=12000000){mLastKeepalive=Time;S=RuntimeCommand(0x25);}
 if(S!=EFI_SUCCESS&&S!=EFI_NOT_READY)mRetained=TRUE;
 return S;
}
EFI_STATUS PianoPogoHardwareStop(VOID){if(mStopped)return EFI_SUCCESS;return mDxe.Signature?PianoPogoDxeStop(&mDxe):mBus||mPower||mPinsHeld?EFI_NOT_READY:EFI_SUCCESS;}
BOOLEAN PianoPogoHardwareRetained(VOID){return mRetained||mDxe.Report.Retained;}
