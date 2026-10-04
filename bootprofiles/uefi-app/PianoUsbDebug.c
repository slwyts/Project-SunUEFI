// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm EFI_USB_DEVICE_PROTOCOL transport for the RAM-only fastboot server.
// Queue replies asynchronously: event handlers never wait for a USB host.
#include "PianoFastboot.h"
// The pinned Qualcomm header predates MdePkg's BOS/IAD additions. Keep its
// wire types under distinct names without editing either upstream header.
#include <IndustryStandard/Usb.h>
#define USB_BOS_DESCRIPTOR QCOM_USB_BOS_DESCRIPTOR
#define USB_INTERFACE_ASSOCIATION_DESCRIPTOR QCOM_USB_INTERFACE_ASSOCIATION_DESCRIPTOR
#define USB_DESC_TYPE_INTERFACE_ASSOCIATION QCOM_USB_DESC_TYPE_INTERFACE_ASSOCIATION
#define USB_DESC_TYPE_BOS QCOM_USB_DESC_TYPE_BOS
#include <Protocol/EFIUsbDevice.h>
#undef USB_BOS_DESCRIPTOR
#undef USB_INTERFACE_ASSOCIATION_DESCRIPTOR
#undef USB_DESC_TYPE_INTERFACE_ASSOCIATION
#undef USB_DESC_TYPE_BOS
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>

#define RX_BYTES  65536U
#define QUEUE_COUNT  256U
typedef struct { UINTN Bytes; UINT8 Data[64]; } REPLY;
STATIC EFI_USB_DEVICE_PROTOCOL *mUsb;
STATIC EFI_EVENT mPoll;
STATIC PIANO_FASTBOOT mState;
STATIC REPLY mQueue[QUEUE_COUNT];
STATIC UINTN mHead,mCount;
STATIC VOID *mRx,*mTx;
STATIC UINTN mRxRequest;
STATIC BOOLEAN mStarted,mConnected,mRxPending,mTxPending,mBusy;
STATIC EFI_GUID mUsbDevice={0xD9D9CE48,0x44B8,0x4F49,{0x8E,0x3E,0x2A,0x3B,0x92,0x7D,0xC6,0xC1}};
STATIC EFI_GUID mUsbfn={0x32D2963A,0xFE5D,0x4F30,{0xB6,0x33,0x6E,0x5D,0xC5,0x58,0x03,0xCC}};

#pragma pack(1)
typedef struct {
  USB_CONFIG_DESCRIPTOR Config;
  USB_INTERFACE_DESCRIPTOR Interface;
  USB_ENDPOINT_DESCRIPTOR In,Out;
} FB_DESCRIPTORS;
#pragma pack()
STATIC USB_DEVICE_DESCRIPTOR mDevice={18,1,0x0200,0,0,0,64,0x18D1,0xD00D,0x0100,0,0,1,1};
STATIC FB_DESCRIPTORS mDescriptors={
  {9,2,sizeof(FB_DESCRIPTORS),1,1,0,0x80,250},
  {9,4,0,0,2,0xff,0x42,0x03,0},
  {7,5,0x81,2,512,0}, {7,5,0x01,2,512,0}
};
STATIC USB_DEVICE_QUALIFIER_DESCRIPTOR mQualifier={10,6,0x0200,0,0,0,64,1,0};
typedef struct { UINT8 Length,Type; CHAR16 Text[13]; } SERIAL_DESCRIPTOR;
STATIC UINT16 mLanguages[2]={0x0304,0x0409};
STATIC SERIAL_DESCRIPTOR mSerial={28,3,{'S','u','n','U','E','F','I','-','p','i','a','n','o'}};

