// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoKeys.c"
EFI_BOOT_SERVICES *gBS;static EFI_BOOT_SERVICES bs;
EFI_GUID gEfiSimpleTextInProtocolGuid={0};EFI_GUID gEfiDevicePathProtocolGuid={0};
static unsigned step,failure;static EFI_STATUS injected;static EFI_TPL current;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){EFI_TPL old=current;current=New;return old;}
static VOID EFIAPI restore(EFI_TPL Old){current=Old;}
static EFI_STATUS result(unsigned expected){assert(current==TPL_APPLICATION && ++step==expected);return step==failure?injected:EFI_SUCCESS;}
static EFI_STATUS EFIAPI timer(EFI_EVENT E,EFI_TIMER_DELAY D,UINT64 T){assert(E==(VOID*)1 && D==TimerCancel && !T);return result(1);}
static EFI_STATUS EFIAPI close_event(EFI_EVENT E){return result(E==(VOID*)1?2:5);}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE H,EFI_HANDLE D,EFI_HANDLE C){assert(H==(VOID*)2 && !D && !C);return result(3);}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE H,...){assert(H==(VOID*)2);return result(4);}
static void run(unsigned mode){
 memset(&bs,0,sizeof(bs));gBS=&bs;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore;bs.SetTimer=timer;bs.CloseEvent=close_event;
 bs.DisconnectController=disconnect;bs.UninstallMultipleProtocolInterfaces=uninstall;
 current=TPL_APPLICATION;mPoll=(VOID*)1;mHandle=(VOID*)2;mInput.WaitForKey=(VOID*)3;
 failure=mode?((mode-1)%5)+1:0;injected=mode>5?EFI_WARN_STALE_DATA:EFI_DEVICE_ERROR;
 PIANO_KEYS_RETIRE_REPORT report;EFI_STATUS s=PianoStopKeysForProduct(&report);
 if(!mode){assert(s==EFI_SUCCESS && report.Clean && report.Returned && !report.Retained && !mPoll && !mHandle && !mInput.WaitForKey && step==5);}
 else{assert(s==EFI_DEVICE_ERROR && report.Retained && !report.Clean && step==failure);unsigned old=step;
  assert(PianoStopKeysForProduct(&report)==EFI_ACCESS_DENIED && step==old);}
}
int main(void){for(unsigned mode=0;mode<=10;mode++){pid_t p=fork();assert(p>=0);if(!p){run(mode);_exit(0);}int status;assert(waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status));}
 puts("Actual GPIO input lifecycle: exact cancel/close/disconnect/uninstall/wait-close, warnings retain and no retry passed (11 forks).");return 0;}
