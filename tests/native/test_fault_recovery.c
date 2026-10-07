// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#undef NULL
#include "../../uefi/core/PianoFaultRecovery.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiCpuArchProtocolGuid;
static EFI_CPU_ARCH_PROTOCOL cpu;static EFI_BOOT_SERVICES bs;static EFI_RUNTIME_SERVICES rt;
static EFI_CPU_INTERRUPT_HANDLER handlers[4];static int fail_serror;
static jmp_buf reset_jump;static unsigned resets;static char logline[512];
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
UINTN EFIAPI AsciiSPrint(CHAR8 *B,UINTN N,CONST CHAR8 *F,...){va_list ap;va_start(ap,F);
  int n=vsnprintf(B,N,F,ap);va_end(ap);return n<0?0:(UINTN)n;}
UINTN EFIAPI SerialPortWrite(UINT8 *P,UINTN N){assert(N<sizeof(logline));memcpy(logline,P,N);logline[N]=0;return N;}
VOID ArmCallSmc(ARM_SMC_ARGS *Args){assert(!"ResetSystem mock should not return");}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN N,VOID *Data){
  assert(Type==EfiResetCold && Status==EFI_ABORTED && N==0 && Data==NULL);++resets;longjmp(reset_jump,1);
}
static EFI_STATUS EFIAPI locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){*Out=&cpu;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI reg(EFI_CPU_ARCH_PROTOCOL *This,EFI_EXCEPTION_TYPE Type,EFI_CPU_INTERRUPT_HANDLER H){
  assert(This==&cpu && (Type==0 || Type==3));
  if(Type==3 && fail_serror)return EFI_ALREADY_STARTED;
  if(H && handlers[Type])return EFI_ALREADY_STARTED;
  handlers[Type]=H;return EFI_SUCCESS;
}
int main(void){
  gBS=&bs;bs.LocateProtocol=locate;gRT=&rt;rt.ResetSystem=reset;cpu.RegisterInterruptHandler=reg;
  assert(PianoTestFaultRecovery()==EFI_NOT_READY);
  fail_serror=1;assert(PianoStartFaultRecovery()==EFI_ALREADY_STARTED);
  assert(!handlers[0] && !mCpu && !mSync);fail_serror=0;
  assert(PianoStartFaultRecovery()==EFI_SUCCESS && handlers[0] && handlers[3]);
  EFI_SYSTEM_CONTEXT_AARCH64 c={.ELR=0x1234,.ESR=0x96000010,.FAR=0xA8A008,.SPSR=0x3c5};
  EFI_SYSTEM_CONTEXT context={.SystemContextAArch64=&c};
  if(setjmp(reset_jump)==0)handlers[0](0,context);
  assert(resets==1 && strstr(logline,"PC=0x1234") && strstr(logline,"FAR=0xa8a008"));
  PianoStopFaultRecovery();assert(!handlers[0] && !handlers[3] && !mInException);
  puts("Fault recovery: handler registration rollback, RAM diagnostic fields, cold reset and callback cleanup passed.");
  return 0;
}
