// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#undef NULL
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_BOUNDED_VOLUME 1
#include "../bootprofiles/uefi-app/PianoUfsDmaLayout.c"
#include "../bootprofiles/uefi-app/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;static UINT8 trl[1024],ucd[1024],data[4096];static UINT32 bell,run,task_run;
static jmp_buf jump;static UINT32 irq;static BOOLEAN stuck;static unsigned resets,deadloops,raises,lowers;
static unsigned begins,completes;static int timeout,bad_ocs;
static UINT32 uic_is,uic_result,tx_state=1,mem_config;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID EFIAPI MemoryFence(VOID){ }
VOID EFIAPI CpuPause(VOID){ }
VOID EFIAPI CpuDeadLoop(VOID){++deadloops;longjmp(jump,1);}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){ }
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){assert(!B->Active);B->Active=TRUE;++begins;return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active);++completes;if(Quiet){B->Active=FALSE;B->Quarantined=FALSE;}else B->Quarantined=TRUE;return Quiet?Status:EFI_DEVICE_ERROR;}
UINT32 EFIAPI MmioRead32(UINTN A){
  if(A==HCI+0x24)return irq;
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
  if(A==HCI+0x24){irq=Value;return Value;}
  if(A==HCI+0x60){if(!stuck || Value)run=Value;return Value;}
  if(A==HCI+0x80){task_run=Value;return Value;}
  if(A==HCI+0x5c){assert(Value==~1U);if(!stuck)bell=0;return Value;}
  assert(A==HCI+0x58 && Value==1 && run==1);bell=1;
  assert(ucd[0]==0 || ucd[0]==1 || (ucd[0]==0x16 && ucd[5]==1 && (ucd[12]==1 || ucd[12]==3)));
  if(!timeout){
    Le32(trl+8,bad_ocs?1:0);UINT8 *r=ucd+64;r[0]=ucd[0]==1?0x21:ucd[0]?0x36:0x20;r[3]=ucd[3];r[6]=0;r[2]=ucd[2];
    if(ucd[0]==0x16 && ucd[12]==1){r[12]=1;r[13]=0;r[10]=0;r[11]=0x40;r[32]=0x40;r[33]=0;r[38]=6;r[39]=4;}
    if(ucd[0]==0x16 && ucd[12]==3){r[12]=3;r[13]=2;Be32(r+20,0x33);}
    if(ucd[0]==1) {
      if(ucd[16]==0x1b){assert(ucd[2]==0xd0 && ucd[20]==0x10 && ReadLe32(trl+28)==0);}
      else if(ucd[16]==0x2A){assert(ucd[1]==0x20 && ucd[2]==4 && ucd[17]==8 && ucd[24]==1 && ReadBe32(ucd+18)>=375040 && ReadBe32(ucd+18)<=378623);}
      else if(ucd[16]==0x35){assert(ucd[1]==0 && ucd[2]==4 && ReadBe32(ucd+18)==375040 && ucd[23]==14 && ucd[24]==0 && ReadLe32(trl+28)==0);}
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
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!"Dirty bounded DMA must not free");return EFI_ACCESS_DENIED;}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){assert(!"Dirty bounded table must not detach");return EFI_ACCESS_DENIED;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL T){assert(T==TPL_CALLBACK);++raises;return TPL_APPLICATION;}
static VOID EFIAPI lower_tpl(EFI_TPL T){assert(T==TPL_APPLICATION);++lowers;}
static VOID EFIAPI reset(EFI_RESET_TYPE T,EFI_STATUS S,UINTN N,VOID *D){++resets;assert(!"Dirty bounded session must not reset");}
static EFI_RUNTIME_SERVICES rt;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiSimpleFileSystemProtocolGuid;
static unsigned existing_block,existing_sfs;static EFI_STATUS inventory_status;
static EFI_HANDLE inventory_handles[1];
VOID EFIAPI FreePool(VOID *P){assert(P==inventory_handles);}
static EFI_STATUS EFIAPI inventory(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Protocol,VOID *Key,UINTN *Count,EFI_HANDLE **Handles){
 assert(Type==ByProtocol);*Count=Protocol==&gEfiBlockIoProtocolGuid?existing_block:existing_sfs;
 *Handles=*Count?inventory_handles:NULL;return inventory_status;
}

