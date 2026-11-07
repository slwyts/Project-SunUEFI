// SPDX-License-Identifier: BSD-2-Clause-Patent
// Sources: stock DT nanosic@4c and mi_qupv3_se6_i2c pinctrl, the Linux 7.3
// keyboard node in patches/linux/7.3/0001-platform.patch and pinctrl-sm8750.c
#include "PianoPogoBoard.h"
#include <Protocol/EFIClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#define TLMM 0xF100000U
#define CTL(G) (TLMM+(G)*0x1000U)
#define IO(G) (CTL(G)+4U)
#define INTR_CFG(G) (CTL(G)+8U)
#define INTR_STATUS(G) (CTL(G)+0xCU)
#define CTL_FIELDS 0x3FFU          // pull[1:0] function[5:2] drive[8:6] OE[9]
#define EGPIO_PRESENT BIT11
#define EGPIO_ENABLE BIT12
#define PULL_NONE 0U
#define PULL_DOWN 1U
#define OE BIT9
#define PIN_SDA 0
#define PIN_SCL 1
#define PIN_RESET 2
#define PIN_SLEEP 3
#define PIN_IRQ 4
STATIC CONST UINT32 mGpio[PIANO_POGO_PINS]={56,57,188,3,97};
STATIC CONST CHAR8 *mClockNames[PIANO_POGO_CLOCKS]={"gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk",
  "gcc_qupv3_wrap1_core_2x_clk","gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_s6_clk"};

// Native RPMh 1.1 ABI as verified by PianoUsbPhy; votes go through RpmhDxe,
// never raw PMIC registers
typedef struct {
  UINT64 Revision;
  VOID *Init;
  EFI_STATUS(EFIAPI *ValidDrv)(UINT32,BOOLEAN *);
  VOID *TcsCommands,*CreateInternal,*CreateExplicit;
  EFI_STATUS(EFIAPI *Create)(UINT32,CONST CHAR8 *,VOID **);
  VOID *Invalidate,*RegisterCallback;
  EFI_STATUS(EFIAPI *Delete)(VOID *);
  EFI_STATUS(EFIAPI *Find)(UINT32,VOID **);
  EFI_STATUS(EFIAPI *Churn)(VOID *,UINT32);
  VOID *Unused[7];
  EFI_STATUS(EFIAPI *Issue)(VOID *,UINT32,BOOLEAN,UINT32,UINT32,UINT32 *);
} POGO_RPMH;
typedef struct {UINT64 Revision;VOID *Query;EFI_STATUS(EFIAPI *Address)(VOID *,CONST CHAR8 *,UINT32 *);} POGO_CMD_DB;
typedef struct {UINT32 Data;BOOLEAN Valid;UINT8 Reserved[3];UINT32 Dirty;} POGO_RPMH_VOTE;
typedef struct POGO_RPMH_DRV {UINT32 Drv,State,Priority;POGO_RPMH_VOTE AtRpmh,Local[3];struct POGO_RPMH_DRV *Next;} POGO_RPMH_DRV;
typedef struct {UINT32 Address;POGO_RPMH_DRV *Votes;VOID *PdcVotes;} POGO_RPMH_RESOURCE;
STATIC_ASSERT(OFFSET_OF(POGO_RPMH,Issue)==0x98,"Piano RPMh issue ABI");
STATIC_ASSERT(OFFSET_OF(POGO_RPMH_DRV,Local)==0x18 && OFFSET_OF(POGO_RPMH_DRV,Next)==0x40,"Piano RPMh vote ABI");

STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}

