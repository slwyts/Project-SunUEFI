// SPDX-License-Identifier: GPL-2.0-only
// Piano-specific read-only input transport. Register layout references:
// MiCode piano-w-oss 45fb9bd6: spmi-pmic-arb.c, pm8941-pwrkey.c,
// pinctrl-spmi-gpio.c. No PMIC writes, IRQ reconfiguration or native PMIC init.
#include <Uefi.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include "PianoKeysLifecycle.h"

#define ARB_CORE 0x0C400000U
#define ARB_CFG  0x0C42D000U
#define ARB_OBS  0x0C440000U
#define QUEUE_SIZE 16
#define KEY_POWER 1
#define KEY_DOWN  2
#define KEY_UP    4

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mInput;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mPoll;
STATIC EFI_INPUT_KEY mQueue[QUEUE_SIZE];
STATIC UINTN mHead, mTail;
STATIC UINT16 mPonApid, mGpioApid;
STATIC UINT8 mCandidate, mStable, mSamples;
STATIC UINTN mPowerTicks;
STATIC BOOLEAN mFailed;
STATIC BOOLEAN mProductStopRetained;
STATIC BOOLEAN mStandardNavigation,mEscapeChord;
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0x62034B27,0xDA8E,0x4A83,{0x83,0x26,0xB3,0xD4,0x7D,0x4B,0x24,0x25}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {4,0}}
};

STATIC UINT32 Be32 (CONST UINT8 *P) {
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}

STATIC BOOLEAN ExpectedController (CONST VOID *Fdt) {
  // Never guess an EE or use another board's register layout.
  STATIC CONST UINT32 Reg[] = {
    ARB_CFG,0x4000,ARB_CORE,0x3000,0x0C500000,0x400000,
    ARB_OBS,0x80000,0x0C4C0000,0x10000
  };
  INT32 Node=FdtPathOffset(Fdt,"/soc/qcom,spmi@c42d000"),Len;
  CONST UINT8 *P;
  if(Node<0)return FALSE;
  P=FdtGetProp(Fdt,Node,"reg",&Len);
  if(P==NULL || Len!=sizeof(Reg))return FALSE;
  for(UINTN I=0;I<ARRAY_SIZE(Reg);++I)if(Be32(P+4*I)!=Reg[I])return FALSE;
  P=FdtGetProp(Fdt,Node,"qcom,ee",&Len);
  if(P==NULL || Len!=4 || Be32(P)!=0)return FALSE;
  P=FdtGetProp(Fdt,Node,"qcom,bus-id",&Len);
  return P!=NULL && Len==4 && Be32(P)==0;
}

