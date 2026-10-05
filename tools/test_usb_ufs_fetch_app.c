// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual combined app; all owners and runtime functions are memory-only mocks.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUsbUfsFetch.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;static EFI_RUNTIME_SERVICES rt;
static PIANO_FB_STORAGE storage;static PIANO_UFS_RESET_REPORT report;
static EFI_STATUS init_status,usb_status,prepare_status,shutdown_status;
static EFI_STATUS proof_status,accept_status;
static BOOLEAN absent_storage,absent_report,reboot,usb_closed;
static unsigned inits,stops,usbs,prepares,shutdowns,reports,resets,loops,raises,lowers,cases;
static unsigned proofs,accepts;
static EFI_TPL tpl;static jmp_buf jump;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN N){return FALSE;}VOID EFIAPI DebugPrint(UINTN N,CONST CHAR8 *F,...){ }
static EFI_TPL EFIAPI raise(EFI_TPL New){assert(New==TPL_CALLBACK && New>=tpl);EFI_TPL Old=tpl;tpl=New;++raises;return Old;}
static VOID EFIAPI lower(EFI_TPL Old){assert(Old==TPL_APPLICATION && tpl==TPL_CALLBACK && usb_closed && report.Clean && !report.Failed && shutdowns==1 && stops==1);tpl=Old;++lowers;}
VOID EFIAPI CpuDeadLoop(VOID){assert(tpl==TPL_CALLBACK);++loops;longjmp(jump,1);}
EFI_STATUS PianoFastbootBlockReadInit(VOID){assert(tpl==TPL_APPLICATION);++inits;return init_status;}
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID){assert(inits==1 && init_status==EFI_SUCCESS);return absent_storage?NULL:&storage;}
VOID PianoFastbootBlockReadStop(VOID){assert(inits==1 && init_status==EFI_SUCCESS);assert(!usbs || usb_closed || usb_status!=EFI_SUCCESS);++stops;}
EFI_STATUS PianoUsbControllerRunWithStorage(CONST VOID *Fdt,CONST PIANO_FB_STORAGE *S,BOOLEAN *Reboot){assert(Fdt==(VOID *)123 && S==&storage && !stops && tpl==TPL_APPLICATION);++usbs;*Reboot=reboot;usb_closed=usb_status==EFI_SUCCESS;return usb_status;}
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *Fdt,PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(Fdt==(VOID *)123 && usb_closed && stops==1 && tpl==TPL_APPLICATION);++proofs;Proof->Valid=proof_status==EFI_SUCCESS;Proof->Slot=1;return proof_status;}
EFI_STATUS PianoUfsAcceptRetiredUsb(CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(proofs==1 && Proof->Valid && Proof->Slot==1 && usb_closed && tpl==TPL_APPLICATION);++accepts;return accept_status;}
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID){assert(usb_closed && stops==1 && tpl==TPL_CALLBACK);++prepares;return prepare_status;}
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID){assert(prepares==1 && prepare_status==EFI_SUCCESS && tpl==TPL_CALLBACK);++shutdowns;return shutdown_status;}
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID){assert(prepares==1);++reports;return absent_report?NULL:&report;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN N,VOID *Data){assert(Type==EfiResetCold && Status==EFI_SUCCESS && !N && !Data && usb_closed && shutdowns==1 && report.Clean && !report.Failed && tpl==TPL_CALLBACK && !lowers);++resets;}
static void fresh(void){++cases;inits=stops=usbs=prepares=shutdowns=reports=resets=loops=raises=lowers=proofs=accepts=0;tpl=TPL_APPLICATION;init_status=usb_status=prepare_status=shutdown_status=proof_status=accept_status=EFI_SUCCESS;absent_storage=absent_report=reboot=usb_closed=FALSE;memset(&report,0,sizeof(report));report.Started=report.Prepared=report.Returned=report.Clean=report.TplHeld=TRUE;}
static void fail(void){if(!setjmp(jump)){PianoRunUsbUfsFetch((VOID *)123);assert(!"Failure must not return");}assert(loops==1 && !lowers && !resets && tpl==TPL_CALLBACK);}
int main(void){gBS=&bs;gRT=&rt;bs.RaiseTPL=raise;bs.RestoreTPL=lower;rt.ResetSystem=reset;
 fresh();assert(PianoRunUsbUfsFetch((VOID *)123)==EFI_SUCCESS && stops==1 && usbs==1 && prepares==1 && shutdowns==1 && reports==1 && lowers==1 && !resets && !loops);
 fresh();reboot=TRUE;if(!setjmp(jump)){PianoRunUsbUfsFetch((VOID *)123);assert(!"Returned ResetSystem must fence");}assert(resets==1 && loops==1 && !lowers && stops==1 && shutdowns==1);
 fresh();usb_status=EFI_DEVICE_ERROR;fail();assert(stops==1 && !prepares && !shutdowns);
 fresh();usb_status=EFI_WARN_UNKNOWN_GLYPH;fail();assert(stops==1 && !prepares);
 fresh();proof_status=EFI_DEVICE_ERROR;fail();assert(proofs==1 && !accepts && !prepares);
 fresh();proof_status=EFI_WARN_UNKNOWN_GLYPH;fail();assert(proofs==1 && !accepts && !prepares);
 fresh();accept_status=EFI_COMPROMISED_DATA;fail();assert(proofs==1 && accepts==1 && !prepares);
 fresh();prepare_status=EFI_TIMEOUT;fail();assert(prepares==1 && !shutdowns);
 fresh();prepare_status=EFI_WARN_UNKNOWN_GLYPH;fail();assert(!shutdowns);
 fresh();shutdown_status=EFI_DEVICE_ERROR;fail();assert(shutdowns==1);
 fresh();absent_report=TRUE;fail();fresh();report.Clean=FALSE;fail();fresh();report.Failed=TRUE;fail();
 fresh();report.TplHeld=FALSE;fail();fresh();report.Started=FALSE;fail();fresh();report.Prepared=FALSE;fail();fresh();report.Returned=FALSE;fail();fresh();report.Result=EFI_NOT_STARTED;fail();
 EFI_STATUS *Stages[]={&report.Disconnect,&report.Halt,&report.Bases,&report.Dma,&report.Domain,&report.Protocols,&report.Clocks};
 for(unsigned I=0;I<7;++I){fresh();*Stages[I]=EFI_DEVICE_ERROR;fail();}
 UINT32 *Regs[]={&report.TransferDoorbell,&report.TaskDoorbell,&report.TransferRun,&report.TaskRun,&report.Interrupt};
 for(unsigned I=0;I<5;++I){fresh();*Regs[I]=0x80000000;fail();}
 fresh();init_status=EFI_NOT_FOUND;fail();assert(inits==1 && !stops && !usbs && !prepares && !shutdowns);
 fresh();init_status=EFI_ALREADY_STARTED;fail();assert(!stops && !usbs && !prepares);
 fresh();init_status=EFI_WARN_UNKNOWN_GLYPH;fail();assert(!stops && !usbs);
 fresh();absent_storage=TRUE;fail();assert(stops==1 && !usbs && !prepares && !shutdowns);
 printf("Actual combined fetch app: %u memory-only ordering/exact-status/cleanup/report/reboot/failstop cases passed.\n",cases);
}
