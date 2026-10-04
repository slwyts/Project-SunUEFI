// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUfsDmaLayout.c"
#include "../bootprofiles/uefi-app/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;static UINT8 trl[1024],ucd[1024],data[4096];static UINT32 bell,run,task_run;
static unsigned begins,completes;static int timeout,bad_ocs;
static UINT32 uic_is,uic_result,tx_state=1,mem_config;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID EFIAPI MemoryFence(VOID){ }
VOID EFIAPI CpuPause(VOID){ }
VOID EFIAPI CpuDeadLoop(VOID){assert(!"Unexpected reset fallthrough");}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){ }
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){assert(!B->Active);B->Active=TRUE;++begins;return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active && Quiet);B->Active=FALSE;++completes;return Status;}
UINT32 EFIAPI MmioRead32(UINTN A){
  if(A==HCI+0x58)return bell;
  if(A==HCI+0x78)return 0;
  if(A==HCI+0x60)return run;
  if(A==HCI+0x80)return task_run;
  if(A==HCI+0x34)return 1;
  if(A==HCI+0x30)return 15;
  if(A==HCI+0x20)return uic_is?uic_is:1;
  if(A==HCI+0x300)return mem_config;
  if(A==HCI+0x98)return uic_result;
  if(A==HCI+0x9c)return tx_state;
  assert(!"Unexpected UFS read");return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 Value){
  if(A==HCI+0x20){uic_is&=~Value;return Value;}
  if(A==HCI+0x94 || A==HCI+0x98 || A==HCI+0x9c)return Value;
  if(A==HCI+0x90){assert(Value==1 || Value==0x18);uic_is=Value==1?0x400:0x420;if(Value==0x18)tx_state=2;return Value;}
  if(A==HCI+0x60){run=Value;return Value;}
  if(A==HCI+0x80){task_run=Value;return Value;}
  if(A==HCI+0x5c){assert(Value==~1U);bell=0;return Value;}
  assert(A==HCI+0x58 && Value==1 && run==1);bell=1;
  assert(ucd[0]==0 || ucd[0]==1 || (ucd[0]==0x16 && ucd[5]==1 && (ucd[12]==1 || ucd[12]==3)));
  if(!timeout){
    Le32(trl+8,bad_ocs?1:0);UINT8 *r=ucd+64;r[0]=ucd[0]==1?0x21:ucd[0]?0x36:0x20;r[3]=ucd[3];r[6]=0;r[2]=ucd[2];
    if(ucd[0]==0x16 && ucd[12]==1){r[12]=1;r[13]=0;r[10]=0;r[11]=0x40;r[32]=0x40;r[33]=0;r[38]=6;r[39]=4;}
    if(ucd[0]==0x16 && ucd[12]==3){r[12]=3;r[13]=2;Be32(r+20,0x33);}
    if(ucd[0]==1) {
      if(ucd[16]==0x1b){assert(ucd[2]==0xd0 && ucd[20]==0x10 && ReadLe32(trl+28)==0);}
      else {
        assert(ucd[1]==0x40 && ReadLe32(trl+28)==0x00400001);
        if(ucd[16]==0xa0){Be32(data,8);data[9]=0;r[1]=0x20;Be32(r+12,4096-16);}
        else if(ucd[16]==0x9e){Be32(data+4,0x3000000);Be32(data+8,4096);}
        else assert(ucd[16]==0x28);
      }
    }
    bell=0;
  }
  return Value;
}
static EFI_STATUS EFIAPI stall(UINTN N){assert(N==10 || N==100);return EFI_SUCCESS;}
int main(void){
  gBS=&bs;bs.Stall=stall;mTrl.Cpu=trl;mTrl.Bytes=sizeof(trl);mTrl.DeviceAddress=0x40000000;
  assert(LinkReady()==EFI_SUCCESS && tx_state==2);
  mem_config=1;assert(LinkReady()==EFI_UNSUPPORTED);mem_config=0;
  mUcd.Cpu=ucd;mUcd.Bytes=sizeof(ucd);mUcd.DeviceAddress=0x40001000;
  assert(PianoUfsBuildNop(trl,1024,ucd,1024,mUcd.DeviceAddress,1)==EFI_SUCCESS);
  assert(Submit("NOP",1,TRUE)==EFI_SUCCESS && begins==2 && completes==2);
  assert(PianoUfsBuildReadDescriptor(trl,1024,ucd,1024,mUcd.DeviceAddress,2,0,0,255)==EFI_SUCCESS);
  assert(Submit("QUERY",2,FALSE)==EFI_SUCCESS);
  assert(PianoUfsBuildReadPowerMode(trl,1024,ucd,1024,mUcd.DeviceAddress,6)==EFI_SUCCESS);
  assert(Submit("POWER",6,FALSE)==EFI_SUCCESS && mPowerMode==0x33);
  assert(PianoUfsBuildResumeActive(trl,1024,ucd,1024,mUcd.DeviceAddress,7)==EFI_SUCCESS);
  assert(Submit("RESUME",7,FALSE)==EFI_SUCCESS && mTransferred==0);
  mData.Cpu=data;mData.Bytes=4096;mData.DeviceAddress=0x40002000;mData.Mapped=TRUE;
  unsigned before=begins;
  assert(ReadScsi("REPORT_LUNS",10,0,PianoUfsReportLuns,0,4096)==EFI_SUCCESS && mTransferred==16);
  assert(begins==before+3 && completes==begins && !mData.Active);
  assert(ReadScsi("CAPACITY",11,0,PianoUfsReadCapacity16,0,32)==EFI_SUCCESS && mTransferred==32);
  bad_ocs=1;PianoUfsBuildNop(trl,1024,ucd,1024,mUcd.DeviceAddress,3);
  assert(Submit("BAD_OCS",3,TRUE)==EFI_DEVICE_ERROR);bad_ocs=0;timeout=1;
  PianoUfsBuildNop(trl,1024,ucd,1024,mUcd.DeviceAddress,4);
  assert(Submit("TIMEOUT",4,TRUE)==EFI_TIMEOUT && !bell && !mTrl.Active && !mUcd.Active);
  puts("Read-only UFS DMA engine: NOP/query layouts, shared lifecycle, response validation and bounded timeout/quiescence passed.");return 0;
}