EFI_STATUS PianoPogoBoardClocksOn(PIANO_POGO_BOARD *B) {
  EFI_GUID Guid=EFI_CLOCK_PROTOCOL_GUID;EFI_CLOCK_PROTOCOL *Clock=NULL;
  if(B==NULL || B->Clock!=NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if(S!=EFI_SUCCESS || Clock==NULL || Clock->Version!=0x1000b || !Clock->GetClockID || !Clock->EnableClock ||
     !Clock->DisableClock || !Clock->IsClockEnabled || !Clock->IsClockOn || !Clock->GetClockFreqHz)return EFI_UNSUPPORTED;
  B->Clock=Clock;
  for(UINTN I=0;I<PIANO_POGO_CLOCKS;++I) {
    BOOLEAN Enabled=FALSE,On=FALSE;
    S=Exact(Clock->GetClockID(Clock,mClockNames[I],&B->ClockIds[I]));
    if(S==EFI_SUCCESS)S=Exact(Clock->IsClockEnabled(Clock,B->ClockIds[I],&Enabled));
    if(S==EFI_SUCCESS && Enabled)B->ClockWasOn|=1U<<I;
    if(S==EFI_SUCCESS){S=Exact(Clock->EnableClock(Clock,B->ClockIds[I]));if(S==EFI_SUCCESS)B->ClockHeld|=1U<<I;}
    if(S==EFI_SUCCESS)S=Exact(Clock->IsClockOn(Clock,B->ClockIds[I],&On));
    DEBUG((DEBUG_WARN,"PIANO_POGO_CLOCK name=%a was_enabled=%u on=%u status=%r\n",mClockNames[I],Enabled,On,S));
    if(S==EFI_SUCCESS && !On)S=EFI_NOT_READY;
    if(S!=EFI_SUCCESS)return S;
  }
  UINT32 Hz=0;S=Exact(Clock->GetClockFreqHz(Clock,B->ClockIds[PIANO_POGO_CLOCKS-1],&Hz));
  DEBUG((DEBUG_WARN,"PIANO_POGO_SE6_CLOCK hz=%u status=%r\n",Hz,S));
  if(S!=EFI_SUCCESS)return S;
  if(Hz<1000000U)return EFI_NOT_READY;
  B->SeSourceHz=Hz;return EFI_SUCCESS;
}

STATIC VOID SavePin(PIANO_POGO_BOARD *B,UINTN P) {
  PIANO_POGO_PIN *Pin=&B->Pins[P];
  if(Pin->Saved)return;
  Pin->Gpio=mGpio[P];Pin->Ctl=MmioRead32(CTL(Pin->Gpio));Pin->Io=MmioRead32(IO(Pin->Gpio));
  Pin->IntrCfg=MmioRead32(INTR_CFG(Pin->Gpio));Pin->Saved=TRUE;
  DEBUG((DEBUG_WARN,"PIANO_POGO_PIN gpio=%u ctl=%08x io=%08x intr=%08x\n",Pin->Gpio,Pin->Ctl,Pin->Io,Pin->IntrCfg));
}
// An eGPIO-capable pin is claimed for the application TLMM, as Linux does
// when it selects a function
STATIC EFI_STATUS SetCtl(PIANO_POGO_BOARD *B,UINTN P,UINT32 Function,UINT32 Pull,BOOLEAN Output) {
  SavePin(B,P);PIANO_POGO_PIN *Pin=&B->Pins[P];
  UINT32 Value=(Pin->Ctl&~(CTL_FIELDS|EGPIO_ENABLE))|(Function<<2)|Pull|(Output?OE:0);
  if(Pin->Ctl&EGPIO_PRESENT)Value|=EGPIO_ENABLE;
  Pin->Changed=TRUE;MmioWrite32(CTL(Pin->Gpio),Value);MemoryFence();
  UINT32 Back=MmioRead32(CTL(Pin->Gpio));
  return (Back&(CTL_FIELDS|EGPIO_ENABLE))==(Value&(CTL_FIELDS|EGPIO_ENABLE))?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
STATIC EFI_STATUS Drive(PIANO_POGO_BOARD *B,UINTN P,BOOLEAN High) {
  PIANO_POGO_PIN *Pin=&B->Pins[P];
  if(!Pin->Saved)return EFI_NOT_READY;
  MmioWrite32(IO(Pin->Gpio),High?BIT1:0);MemoryFence();
  return ((MmioRead32(IO(Pin->Gpio))>>1)&1)==(High?1U:0U)?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

EFI_STATUS PianoPogoBoardI2cPins(PIANO_POGO_BOARD *B) {
  EFI_STATUS S=SetCtl(B,PIN_SDA,1,PULL_NONE,FALSE);
  if(S==EFI_SUCCESS)S=SetCtl(B,PIN_SCL,1,PULL_NONE,FALSE);
  return S;
}
EFI_STATUS PianoPogoBoardSetReset(PIANO_POGO_BOARD *B,BOOLEAN Asserted){return B->McuPinsReady?Drive(B,PIN_RESET,!Asserted):EFI_NOT_READY;}
EFI_STATUS PianoPogoBoardSetSleep(PIANO_POGO_BOARD *B,BOOLEAN Asserted){return B->McuPinsReady?Drive(B,PIN_SLEEP,!Asserted):EFI_NOT_READY;}

STATIC EFI_STATUS Vote(PIANO_POGO_BOARD *B,UINT32 Address,UINT32 Value) {
  POGO_RPMH *Rpmh=B->Rpmh;UINT32 Request=0;
  EFI_STATUS S=Exact(Rpmh->Issue(B->PowerHandle,0,TRUE,Address,Value,&Request));
  if(S==EFI_SUCCESS)S=Exact(Rpmh->Churn(B->PowerHandle,Request));
  DEBUG((DEBUG_WARN,"PIANO_POGO_POWER_VOTE address=%08x value=%u status=%r\n",Address,Value,S));
  return S;
}

EFI_STATUS PianoPogoBoardPowerOn(PIANO_POGO_BOARD *B) {
  if(B==NULL || B->PowerHandle!=NULL)return EFI_INVALID_PARAMETER;
  // Hold the MCU in reset and asleep before its supplies come up
  SavePin(B,PIN_RESET);SavePin(B,PIN_SLEEP);
  MmioWrite32(IO(mGpio[PIN_RESET]),0);MmioWrite32(IO(mGpio[PIN_SLEEP]),0);MemoryFence();
  EFI_STATUS S=SetCtl(B,PIN_RESET,0,PULL_NONE,TRUE);
  if(S==EFI_SUCCESS)S=SetCtl(B,PIN_SLEEP,0,PULL_DOWN,TRUE);
  if(S!=EFI_SUCCESS)return S;
  B->McuPinsReady=TRUE;
  if((S=PianoPogoBoardSetReset(B,TRUE))!=EFI_SUCCESS || (S=PianoPogoBoardSetSleep(B,TRUE))!=EFI_SUCCESS)return S;
  EFI_GUID RpmhGuid={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
  EFI_GUID DbGuid={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
  POGO_RPMH *Rpmh=NULL;POGO_CMD_DB *Db=NULL;
  if((S=gBS->LocateProtocol(&RpmhGuid,NULL,(VOID **)&Rpmh))!=EFI_SUCCESS ||
     (S=gBS->LocateProtocol(&DbGuid,NULL,(VOID **)&Db))!=EFI_SUCCESS)return S;
  if(!Rpmh || Rpmh->Revision!=0x10001 || !Rpmh->ValidDrv || !Rpmh->Create || !Rpmh->Delete || !Rpmh->Issue ||
     !Rpmh->Churn || !Rpmh->Find || !Db || Db->Revision!=0x10000 || !Db->Address)return EFI_UNSUPPORTED;
  BOOLEAN Valid=FALSE;S=Rpmh->ValidDrv(2,&Valid);if(S!=EFI_SUCCESS || !Valid)return EFI_UNSUPPORTED;
  B->Rpmh=Rpmh;
  // I/O supply first: 3.3 V must never be up while 1.8 V is down
  STATIC CONST struct {CONST CHAR8 *Name;UINT32 Mv;} Rails[]={{"ldob11",1800},{"ldob14",3296}};
  for(UINTN I=0;I<ARRAY_SIZE(Rails);++I) {
    UINT32 Base=0;S=Exact(Db->Address(Db,Rails[I].Name,&Base));
    if(S!=EFI_SUCCESS || (Base>>16)!=4 || (Base&255))return EFI_UNSUPPORTED;
    for(UINT32 J=0;J<3;++J) {
      UINT32 Address=Base+J*4,Old=0;POGO_RPMH_RESOURCE *Resource=NULL;
      S=Exact(Rpmh->Find(Address,(VOID **)&Resource));if(S!=EFI_SUCCESS)return S;
      if(Resource) {
        if(Resource->Address!=Address)return EFI_COMPROMISED_DATA;
        POGO_RPMH_DRV *Drv=Resource->Votes;UINTN N=0;
        for(;Drv && N<16;Drv=Drv->Next,++N)if(Drv->Drv==2)break;
        if(Drv && N==16)return EFI_COMPROMISED_DATA;
        if(Drv && Drv->Local[0].Valid)Old=Drv->Local[0].Data;
      }
      UINT32 Want=J==0?Rails[I].Mv:J==1?1:7; // voltage mV, enable, high-power mode
      B->Votes[B->VoteCount].Address=Address;B->Votes[B->VoteCount].Before=Old;
      B->Votes[B->VoteCount].After=MAX(Old,Want);++B->VoteCount;
    }
  }
  S=Exact(Rpmh->Create(2,"SunUEFI keyboard",&B->PowerHandle));
  if(S!=EFI_SUCCESS || !B->PowerHandle){B->PowerHandle=NULL;return S==EFI_SUCCESS?EFI_OUT_OF_RESOURCES:S;}
  B->RailsOn=TRUE;
  for(UINT32 I=0;I<B->VoteCount;++I) {
    B->Votes[I].Changed=TRUE;S=Vote(B,B->Votes[I].Address,B->Votes[I].After);if(S!=EFI_SUCCESS)return S;
  }
  return EFI_SUCCESS;
}

EFI_STATUS PianoPogoBoardArmIrq(PIANO_POGO_BOARD *B) {
  SavePin(B,PIN_IRQ);
  // Input, keeping the pull the board already has
  EFI_STATUS S=SetCtl(B,PIN_IRQ,0,B->Pins[PIN_IRQ].Ctl&3,FALSE);
  if(S!=EFI_SUCCESS)return S;
  // Rising edge latched in TLMM; interrupt enable stays clear, so nothing
  // reaches the GIC
  UINT32 Cfg=(B->Pins[PIN_IRQ].IntrCfg&~(BIT0|BIT1|BIT2|BIT3|BIT4|(7U<<8)))|BIT4|BIT2|BIT1|(3U<<8);
  MmioWrite32(INTR_CFG(mGpio[PIN_IRQ]),Cfg);MemoryFence();
  MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();
  if(MmioRead32(INTR_CFG(mGpio[PIN_IRQ]))!=Cfg)return EFI_ACCESS_DENIED;
  B->IrqArmed=TRUE;return EFI_SUCCESS;
}

BOOLEAN PianoPogoBoardIrqPending(PIANO_POGO_BOARD *B) {
  if(!B->IrqArmed || !(MmioRead32(INTR_STATUS(mGpio[PIN_IRQ]))&BIT0))return FALSE;
  MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();
  B->EdgeSeen=TRUE;return TRUE;
}

EFI_STATUS PianoPogoBoardRelease(PIANO_POGO_BOARD *B,PIANO_POGO_BOARD_RELEASE *R) {
  if(B==NULL || R==NULL)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<sizeof(*R);++I)((UINT8 *)R)[I]=0;
  R->Attempted=TRUE;R->Pins=R->Rails=R->Clocks=EFI_SUCCESS;
  // Linux power_off order: sleep, reset, 10 ms, dvdd off, vddio off
  if(B->McuPinsReady) {
    EFI_STATUS S=PianoPogoBoardSetSleep(B,TRUE);EFI_STATUS T=PianoPogoBoardSetReset(B,TRUE);
    if(S!=EFI_SUCCESS)R->Pins=S;else if(T!=EFI_SUCCESS)R->Pins=T;
    if(B->RailsOn)gBS->Stall(10000);
  }
  if(B->PowerHandle!=NULL) {
    POGO_RPMH *Rpmh=B->Rpmh;
    for(UINT32 I=B->VoteCount;I>0;--I)if(B->Votes[I-1].Changed) {
      EFI_STATUS S=Vote(B,B->Votes[I-1].Address,B->Votes[I-1].Before);
      if(S==EFI_SUCCESS){B->Votes[I-1].Changed=FALSE;++R->VotesRestored;}else if(R->Rails==EFI_SUCCESS)R->Rails=S;
    }
    if(R->Rails==EFI_SUCCESS) {
      EFI_STATUS S=Exact(Rpmh->Delete(B->PowerHandle));
      if(S==EFI_SUCCESS){B->PowerHandle=NULL;B->RailsOn=FALSE;}else R->Rails=S;
    }
  }
  if(B->IrqArmed) {
    MmioWrite32(INTR_CFG(mGpio[PIN_IRQ]),B->Pins[PIN_IRQ].IntrCfg);MemoryFence();
    MmioWrite32(INTR_STATUS(mGpio[PIN_IRQ]),0);MemoryFence();
    if(MmioRead32(INTR_CFG(mGpio[PIN_IRQ]))!=B->Pins[PIN_IRQ].IntrCfg && R->Pins==EFI_SUCCESS)R->Pins=EFI_DEVICE_ERROR;
    else B->IrqArmed=FALSE;
  }
  // Output level before function/direction, so a restored output does not glitch
  for(UINTN P=PIANO_POGO_PINS;P>0;--P) {
    PIANO_POGO_PIN *Pin=&B->Pins[P-1];
    if(!Pin->Changed)continue;
    MmioWrite32(IO(Pin->Gpio),Pin->Io&BIT1);MmioWrite32(CTL(Pin->Gpio),Pin->Ctl);MemoryFence();
    if((MmioRead32(CTL(Pin->Gpio))&(CTL_FIELDS|EGPIO_ENABLE))!=(Pin->Ctl&(CTL_FIELDS|EGPIO_ENABLE))) {
      if(R->Pins==EFI_SUCCESS)R->Pins=EFI_DEVICE_ERROR;
    } else {Pin->Changed=FALSE;++R->PinsRestored;}
  }
  B->McuPinsReady=FALSE;
  if(B->Clock!=NULL) {
    EFI_CLOCK_PROTOCOL *Clock=B->Clock;
    for(UINTN I=PIANO_POGO_CLOCKS;I>0;--I)if(B->ClockHeld&(1U<<(I-1))) {
      EFI_STATUS S=Exact(Clock->DisableClock(Clock,B->ClockIds[I-1]));
      if(S==EFI_SUCCESS){B->ClockHeld&=~(1U<<(I-1));++R->ClocksReleased;}else if(R->Clocks==EFI_SUCCESS)R->Clocks=S;
    }
  }
  R->Clean=R->Pins==EFI_SUCCESS && R->Rails==EFI_SUCCESS && R->Clocks==EFI_SUCCESS && !B->ClockHeld && !B->PowerHandle;
  DEBUG((DEBUG_WARN,"PIANO_POGO_BOARD_RELEASE clean=%u pins=%r restored=%u rails=%r votes=%u clocks=%r released=%u\n",
    R->Clean,R->Pins,R->PinsRestored,R->Rails,R->VotesRestored,R->Clocks,R->ClocksReleased));
  return R->Clean?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