STATIC EFI_STATUS Enqueue(VOID *Context,CONST VOID *Data,UINTN Bytes) {
  (VOID)Context;
  if(Data==NULL || Bytes>64 || Bytes<4)return EFI_INVALID_PARAMETER;
  if(mCount==QUEUE_COUNT)return EFI_OUT_OF_RESOURCES;
  REPLY *R=&mQueue[(mHead+mCount)%QUEUE_COUNT];
  CopyMem(R->Data,Data,Bytes);R->Bytes=Bytes;++mCount;return EFI_SUCCESS;
}
STATIC EFI_STATUS LogConsole(CONST volatile UINT32 *Header,VOID *Context,PIANO_FB_SEND Send) {
  // Snapshot before enqueueing; other serial writers may advance this ring.
  CONST UINTN Capacity=0x200000-12;
  UINT32 Signature=Header[0],Start=Header[1],Size=Header[2];
  if(Signature!=0x43474244 || Start>=Capacity || Size>Capacity)
    return Send(Context,"INFOinvalid RAM console",23);
  UINTN Bytes=Size;
  UINT8 *Snapshot=AllocatePool(Bytes?Bytes:1);
  if(Snapshot==NULL)return EFI_OUT_OF_RESOURCES;
  CONST UINT8 *Ring=(CONST UINT8 *)Header+12;
  UINTN Offset=(Start+Capacity-Bytes)%Capacity;
  for(UINTN I=0;I<Bytes;++I)Snapshot[I]=Ring[(Offset+I)%Capacity];
  // Send this UEFI session only. PNG base64 lines would otherwise crowd out
  // the useful boot diagnostics and older Android logs are unrelated here.
  STATIC CONST CHAR8 Marker[]="SUNUEFI_RAMLOG_BEGIN";
  STATIC CONST CHAR8 Png[]="SUNUEFI_PNG_";
  UINTN Begin=Bytes;
  for(UINTN I=0;I+sizeof(Marker)-1<=Bytes;++I)
    if(!CompareMem(Snapshot+I,Marker,sizeof(Marker)-1))Begin=I;
  if(Begin==Bytes) {
    STATIC CONST CHAR8 Missing[]="INFOno current UEFI log marker";
    FreePool(Snapshot);return Send(Context,Missing,sizeof(Missing)-1);
  }
  EFI_STATUS Status=EFI_SUCCESS;CHAR8 Line[64]="INFO";UINTN Used=4;
  for(UINTN I=Begin;I<Bytes;++I) {
    if(mCount>=QUEUE_COUNT-2) {
      Status=Send(Context,"INFOlog tail truncated",22);Used=4;break;
    }
    if((I==Begin || Snapshot[I-1]=='\n') && I+sizeof(Png)-1<=Bytes &&
       !CompareMem(Snapshot+I,Png,sizeof(Png)-1)) {
      while(I<Bytes && Snapshot[I]!='\n')++I;
      continue;
    }
    UINT8 C=Snapshot[I];
    if(C=='\r')continue;
    if(C=='\n' || Used==64) {
      if(Used>4){Status=Send(Context,Line,Used);if(EFI_ERROR(Status))break;}
      Used=4;if(C=='\n')continue;
    }
    Line[Used++]=(C>=32 && C<=126)?C:'.';
  }
  if(!EFI_ERROR(Status) && Used>4)Status=Send(Context,Line,Used);
  FreePool(Snapshot);return Status;
}
STATIC EFI_STATUS LogTail(VOID *Context,PIANO_FB_SEND Send) {
  // The command surface exposes only this fixed reserved console region.
  return LogConsole((CONST volatile UINT32 *)(UINTN)0xA3500000,Context,Send);
}
STATIC EFI_STATUS ValidateNativeAbi(VOID) {
  // This native module is hash-verified by prepare_native_probe.py. Reject
  // unrelated implementations rather than assuming their callback ABI.
  STATIC CONST UINTN Rvas[]={0x156c,0x16dc,0x1764,0x1ab8,0x1b3c,0x1b94,0x1c6c,0x1cc8,0x1d28};
  EFI_HANDLE *Handles=NULL;UINTN Count=0;EFI_STATUS Status;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status))return Status;
  Status=EFI_SECURITY_VIOLATION;
  for(UINTN I=0;I<Count;++I) {
    EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image)))continue;
    UINTN Base=(UINTN)Image->ImageBase;
    if((UINTN)mUsb!=Base+0xb0f8 || Image->ImageSize<0xb148)continue;
    if(mUsb->Revision!=0x10000001)break;
    UINTN *Callbacks=(UINTN *)((UINT8 *)mUsb+sizeof(UINTN));BOOLEAN Match=TRUE;
    for(UINTN J=0;J<ARRAY_SIZE(Rvas);++J)if(Callbacks[J]!=Base+Rvas[J])Match=FALSE;
    if(Match){Status=EFI_SUCCESS;DEBUG((DEBUG_WARN,"SUNUEFI_USB_ABI_OK base=0x%lx\n",Base));}
    break;
  }
  FreePool(Handles);return Status;
}
STATIC EFI_STATUS Disconnect(VOID) {
  mConnected=FALSE;
  EFI_STATUS RxStatus=mRxPending?mUsb->AbortXfer(1):EFI_SUCCESS;
  EFI_STATUS TxStatus=mTxPending?mUsb->AbortXfer(0x81):EFI_SUCCESS;
  if(EFI_ERROR(RxStatus) || EFI_ERROR(TxStatus))return EFI_DEVICE_ERROR;
  mRxPending=FALSE;mTxPending=FALSE;mHead=0;mCount=0;
  PianoFastbootReset(&mState);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PostRx(VOID) {
  if(!mConnected || mRxPending || mCount || mTxPending)return EFI_SUCCESS;
  mRxRequest=mState.Receiving?MIN((UINTN)RX_BYTES,mState.Expected-mState.Received):64;
  EFI_STATUS Status=mUsb->Send(1,mRxRequest,mRx);
  if(!EFI_ERROR(Status))mRxPending=TRUE;
  return Status;
}
STATIC EFI_STATUS PostTx(VOID) {
  if(!mConnected || mTxPending || mCount==0)return EFI_SUCCESS;
  REPLY *R=&mQueue[mHead];CopyMem(mTx,R->Data,R->Bytes);
  EFI_STATUS Status=mUsb->Send(0x81,R->Bytes,mTx);
  if(!EFI_ERROR(Status))mTxPending=TRUE;
  return Status;
}
STATIC VOID EFIAPI Poll(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(mBusy || !mStarted)return;
  mBusy=TRUE;EFI_STATUS Status=EFI_SUCCESS;
  for(UINTN I=0;I<8;++I) {
    USB_DEVICE_EVENT Kind=UsbDeviceEventNoEvent;USB_DEVICE_EVENT_DATA Data;
    UINTN Size=sizeof(Data);ZeroMem(&Data,sizeof(Data));
    Status=mUsb->HandleEvent(&Kind,&Size,&Data);
    if(EFI_ERROR(Status) || Kind==UsbDeviceEventNoEvent)break;
    if(Size>sizeof(Data)){Status=EFI_COMPROMISED_DATA;break;}
    if(Kind==UsbDeviceEventDeviceStateChange) {
      if(Size<sizeof(Data.DeviceState)){Status=EFI_COMPROMISED_DATA;break;}
      DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONNECTION state=%u\n",Data.DeviceState));
      if(Data.DeviceState==UsbDeviceStateConnected)mConnected=TRUE;
      else {Status=Disconnect();if(EFI_ERROR(Status))break;}
    } else if(Kind==UsbDeviceEventTransferNotification) {
      USB_DEVICE_TRANSFER_OUTCOME *T=&Data.TransferOutcome;
      if(Size<sizeof(*T)){Status=EFI_COMPROMISED_DATA;break;}
      if(T->Status==UsbDeviceTransferStatusCancelled &&
         ((T->DataBuffer==mRx && !mRxPending) || (T->DataBuffer==mTx && !mTxPending)))continue;
      if(T->DataBuffer==mTx && T->EndpointIndex==0x81 && mTxPending) {
        mTxPending=FALSE;
        if(T->Status!=UsbDeviceTransferStatusCompleteOK || T->BytesCompleted!=mQueue[mHead].Bytes) {
          Status=EFI_DEVICE_ERROR;break;
        }
        mHead=(mHead+1)%QUEUE_COUNT;--mCount;
      } else if(T->DataBuffer==mRx && T->EndpointIndex==1 && mRxPending) {
        mRxPending=FALSE;
        if(T->Status!=UsbDeviceTransferStatusCompleteOK || T->BytesCompleted>mRxRequest) {
          Status=EFI_DEVICE_ERROR;break;
        }
        Status=PianoFastbootPacket(&mState,mRx,T->BytesCompleted);
        if(EFI_ERROR(Status))break;
      } else {Status=EFI_COMPROMISED_DATA;break;}
    }
  }
  if(Status==EFI_NOT_READY)Status=EFI_SUCCESS;
  if(!EFI_ERROR(Status))Status=PostTx();
  if(!EFI_ERROR(Status))Status=PostRx();
  BOOLEAN Reboot=mState.RebootRequested && !mCount && !mTxPending;
  BOOLEAN Exit=mState.ExitRequested && !mCount && !mTxPending;
  mBusy=FALSE;
  if(EFI_ERROR(Status) && Status!=EFI_NOT_READY) {
    DEBUG((DEBUG_WARN,"SUNUEFI_USB_POLL_ERROR %r\n",Status));PianoStopUsbDebug();
  } else if(Reboot) {
    PianoStopUsbDebug();gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
  } else if(Exit)PianoStopUsbDebug();
}
VOID PianoStopUsbDebug(VOID) {
  if(mPoll!=NULL){gBS->SetTimer(mPoll,TimerCancel,0);gBS->CloseEvent(mPoll);mPoll=NULL;}
  if(mUsb!=NULL) {
    EFI_STATUS RxStatus=mRxPending?mUsb->AbortXfer(1):EFI_SUCCESS;
    EFI_STATUS TxStatus=mTxPending?mUsb->AbortXfer(0x81):EFI_SUCCESS;
    // A failed abort may leave DMA owning its transfer buffer. Retain that
    // allocation until the imminent diagnostic reboot instead of freeing it.
    if(mRx!=NULL && !EFI_ERROR(RxStatus))mUsb->FreeTransferBuffer(mRx);
    if(mTx!=NULL && !EFI_ERROR(TxStatus))mUsb->FreeTransferBuffer(mTx);
    if(mStarted)mUsb->Stop();
  }
  PianoFastbootReset(&mState);
  mUsb=NULL;mRx=NULL;mTx=NULL;mStarted=FALSE;mConnected=FALSE;
  mRxPending=FALSE;mTxPending=FALSE;mHead=0;mCount=0;mBusy=FALSE;
}
EFI_STATUS PianoStartUsbDebug(VOID) {
  VOID *Interface=NULL;EFI_STATUS Status;
  Status=gBS->LocateProtocol(&mUsbDevice,NULL,(VOID **)&mUsb);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEVICE_PROTOCOL %r\n",Status));
  if(EFI_ERROR(Status)){mUsb=NULL;return Status;}
  Status=ValidateNativeAbi();if(EFI_ERROR(Status)){mUsb=NULL;return Status;}
  Status=gBS->LocateProtocol(&mUsbfn,NULL,&Interface);
  DEBUG((DEBUG_WARN,"SUNUEFI_USBFN_IO_PROTOCOL %r\n",Status));
  if(EFI_ERROR(Status)){mUsb=NULL;return Status;}
  VOID *Configurations[]={&mDescriptors};
  USB_STRING_DESCRIPTOR *Strings[]={(USB_STRING_DESCRIPTOR *)mLanguages,(USB_STRING_DESCRIPTOR *)&mSerial};
  Status=mUsb->Start(&mDevice,Configurations,&mQualifier,NULL,2,Strings);
  DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_START %r\n",Status));
  if(EFI_ERROR(Status)){mUsb=NULL;return Status;}
  mStarted=TRUE;
  Status=mUsb->AllocateTransferBuffer(RX_BYTES,&mRx);
  if(!EFI_ERROR(Status))Status=mUsb->AllocateTransferBuffer(64,&mTx);
  if(!EFI_ERROR(Status))Status=PianoFastbootInit(&mState,NULL,Enqueue,LogTail);
  if(!EFI_ERROR(Status))Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,NULL,&mPoll);
  if(!EFI_ERROR(Status))Status=gBS->SetTimer(mPoll,TimerPeriodic,100000);
  if(EFI_ERROR(Status)){PianoStopUsbDebug();return Status;}
  DEBUG((DEBUG_WARN,"SUNUEFI_FASTBOOT_READY vid=18d1 pid=d00d serial=SunUEFI-piano policy=RAM-only\n"));
  return EFI_SUCCESS;
}
