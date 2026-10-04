// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the actual firmware transport with a USB protocol mock.
#define main fastboot_command_tests
#include "test_fastboot.c"
#undef main
#include "../bootprofiles/uefi-app/PianoUsbDebug.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI AllocatePool(UINTN N){return malloc(N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_BOOT_SERVICES bs;static EFI_USB_DEVICE_PROTOCOL usb;
static UINT8 rx[RX_BYTES],tx[64];
static USB_DEVICE_EVENT next_kind;static USB_DEVICE_EVENT_DATA next_data;
static UINTN next_size,posted_rx,posted_tx;static char transmitted[65];
static unsigned aborted,stopped,freed;static int transfer_fail,abort_fail,no_events_not_ready;
static EFI_STATUS EFIAPI event(USB_DEVICE_EVENT *Kind,UINTN *Size,USB_DEVICE_EVENT_DATA *Data){
  *Kind=next_kind;*Size=next_size;*Data=next_data;next_kind=UsbDeviceEventNoEvent;
  return no_events_not_ready && *Kind==UsbDeviceEventNoEvent?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS send_usb(UINT8 Ep,UINTN N,VOID *Buffer){
  if(transfer_fail)return EFI_DEVICE_ERROR;
  if(Ep==1){assert(Buffer==rx && N<=RX_BYTES && !mRxPending);posted_rx=N;}
  else {assert(Ep==0x81 && Buffer==tx && N<=64 && !mTxPending);
    posted_tx=N;memcpy(transmitted,Buffer,N);transmitted[N]=0;}
  return EFI_SUCCESS;
}
static EFI_STATUS abort_usb(UINT8 Ep){assert(Ep==1 || Ep==0x81);++aborted;return abort_fail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS stop_usb(VOID){++stopped;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI free_buffer(VOID *Buffer){assert(Buffer==rx || Buffer==tx);++freed;return EFI_SUCCESS;}
static void setup(void){
  usb.Send=send_usb;usb.HandleEvent=event;usb.AbortXfer=abort_usb;
  usb.FreeTransferBuffer=free_buffer;usb.Stop=stop_usb;
  gBS=&bs;mUsb=&usb;mRx=rx;mTx=tx;mStarted=TRUE;mConnected=FALSE;
  mRxPending=mTxPending=mBusy=FALSE;mHead=mCount=0;next_kind=UsbDeviceEventNoEvent;
  assert(PianoFastbootInit(&mState,NULL,Enqueue,NULL)==EFI_SUCCESS);
}
static void connect(int Connected){
  next_kind=UsbDeviceEventDeviceStateChange;next_size=sizeof(next_data.DeviceState);
  next_data.DeviceState=Connected?UsbDeviceStateConnected:UsbDeviceStateDisconnected;
  Poll(NULL,NULL);
}
static void complete(UINT8 Ep,UINTN N,USB_DEVICE_TRANSFER_STATUS Status){
  next_kind=UsbDeviceEventTransferNotification;next_size=sizeof(next_data.TransferOutcome);
  next_data.TransferOutcome=(USB_DEVICE_TRANSFER_OUTCOME){Status,Ep,N,Ep==1?(void *)rx:(void *)tx};
  Poll(NULL,NULL);
}
static void receive(const char *Data,UINTN N){assert(mRxPending && N<=posted_rx);
  memcpy(rx,Data,N);complete(1,N,UsbDeviceTransferStatusCompleteOK);}
int main(void){
  assert(fastboot_command_tests()==0);
  fail_alloc=0;fail_send=0;
  setup();connect(1);assert(mRxPending && posted_rx==64 && !mTxPending);
  receive("getvar:version",14);assert(mTxPending && strcmp(transmitted,"OKAY0.4")==0);
  assert(!mRxPending);complete(0x81,posted_tx,UsbDeviceTransferStatusCompleteOK);
  assert(mCount==0 && mRxPending && posted_rx==64);
  receive("download:00000003",17);assert(strcmp(transmitted,"DATA00000003")==0);
  complete(0x81,posted_tx,UsbDeviceTransferStatusCompleteOK);assert(posted_rx==3);
  receive("a",1);assert(!mTxPending && mRxPending && posted_rx==2);
  receive("bc",2);assert(mState.Complete && strcmp(transmitted,"OKAY")==0);
  complete(0x81,posted_tx,UsbDeviceTransferStatusCompleteOK);
  connect(0);assert(!mConnected && !allocation && !mRxPending && mCount==0);
  complete(1,0,UsbDeviceTransferStatusCancelled);assert(mStarted);
  connect(1);assert(mRxPending && posted_rx==64);
  receive("erase:userdata",14);assert(strcmp(transmitted,"FAILcommand disabled by RAM-only policy")==0);
  complete(0x81,posted_tx,UsbDeviceTransferStatusCompleteOK);
  // Fill the bounded queue, prove rejection does not overwrite queued replies.
  while(mCount<QUEUE_COUNT)assert(Enqueue(NULL,"INFOx",5)==EFI_SUCCESS);
  assert(Enqueue(NULL,"INFOy",5)==EFI_OUT_OF_RESOURCES);
  mHead=0;mCount=0;
  // A malicious controller notification cannot make the parser read past RX.
  complete(1,65,UsbDeviceTransferStatusCompleteOK);
  assert(!mStarted && stopped==1 && freed==2 && mUsb==NULL);
  setup();connect(1);transfer_fail=1;
  receive("getvar:version",14);assert(!mStarted && stopped==2 && mPoll==NULL);
  transfer_fail=0;setup();no_events_not_ready=1;connect(1);
  assert(mRxPending && posted_rx==64);abort_fail=1;
  unsigned before=freed;connect(0);
  assert(!mStarted && stopped==3 && freed==before+1); // Unsafe RX retained, idle TX freed.
  // Real log exporter: ring wrap, newest session and screenshot filtering.
  void *ring=calloc(1,0x200000);assert(ring);
  const char *text="older Android\nSUNUEFI_RAMLOG_BEGIN\nold session\nSUNUEFI_RAMLOG_BEGIN\nUSB useful\nSUNUEFI_PNG_DATA ignored\nfinal line\n";
  size_t cap=0x200000-12,n=strlen(text),offset=cap-10;
  UINT32 *header=ring;header[0]=0x43474244;header[1]=(offset+n)%cap;header[2]=n;
  for(size_t i=0;i<n;++i)((char *)ring)[12+(offset+i)%cap]=text[i];
  mHead=mCount=0;assert(LogConsole(ring,NULL,Enqueue)==EFI_SUCCESS && mCount==3);
  assert(mQueue[0].Bytes==24 && memcmp(mQueue[0].Data,"INFOSUNUEFI_RAMLOG_BEGIN",24)==0);
  assert(mQueue[1].Bytes==14 && memcmp(mQueue[1].Data,"INFOUSB useful",14)==0);
  assert(mQueue[2].Bytes==14 && memcmp(mQueue[2].Data,"INFOfinal line",14)==0);
  free(ring);
  puts("USB asynchronous IN/OUT, download framing, unplug/reconnect, queue limits and invalid completion cleanup passed.");
  return 0;
}
