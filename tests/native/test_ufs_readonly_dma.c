// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoUfsDmaLayout.c"
#include "../../uefi/core/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;static UINT8 trl[1024],ucd[1024],data[4096];static UINT32 bell,run,task_run;
static unsigned begins,completes;static int timeout,bad_ocs;
static unsigned unit_attentions,read_commands;
static int unit_attention_full_residual;
static UINT32 uic_is,uic_result,tx_state=1,mem_config;
static UINT32 power_mode=0x33;
static unsigned power_queries,ssu_commands,power_waits,pre_power_queries,ssu_unit_attentions;
static int power_query_fail,ssu_fail,ssu_keep_mode,ssu_response_one_good;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
VOID EFIAPI MemoryFence(VOID){ }
VOID EFIAPI CpuPause(VOID){ }
VOID EFIAPI CpuDeadLoop(VOID){assert(!"Unexpected reset fallthrough");}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){ }
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){assert(!B->Active&&!bell&&!run&&!task_run);B->Active=TRUE;++begins;return EFI_SUCCESS;}
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
    if(ucd[0]==0x16 && ucd[12]==3){
      ++power_queries;r[12]=3;r[13]=2;
      UINT32 mode=power_mode;if(pre_power_queries){--pre_power_queries;mode=0x20;}
      Be32(r+20,mode);if(power_query_fail)r[6]=0xFF;
    }
    if(ucd[0]==1) {
      ++read_commands;
      if(ucd[16]==0x1b){
        ++ssu_commands;
        assert(ucd[2]==0xd0 && ucd[20]==0x10 && ReadLe32(trl+28)==0 && ReadBe32(ucd+12)==0);
        if(ssu_unit_attentions || ssu_fail){
          r[6]=1;r[7]=2;Be16(r+10,20);Be16(r+32,18);r[34]=0x70;
          if(ssu_unit_attentions){--ssu_unit_attentions;r[36]=6;r[46]=0x29;}
          else {r[36]=2;r[46]=4;r[47]=2;}
        }else if(ssu_response_one_good){r[6]=1;r[7]=0;}
        else if(!ssu_keep_mode)power_mode=0x11;
        bell=0;return Value;
      }
      if(unit_attentions){
        --unit_attentions;r[7]=2;Be16(r+10,20);Be16(r+32,18);r[34]=0x70;r[36]=6;r[46]=0x29;
        // CHECK CONDITION may report all bytes untransferred with F=0.
        if(unit_attention_full_residual){r[1]=0;Be32(r+12,ReadBe32(ucd+12));}
        bell=0;return Value;
      }
      assert(ucd[1]==0x40 && ReadLe32(trl+28)==0x00400001);
      if(ucd[16]==0xa0){Be32(data,8);data[9]=0;r[1]=0x20;Be32(r+12,4096-16);}
      else if(ucd[16]==0x9e){Be32(data+4,0x3000000);Be32(data+8,4096);}
      else assert(ucd[16]==0x28);
    }
    bell=0;
  }
  return Value;
}
static EFI_STATUS EFIAPI stall(UINTN N){assert(N==10 || N==100 || N==10000);if(N==10000)++power_waits;return EFI_SUCCESS;}
static void ConfigurePower(UINT32 Mode){
  assert(begins==completes&&!bell&&!run&&!task_run&&!mData.Active&&!mUcd.Active&&!mTrl.Active);
  power_mode=Mode;power_queries=ssu_commands=power_waits=pre_power_queries=ssu_unit_attentions=0;
  power_query_fail=ssu_fail=ssu_keep_mode=ssu_response_one_good=0;
  unit_attentions=read_commands=0;unit_attention_full_residual=0;
  mPowerMode=MAX_UINT32;
}
static void ExpectPowerQuiet(void){
  assert(begins==completes&&!bell&&!run&&!task_run&&!mData.Active&&!mUcd.Active&&!mTrl.Active);
  assert(read_commands==ssu_commands);
}
static void TestEnsureDeviceActive(void){
  CONST UINT32 Ready[]={0x11,0};
  for(UINTN I=0;I<ARRAY_SIZE(Ready);++I){
    ConfigurePower(Ready[I]);assert(EnsureDeviceActive()==EFI_SUCCESS);
    assert(power_queries==1&&ssu_commands==0&&power_waits==0&&mPowerMode==Ready[I]);
    ExpectPowerQuiet();
  }
  CONST UINT32 Sleeping[]={0x22,0x33};
  for(UINTN I=0;I<ARRAY_SIZE(Sleeping);++I){
    ConfigurePower(Sleeping[I]);assert(EnsureDeviceActive()==EFI_SUCCESS);
    assert(power_queries==2&&ssu_commands==1&&power_mode==0x11&&mPowerMode==0x11);
    ExpectPowerQuiet();
  }
  ConfigurePower(0x44);assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==1&&ssu_commands==0&&mPowerMode==0x44);
  ExpectPowerQuiet();

  ConfigurePower(0x22);power_query_fail=1;assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==1&&ssu_commands==0&&mPowerMode==MAX_UINT32);
  ExpectPowerQuiet();

  ConfigurePower(0x22);ssu_fail=1;assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==1&&ssu_commands==1&&mPowerMode==0x22);
  ExpectPowerQuiet();

  ConfigurePower(0x22);ssu_keep_mode=1;assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==2&&ssu_commands==1&&mPowerMode==0x22);
  ExpectPowerQuiet();

  ConfigurePower(0x11);pre_power_queries=2;assert(EnsureDeviceActive()==EFI_SUCCESS);
  assert(power_queries==3&&ssu_commands==0&&power_waits==2&&mPowerMode==0x11);
  ExpectPowerQuiet();
  ConfigurePower(0x20);assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==3&&ssu_commands==0&&power_waits==2&&mPowerMode==0x20);
  ExpectPowerQuiet();

  ConfigurePower(0x22);ssu_unit_attentions=2;assert(EnsureDeviceActive()==EFI_SUCCESS);
  assert(power_queries==2&&ssu_commands==3&&ssu_unit_attentions==0&&mPowerMode==0x11);
  ExpectPowerQuiet();
  ConfigurePower(0x22);ssu_unit_attentions=3;assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==1&&ssu_commands==3&&ssu_unit_attentions==0&&mPowerMode==0x22);
  ExpectPowerQuiet();

  ConfigurePower(0x22);ssu_response_one_good=1;assert(EFI_ERROR(EnsureDeviceActive()));
  assert(power_queries==1&&ssu_commands==1&&mPowerMode==0x22);
  ExpectPowerQuiet();
  ConfigurePower(0x11);
}
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
  assert(power_mode==0x11);
  TestEnsureDeviceActive();
  mPowerMode=MAX_UINT32;
  mData.Cpu=data;mData.Bytes=4096;mData.DeviceAddress=0x40002000;mData.Mapped=TRUE;
  unsigned before=begins;
  assert(ReadScsi("REPORT_LUNS",10,0,PianoUfsReportLuns,0,4096)==EFI_SUCCESS && mTransferred==16);
  assert(begins==before+3 && completes==begins && !mData.Active);
  assert(ReadScsi("CAPACITY",11,0,PianoUfsReadCapacity16,0,32)==EFI_SUCCESS && mTransferred==32);
  before=read_commands;unit_attentions=1;
  assert(ReadScsi("CAPACITY",12,0,PianoUfsReadCapacity16,0,32)==EFI_SUCCESS && read_commands==before+2);
  before=read_commands;unit_attentions=4;
  assert(ReadScsi("CAPACITY",14,0,PianoUfsReadCapacity16,0,32)==EFI_DEVICE_ERROR && read_commands==before+3);
  unit_attention_full_residual=1;unit_attentions=1;before=read_commands;
  unsigned dma_before=begins;
  assert(ReadScsi("REPORT_LUNS",17,0,PianoUfsReportLuns,0,4096)==EFI_SUCCESS && mTransferred==16);
  assert(read_commands==before+2 && unit_attentions==0 && begins==dma_before+6 && completes==begins);
  assert(!mData.Active && !mUcd.Active && !mTrl.Active && !bell);
  unit_attentions=4;before=read_commands;dma_before=begins;
  assert(ReadScsi("CAPACITY",20,0,PianoUfsReadCapacity16,0,32)==EFI_DEVICE_ERROR);
  assert(read_commands==before+3 && unit_attentions==1 && begins==dma_before+9 && completes==begins);
  assert(!mData.Active && !mUcd.Active && !mTrl.Active && !bell);
  unit_attentions=0;unit_attention_full_residual=0;
  bad_ocs=1;PianoUfsBuildNop(trl,1024,ucd,1024,mUcd.DeviceAddress,3);
  assert(Submit("BAD_OCS",3,TRUE)==EFI_DEVICE_ERROR);bad_ocs=0;timeout=1;
  PianoUfsBuildNop(trl,1024,ucd,1024,mUcd.DeviceAddress,4);
  assert(Submit("TIMEOUT",4,TRUE)==EFI_TIMEOUT && !bell && !mTrl.Active && !mUcd.Active);
  before=begins;ZeroMem(ucd,sizeof(ucd));ucd[0]=1;ucd[1]=0x20;ucd[16]=0x2A;
  assert(Submit("DEFAULT_WRITE_DENIED",9,FALSE)==EFI_ACCESS_DENIED && begins==before);
  ucd[1]=0;ucd[16]=0x35;
  assert(Submit("DEFAULT_SYNC_DENIED",9,FALSE)==EFI_ACCESS_DENIED && begins==before);
  ucd[16]=0x8A;
  assert(Submit("UNKNOWN_WRITE_CDB_DENIED",9,FALSE)==EFI_ACCESS_DENIED && begins==before);
  ucd[0]=0x16;ucd[5]=0x81;ucd[12]=2;
  assert(Submit("WRITE_QUERY_DENIED",9,FALSE)==EFI_ACCESS_DENIED && begins==before);
  puts("Read-only UFS DMA engine: power readiness, bounded Active recovery, response validation and DMA quiescence passed.");return 0;
}