STATIC EFI_STATUS FindApid (UINT16 Ppid,UINTN Count,UINT16 *Result) {
  // v7 permits duplicate PPID entries. Prefer the entry owned by Android's
  // documented EE 0, as the Linux map builder does. Observer reads only.
  BOOLEAN Found=FALSE,Own=FALSE;
  for(UINTN I=0;I<Count;++I) {
    UINT32 Map=MmioRead32(ARB_CORE+0x2000+4*I);
    if(Map==0 || ((Map>>8)&0xFFF)!=Ppid)continue;
    UINT32 Owner=MmioRead32(ARB_CFG+4*I)&7;
    DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_MAP ppid=0x%x apid=%u owner=%u\n",Ppid,(UINT32)I,Owner));
    if(!Found || Owner==0) {*Result=(UINT16)I;Found=TRUE;Own=Owner==0;}
  }
  // Key peripherals must have a matching HLOS owner entry; no fallback to a
  // secure or firmware-owned peripheral merely to keep the UI running.
  return Found && Own ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

STATIC EFI_STATUS ReadKeyByte (UINT16 Address,UINT8 *Value) {
  UINT16 Apid;
  // The public surface cannot address any charger, regulator or storage state.
  if(Address==0x1310)Apid=mPonApid;       // SID 0, PON HLOS realtime state.
  else if(Address==0x8D10)Apid=mGpioApid; // SID 1, GPIO6 realtime state.
  else return EFI_ACCESS_DENIED;
  UINTN Channel=ARB_OBS+0x20*(UINTN)Apid; // EE 0: no EE offset.
  if(Apid>=1024 || Channel+0x1C>=ARB_OBS+0x80000)return EFI_COMPROMISED_DATA;
  // The only MMIO write in this file submits EXT_READL, one byte, through the
  // observer channel. No SPMI EXT_WRITE opcode or write-data FIFO exists here.
  MmioWrite32(Channel,(1U<<27)|((Address&0xFFU)<<4));
  for(UINTN Us=0;Us<1000;++Us) {
    UINT32 Status=MmioRead32(Channel+8);
    if(Status&1) {
      if(Status&0xE) {
        DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_READ_ERROR addr=0x%x status=0x%x\n",Address,Status));
        return EFI_DEVICE_ERROR;
      }
      *Value=(UINT8)MmioRead32(Channel+0x18);
      return EFI_SUCCESS;
    }
    gBS->Stall(1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS Sample (UINT8 *Keys) {
  UINT8 Pon,Gpio;EFI_STATUS Status;
  Status=ReadKeyByte(0x1310,&Pon);if(EFI_ERROR(Status))return Status;
  Status=ReadKeyByte(0x8D10,&Gpio);if(EFI_ERROR(Status))return Status;
  *Keys=((Pon&0x80)?KEY_POWER:0)|((Pon&0x40)?KEY_DOWN:0)|((Gpio&1)?0:KEY_UP);
  return EFI_SUCCESS;
}

STATIC VOID Push (UINT16 Scan,CHAR16 Unicode) {
  UINTN Next=(mTail+1)%QUEUE_SIZE;
  if(Next==mHead)return;
  mQueue[mTail].ScanCode=Scan;mQueue[mTail].UnicodeChar=Unicode;mTail=Next;
  DEBUG((DEBUG_WARN,"SUNUEFI_PHYSICAL_KEY scan=0x%x unicode=0x%x\n",Scan,Unicode));
  gBS->SignalEvent(mInput.WaitForKey);
}

BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable) {
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);BOOLEAN Previous=mStandardNavigation;
  mStandardNavigation=Enable;mEscapeChord=FALSE;mHead=mTail=0;
  gBS->RestoreTPL(Old);return Previous;
}

STATIC VOID EFIAPI Poll (EFI_EVENT Event,VOID *Context) {
  UINT8 Now;EFI_STATUS Status=Sample(&Now);
  if(EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_POLL_STOP %r\n",Status));
    mFailed=TRUE;gBS->SetTimer(mPoll,TimerCancel,0);return;
  }
  if(mStable&KEY_POWER)++mPowerTicks;
  if(Now!=mCandidate) {mCandidate=Now;mSamples=1;return;}
  if(mSamples<2)++mSamples;
  if(mSamples<2 || Now==mStable)return;
  UINT8 Pressed=Now&~mStable,Released=mStable&~Now;
  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_STATE previous=%u current=%u\n",mStable,Now));
  if(mStandardNavigation && (Now&(KEY_UP|KEY_DOWN))==(KEY_UP|KEY_DOWN)) {
    if(!mEscapeChord)Push(SCAN_ESC,0);
    mEscapeChord=TRUE;
  } else if(mStandardNavigation && mEscapeChord) {
    // Do not emit a direction when releasing half of the Escape chord.
    if(!(Now&(KEY_UP|KEY_DOWN)))mEscapeChord=FALSE;
  } else {
    if(Pressed&KEY_UP)Push(mStandardNavigation?SCAN_UP:SCAN_VOLUME_UP,0);
    if(Pressed&KEY_DOWN)Push(mStandardNavigation?SCAN_DOWN:SCAN_VOLUME_DOWN,0);
  }
  if(Pressed&KEY_POWER)mPowerTicks=0;
  // Emit Enter on a short power-key release. Holding power remains available
  // for hardware recovery without a menu activation just before reset.
  if((Released&KEY_POWER) && mPowerTicks<50 && !mEscapeChord)Push(0,CHAR_CARRIAGE_RETURN);
  mStable=Now;
}

STATIC EFI_STATUS EFIAPI Reset (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,BOOLEAN Extended) {
  if(This!=&mInput)return EFI_INVALID_PARAMETER;
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);mHead=mTail=0;gBS->RestoreTPL(Old);
  return mFailed ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Read (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key) {
  if(This!=&mInput || Key==NULL)return EFI_INVALID_PARAMETER;
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS Status=mFailed?EFI_DEVICE_ERROR:EFI_NOT_READY;
  if(mHead!=mTail) {*Key=mQueue[mHead];mHead=(mHead+1)%QUEUE_SIZE;Status=EFI_SUCCESS;}
  gBS->RestoreTPL(Old);return Status;
}

STATIC VOID EFIAPI Wait (EFI_EVENT Event,VOID *Context) {
  if(mHead!=mTail)gBS->SignalEvent(Event);
}

VOID PianoStopKeys (VOID) {
  if(mPoll!=NULL) {gBS->SetTimer(mPoll,TimerCancel,0);gBS->CloseEvent(mPoll);mPoll=NULL;}
  if(mHandle!=NULL) {
    gBS->DisconnectController(mHandle,NULL,NULL);
    gBS->UninstallMultipleProtocolInterfaces(mHandle,&gEfiSimpleTextInProtocolGuid,&mInput,
      &gEfiDevicePathProtocolGuid,&mPath,NULL);mHandle=NULL;
  }
  if(mInput.WaitForKey!=NULL) {gBS->CloseEvent(mInput.WaitForKey);mInput.WaitForKey=NULL;}
}
EFI_STATUS PianoStopKeysForProduct(PIANO_KEYS_RETIRE_REPORT *Report) {
  if(Report==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Report,sizeof(*Report));Report->Started=TRUE;
  if(mProductStopRetained)return Report->Status=EFI_ACCESS_DENIED;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  if(Old!=TPL_APPLICATION)return Report->Status=EFI_UNSUPPORTED;
  EFI_STATUS Status=EFI_SUCCESS;
  if(mPoll!=NULL) {
    Status=Report->TimerCancel=gBS->SetTimer(mPoll,TimerCancel,0);if(Status!=EFI_SUCCESS)goto Failed;
    Status=Report->TimerClose=gBS->CloseEvent(mPoll);if(Status!=EFI_SUCCESS)goto Failed;mPoll=NULL;
  }
  if(mHandle!=NULL) {
    Status=Report->Disconnect=gBS->DisconnectController(mHandle,NULL,NULL);if(Status!=EFI_SUCCESS)goto Failed;
    Status=Report->Uninstall=gBS->UninstallMultipleProtocolInterfaces(mHandle,&gEfiSimpleTextInProtocolGuid,&mInput,
      &gEfiDevicePathProtocolGuid,&mPath,NULL);if(Status!=EFI_SUCCESS)goto Failed;mHandle=NULL;
  }
  if(mInput.WaitForKey!=NULL) {
    Status=Report->WaitClose=gBS->CloseEvent(mInput.WaitForKey);if(Status!=EFI_SUCCESS)goto Failed;mInput.WaitForKey=NULL;
  }
  Report->Returned=Report->Clean=TRUE;Report->Status=EFI_SUCCESS;return EFI_SUCCESS;
Failed:
  mProductStopRetained=TRUE;Report->Returned=Report->Retained=TRUE;
  return Report->Status=EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}

EFI_STATUS PianoStartKeys (CONST VOID *Fdt) {
  EFI_STATUS Status;
  if(!ExpectedController(Fdt))return EFI_UNSUPPORTED;
  UINT32 Version=MmioRead32(ARB_CORE),Count=MmioRead32(ARB_CORE+4)&0x7FF;
  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_CONTROLLER version=0x%x apids=%u ee=0 readonly=1\n",Version,Count));
  if(Version<0x70000000 || Version>=0x80000000 || Count==0 || Count>1024)return EFI_UNSUPPORTED;
  Status=FindApid(0x013,Count,&mPonApid);if(EFI_ERROR(Status))return Status;
  Status=FindApid(0x18D,Count,&mGpioApid);if(EFI_ERROR(Status))return Status;
  Status=Sample(&mStable);if(EFI_ERROR(Status))return Status;
  mCandidate=mStable;mSamples=2;mHead=mTail=0;mPowerTicks=0;mFailed=FALSE;
  mStandardNavigation=mEscapeChord=FALSE;
  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_INITIAL state=%u pon_apid=%u gpio_apid=%u\n",mStable,mPonApid,mGpioApid));
  mInput.Reset=Reset;mInput.ReadKeyStroke=Read;
  Status=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,Wait,NULL,&mInput.WaitForKey);
  if(EFI_ERROR(Status))return Status;
  Status=gBS->InstallMultipleProtocolInterfaces(&mHandle,&gEfiSimpleTextInProtocolGuid,&mInput,
    &gEfiDevicePathProtocolGuid,&mPath,NULL);
  if(EFI_ERROR(Status)) {PianoStopKeys();return Status;}
  Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,NULL,&mPoll);
  if(!EFI_ERROR(Status))Status=gBS->SetTimer(mPoll,TimerPeriodic,20ULL*10000ULL);
  if(EFI_ERROR(Status)) {PianoStopKeys();return Status;}
  Status=gBS->ConnectController(mHandle,NULL,NULL,TRUE);
  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_CONNECT %r\n",Status));
  return EFI_SUCCESS;
}