int main(void){
 gBS=&bs;gRT=&rt;bs.Stall=stall;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=lower_tpl;rt.ResetSystem=reset;
 mTrl=(PIANO_DMA_BUFFER){.Cpu=trl,.Bytes=1024,.DeviceAddress=0x40000000,.Mapped=TRUE};
 mUcd=(PIANO_DMA_BUFFER){.Cpu=ucd,.Bytes=1024,.DeviceAddress=0x40001000,.Mapped=TRUE};
 mData=(PIANO_DMA_BUFFER){.Cpu=data,.Bytes=4096,.DeviceAddress=0x40002000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
 assert(PianoUfsBoundedBuildWrite10(trl,1024,ucd,1024,mUcd.DeviceAddress,mData.DeviceAddress,9,4,378623,4096)==EFI_SUCCESS);
 assert(Submit("NO_WINDOW_SCOPE",9,FALSE)==EFI_ACCESS_DENIED && !begins);
 mWindowExecuting=mBlockBusy=TRUE;
 assert(Submit("LAST_GAP_BLOCK",9,FALSE)==EFI_SUCCESS && mTransferred==4096 && mWindowWriteDoorbells==1);
 assert(PianoUfsBoundedBuildWrite10(trl,1024,ucd,1024,mUcd.DeviceAddress,mData.DeviceAddress,10,4,375040,4096)==EFI_SUCCESS);
 Be32(ucd+18,378624);unsigned before=begins;assert(Submit("ESCAPED_LAYOUT",10,FALSE)==EFI_ACCESS_DENIED && begins==before);
 assert(PianoUfsBoundedBuildSync10(trl,1024,ucd,1024,mUcd.DeviceAddress,11,4,375040,14680064)==EFI_SUCCESS);
 assert(Submit("FULL_GAP_SYNC",11,FALSE)==EFI_SUCCESS && mWindowSyncDoorbells==1 && !mTransferred);
 assert(PianoUfsBoundedBuildWrite10(trl,1024,ucd,1024,mUcd.DeviceAddress,mData.DeviceAddress,12,4,375040,4096)==EFI_SUCCESS);
 timeout=stuck=TRUE;assert(Submit("UNKNOWN_QUIET",12,FALSE)==EFI_TIMEOUT && mData.Active && mData.Quarantined && !resets);
 timeout=stuck=FALSE;BOOLEAN Quiet=FALSE;assert(WindowQuiet(&mWindow,&Quiet)==EFI_SUCCESS && Quiet && !mData.Active && !mData.Quarantined);
 mWindowExecuting=mBlockBusy=FALSE;mInstalled=TRUE;mWindow.State.Closed=TRUE;mWindowDisconnected=TRUE;
 assert(WindowAcquire(&mWindowRecovery)==EFI_SUCCESS && raises==1);
 assert(WindowAcquire(&mWindowRecovery)==EFI_SUCCESS && raises==1 && mWindowDepth==2);
 assert(WindowRelease(&mWindowRecovery,FALSE)==EFI_SUCCESS && !lowers && mWindowDepth==1);
 assert(WindowRelease(&mWindowRecovery,FALSE)==EFI_SUCCESS && lowers==1 && !mBlockBusy);
 mWindow.State.Dirty=mWindow.State.NeedsRecovery=TRUE;mWindow.Media.MediaPresent=TRUE;
 if(!setjmp(jump)){HaltService();assert(!"Dirty timer Halt must fence reset");}assert(deadloops==1 && !resets);
 if(!setjmp(jump)){FaultDiagnostic();assert(!"Dirty exception must fence reset");}assert(deadloops==2 && !resets);
 assert(Cleanup()==EFI_ACCESS_DENIED);
 bs.LocateHandleBuffer=inventory;inventory_status=EFI_NOT_FOUND;
 assert(WindowRequireIsolation()==EFI_SUCCESS);existing_block=1;inventory_status=EFI_SUCCESS;
 assert(WindowRequireIsolation()==EFI_ACCESS_DENIED);existing_block=0;existing_sfs=1;assert(WindowRequireIsolation()==EFI_ACCESS_DENIED);
 existing_sfs=0;inventory_status=EFI_DEVICE_ERROR;assert(WindowRequireIsolation()==EFI_DEVICE_ERROR);
 inventory_status=EFI_WARN_UNKNOWN_GLYPH;assert(WindowRequireIsolation()==EFI_DEVICE_ERROR);

 puts("Actual bounded Submit: exact gap golden layouts, scope/default rejection, full-range SYNC, unknown queue retention, confirmed retirement, nested recovery/TPL and dirty timer/exception fences passed.");
}
