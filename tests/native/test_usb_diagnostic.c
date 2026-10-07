// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real firmware EP0 + console snapshot, backed only by a private host mapping.
#include <sys/mman.h>
#define main session_suite_main
#include "test_usb_session.c"
#undef main
static UINT32 get32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static EFI_STATUS request(UINT8 Op,UINT16 Page,UINT16 Length,UINT8 *Data,UINTN *Bytes) {
  UINT8 u[8]={0xC0,0x5B,Op,0,(UINT8)Page,(UINT8)(Page>>8),(UINT8)Length,(UINT8)(Length>>8)};
  PIANO_USB_CONTROL_ACTION action;
  return DiagnosticSetup(u,Data,4096,Bytes,&action);
}
static UINTN exchange(UINT8 Request,UINT8 Op,UINT16 Page,UINT16 Length,UINT8 *Reply) {
  UINT8 u[8]={0xC0,Request,Op,0,(UINT8)Page,(UINT8)(Page>>8),(UINT8)Length,(UINT8)(Length>>8)};
  assert(mPending[0] && mPhase==0);CopyMem(mSetup.Cpu,u,8);
  DWC_TRB *trb=mTrbs[0].Cpu;trb->Size=0;trb->Control&=~BIT0;
  assert(Event(0xC040)==EFI_SUCCESS && mPhase==1 && mPending[1]);
  trb=mTrbs[1].Cpu;UINTN size=trb->Size;
  assert(((trb->Control>>4)&63)==5 && mPayload[1]==&mTx && mTx.Active);
  assert(trb->Low==(UINT32)mTx.DeviceAddress);CopyMem(Reply,mTx.Cpu,size);
  trb->Size=0;trb->Control&=~BIT0;assert(Event(0xC042)==EFI_SUCCESS && mPhase==2 && !mTx.Active);
  assert(Event(0x20C0)==EFI_SUCCESS && mPhase==3 && mPending[0]);
  trb=mTrbs[0].Cpu;assert(!trb->Size && ((trb->Control>>4)&63)==4);
  trb->Control&=~BIT0;assert(Event(0xC040)==EFI_SUCCESS && mPhase==0 && mPending[0]);
  return size;
}
static VOID large_console(UINT32 *Header,UINTN Cap){
  assert(USB_DIAG_LOG_BYTES==262144U&&USB_DIAG_PAGE_BYTES==512U&&USB_DIAG_LOG_BYTES/USB_DIAG_PAGE_BYTES==512);
  UINT8 *Ring=(UINT8 *)(Header+3),*Expected=malloc(USB_DIAG_LOG_BYTES);assert(Expected);UINT8 Data[4096];UINTN Bytes=0;
  // More than the old64KiB limit, without truncation or session-marker trim.
  UINT32 Size=131113;Header[0]=0x43474244;Header[1]=Header[2]=Size;memset(Ring,'Q',Cap);
  assert(request(1,0,24,Data,&Bytes)==EFI_SUCCESS&&get32(Data+12)==Size&&mLogBytes==Size&&!mLogFlags);
  memset(Expected,'Q',Size);assert(get32(Data+16)==DiagCrc(Expected,Size)&&!memcmp(mLogSnapshot,Expected,Size));
  UINT32 Generation=mLogGeneration;memset(Ring,'W',Cap);assert(!memcmp(mLogSnapshot,Expected,Size)&&mLogGeneration==Generation);
  assert(request(2,256,65,Data,&Bytes)==EFI_SUCCESS&&get32(Data+12)==131072&&DiagLe16(Data+16)==41&&Bytes==65);
  // Full wrapped ring: the saved bytes are exactly its latest256KiB tail.
  for(UINTN I=0;I<Cap;++I)Ring[I]=(UINT8)('a'+I%23);
  Header[1]=1637;Header[2]=(UINT32)Cap;UINTN Offset=(Header[1]+Cap-USB_DIAG_LOG_BYTES)%Cap;
  for(UINTN I=0;I<USB_DIAG_LOG_BYTES;++I)Expected[I]=Ring[(Offset+I)%Cap];
  assert(request(1,0,24,Data,&Bytes)==EFI_SUCCESS&&mLogBytes==USB_DIAG_LOG_BYTES&&mLogFlags==1&&get32(Data+16)==DiagCrc(Expected,USB_DIAG_LOG_BYTES));
  assert(!memcmp(mLogSnapshot,Expected,USB_DIAG_LOG_BYTES));Generation=mLogGeneration;memset(Ring,'R',Cap);
  assert(request(2,511,536,Data,&Bytes)==EFI_SUCCESS&&Bytes==536&&get32(Data+8)==Generation&&get32(Data+12)==511*512&&DiagLe16(Data+16)==512&&get32(Data+20)==DiagCrc(Expected+511*512,512)&&!memcmp(Data+24,Expected+511*512,512));
  assert(request(2,512,536,Data,&Bytes)==EFI_UNSUPPORTED&&request(2,65535,536,Data,&Bytes)==EFI_UNSUPPORTED);
  // An older marker outside the bounded tail cannot fake a current marker.
  CONST CHAR8 *Marker="SUNUEFI_RAMLOG_BEGIN\n";UINTN M=strlen(Marker);memset(Ring,'T',Cap);memcpy(Ring,Marker,M);
  Header[1]=Header[2]=USB_DIAG_LOG_BYTES+5000;
  assert(request(1,0,24,Data,&Bytes)==EFI_SUCCESS&&mLogBytes==USB_DIAG_LOG_BYTES&&mLogFlags==1&&mLogSnapshot[0]=='T');
  // Keep the newest marker within the tail and freeze its exact remaining log.
  UINTN Newest=Header[2]-131113,Older=Header[2]-200000;Ring[Older-1]=Ring[Newest-1]='\n';memcpy(Ring+Older,Marker,M);memcpy(Ring+Newest,Marker,M);
  assert(request(1,0,24,Data,&Bytes)==EFI_SUCCESS&&mLogBytes==131113&&mLogFlags==3&&!memcmp(mLogSnapshot,Marker,M)&&get32(Data+16)==DiagCrc(Ring+Newest,131113));
  memcpy(Expected,mLogSnapshot,mLogBytes);Generation=mLogGeneration;memset(Ring,'X',Cap);assert(!memcmp(mLogSnapshot,Expected,131113)&&mLogGeneration==Generation);
  free(Expected);
}
int main(void) {
  assert(session_suite_main()==0);
  void *console=mmap((void *)0xA3500000,0x200000,PROT_READ|PROT_WRITE,
    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  assert(console==(void *)0xA3500000);
  UINT32 *header=console;CONST UINTN cap=0x200000-12;
  const char *prefix="older Android\nSUNUEFI_RAMLOG_BEGIN\nold session\nSUNUEFI_RAMLOG_BEGIN\n";
  UINTN n=strlen(prefix)+1100,start=cap-31;
  header[0]=0x43474244;header[1]=(UINT32)((start+n)%cap);header[2]=(UINT32)n;
  for(UINTN I=0;I<n;++I)((UINT8 *)console)[12+(start+I)%cap]=I<strlen(prefix)?prefix[I]:(UINT8)I;
  UINT8 data[4096],reply[4096];UINTN bytes=0;
  init();mControl.Configuration=0;
  assert(request(0,0,48,data,&bytes)==EFI_UNSUPPORTED);
  mControl.Configuration=1;mConfigured=TRUE;
  assert(request(2,0,536,data,&bytes)==EFI_UNSUPPORTED); // No snapshot yet.
  assert(request(1,0,24,data,&bytes)==EFI_SUCCESS && bytes==24 && !memcmp(data,"SUNLOG01",8));
  assert(get32(data+8)==1 && get32(data+12)==1121 && get32(data+16)==DiagCrc(mLogSnapshot,mLogBytes));
  assert(DiagLe16(data+20)==512 && DiagLe16(data+22)==2);
  UINT8 frozen[1121];CopyMem(frozen,mLogSnapshot,sizeof(frozen));
  memset((UINT8 *)console+12,0,cap); // Later live writes must not alter any snapshot page.
  UINT8 rebuilt[1121];
  for(UINT16 page=0;page<3;++page) {
    UINT16 payload=(UINT16)MIN((UINTN)512,sizeof(frozen)-(UINTN)page*512);
    assert(request(2,page,24+payload,data,&bytes)==EFI_SUCCESS && bytes==(UINTN)24+payload);
    assert(!memcmp(data,"SUNPAGE1",8) && get32(data+8)==1 && get32(data+12)==(UINT32)page*512);
    assert(DiagLe16(data+16)==payload && DiagLe16(data+18)==24 && get32(data+20)==DiagCrc(data+24,payload));
    CopyMem(rebuilt+(UINTN)page*512,data+24,payload);
  }
  assert(!memcmp(frozen,rebuilt,sizeof(frozen)));
  assert(request(2,2,536,data,&bytes)==EFI_BAD_BUFFER_SIZE); // Exact final-page length is required.
  assert(request(2,3,536,data,&bytes)==EFI_UNSUPPORTED);
  assert(request(2,65535,536,data,&bytes)==EFI_UNSUPPORTED);
  assert(request(0,1,48,data,&bytes)==EFI_UNSUPPORTED);
  assert(request(0,0,47,data,&bytes)==EFI_UNSUPPORTED);
  UINT8 out[8]={0x40,0x5B,0,0,0,0,48,0};PIANO_USB_CONTROL_ACTION action;
  assert(DiagnosticSetup(out,data,4096,&bytes,&action)==EFI_UNSUPPORTED);
  // The actual event handler copies replies through shared mapped mTx and a three-stage status OUT.
  PIANO_DMA_DEVICE device={0};PIANO_DMA_BUFFER *buffers[]={&mTrbs[0],&mTrbs[1],&mSetup,&mTx};
  for(UINTN I=0;I<ARRAY_SIZE(buffers);++I)
    assert(PianoDmaAllocate(&device,4096,4096,32,PianoDmaBidirectional,buffers[I])==EFI_SUCCESS);
  ZeroMem(mPending,sizeof(mPending));ZeroMem(mPayload,sizeof(mPayload));mControl.Address=1;
  assert(ArmSetup()==EFI_SUCCESS);
  assert(exchange(0x5A,0,0,12,reply)==12 && !memcmp(reply,"SUNUEFI1",8));
  assert(exchange(0x5B,0,0,48,reply)==48 && !memcmp(reply,"SUNDBG01",8));
  assert(reply[8]==1 && reply[9]==1 && get32(reply+44)==3);
  assert(exchange(0x5B,2,0,536,reply)==536 && !memcmp(reply+24,frozen,512));
  assert(Event(0x101)==EFI_SUCCESS && !mLogValid); // Reset invalidates pages.
  assert(request(2,0,536,data,&bytes)==EFI_UNSUPPORTED);
  assert(StopTransfer(0)==EFI_SUCCESS);
  for(UINTN I=0;I<ARRAY_SIZE(buffers);++I)assert(PianoDmaFree(buffers[I])==EFI_SUCCESS);
  mControl.Configuration=1;header[0]=0;
  assert(request(1,0,24,data,&bytes)==EFI_COMPROMISED_DATA && !mLogValid);
  header[0]=0x43474244;header[1]=0;header[2]=USB_DIAG_LOG_BYTES+123;
  assert(request(1,0,24,data,&bytes)==EFI_SUCCESS && mLogBytes==USB_DIAG_LOG_BYTES && (mLogFlags&1));
  large_console(header,cap);
  FreePool(mLogSnapshot);mLogSnapshot=NULL;mLogValid=FALSE;
  assert(munmap(console,0x200000)==0);
  puts("USB diagnostics: fixed console wrap/newest marker, bounded immutable pages/CRC, invalid request rejection, reset invalidation, and shared-DMA EP0 IN/STATUS-OUT passed.");
  return 0;
}