#ifdef PIANO_USB_POWER_PROBE
STATIC EFI_STATUS RevisionApid(UINT16 Ppid,UINTN Count,UINT16 *Result) {
  // MiCode pmic_arb_offset_v7 checks write_ee only for CHANNEL_RW. The
  // CHANNEL_OBS path reads through the calling EE and lets hardware enforce
  // read permissions. Apply that rule only to the fixed revision registers.
  BOOLEAN Found=FALSE;
  for(UINTN I=0;I<Count;++I) {
    UINT32 Map=MmioRead32(ARB_CORE+0x2000+4*I);
    if(Map==0 || ((Map>>8)&0xFFF)!=Ppid)continue;
    UINT32 Owner=MmioRead32(ARB_CFG+4*I)&7;
    DEBUG((DEBUG_WARN,"SUNUEFI_PMIC_OBSERVER_MAP ppid=%03x apid=%u write_owner=%u read_ee=0\n",Ppid,(UINT32)I,Owner));
    if(!Found || Owner==0){*Result=(UINT16)I;Found=TRUE;}
    if(Owner==0)break;
  }
  return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS UsbPowerRead(UINT8 Sid,UINT16 Address,UINT8 *Value,UINTN Bytes,UINTN Count) {
  BOOLEAN Revision=(Sid==0 || Sid==2 || Sid==4 || Sid==6 || Sid==7) && Address==0x101 && Bytes==5;
  BOOLEAN Repeater=Sid==7 && Bytes==1 &&
    (Address==0xFD08 || Address==0xFD46 || Address==0xFD51 || Address==0xFD54 || Address==0xFD55 || Address==0xFD57);
  if((!Revision && !Repeater) || Value==NULL || Count==0 || Count>1024)return EFI_ACCESS_DENIED;
  UINT16 Apid=0,Ppid=((UINT16)Sid<<8)|(Address>>8);
  BOOLEAN FixedStatus=Sid==7 && (Address==0xFD08 || Address==0xFD46);
  EFI_STATUS Status=(Revision || FixedStatus)?RevisionApid(Ppid,Count,&Apid):FindApid(Ppid,Count,&Apid);
  if(EFI_ERROR(Status))return Status;
  UINTN Channel=ARB_OBS+0x20*(UINTN)Apid;
  // Only EXT_READL through EE0 observer. No write-data FIFO or RW-channel
  // access, including PMIC version discovery and all USB tuning registers.
  MmioWrite32(Channel,(1U<<27)|((Address&0xFFU)<<4)|(UINT32)(Bytes-1));
  for(UINTN Us=0;Us<1000;++Us) {
    UINT32 State=MmioRead32(Channel+8);
    if(State&1) {
      if(State&0xE)return EFI_DEVICE_ERROR;
      UINT32 Low=MmioRead32(Channel+0x18),High=Bytes>4?MmioRead32(Channel+0x1C):0;
      for(UINTN I=0;I<Bytes;++I)Value[I]=(UINT8)((I<4?Low:High)>>((I&3)*8));
      return EFI_SUCCESS;
    }
    gBS->Stall(1);
  }
  return EFI_TIMEOUT;
}
VOID PianoProbeUsbPower(CONST VOID *Fdt) {
  if(!ExpectedController(Fdt)){DEBUG((DEBUG_WARN,"SUNUEFI_USB_POWER_DT_REJECTED\n"));return;}
  UINT32 Version=MmioRead32(ARB_CORE),Count=MmioRead32(ARB_CORE+4)&0x7FF;
  if(Version<0x70000000 || Version>=0x80000000 || Count==0 || Count>1024)return;
  STATIC CONST UINT8 Sids[]={0,2,4,6,7};
  for(UINTN I=0;I<ARRAY_SIZE(Sids);++I) {
    UINT8 Data[5]={0};EFI_STATUS Status=UsbPowerRead(Sids[I],0x101,Data,sizeof(Data),Count);
    DEBUG((DEBUG_WARN,"SUNUEFI_PMIC_REV sid=%u status=%r rev2=%02x minor=%02x major=%02x type=%02x subtype=%02x\n",
      Sids[I],Status,Data[0],Data[1],Data[2],Data[3],Data[4]));
  }
  STATIC CONST UINT16 Registers[]={0xFD08,0xFD46,0xFD51,0xFD54,0xFD55,0xFD57};
  for(UINTN I=0;I<ARRAY_SIZE(Registers);++I) {
    UINT8 Value=0;EFI_STATUS Status=UsbPowerRead(7,Registers[I],&Value,1,Count);
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_REPEATER sid=7 addr=%04x value=%02x status=%r readonly=1\n",Registers[I],Value,Status));
  }
}
#endif
