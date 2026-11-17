// SPDX-License-Identifier: BSD-2-Clause-Patent
// Piano factory DT: PMK8550 SDAM 0x7148 bits[7:1], TCSR dload at 0x1fd9000.
// Protocol facts: MiCode piano-w-oss 45fb9bd6, qcom-reboot-reason.c,
// spmi-pmic-arb.c and qcom_scm.c. No partition or persistent firmware writes.
#include "PianoReset.h"
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DebugLib.h>

#define ARB_CORE 0x0C400000U
#define ARB_CFG 0x0C42D000U
#define ARB_OBS 0x0C440000U
#define ARB_RW 0x0C500000U
STATIC BOOLEAN mReasonLayout,mDownloadLayout;
STATIC UINT32 Be(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC BOOLEAN Cells(CONST VOID *F,INT32 N,CONST CHAR8 *Name,CONST UINT32 *V,UINTN Count){
  INT32 Len=0;CONST UINT8 *P=FdtGetProp(F,N,Name,&Len);
  if(!P||Len!=(INT32)(Count*4))return FALSE;
  for(UINTN I=0;I<Count;++I)if(Be(P+I*4)!=V[I])return FALSE;return TRUE;
}
EFI_STATUS PianoResetInitialize(CONST VOID *Fdt){
  if(!Fdt||FdtCheckHeader(Fdt))return EFI_INVALID_PARAMETER;
  INT32 N=FdtPathOffset(Fdt,"/soc/qcom,spmi@c42d000/qcom,pmk8550@0/sdam@7100/restart@48");
  CONST UINT32 Reg[]={0x48,1},Bits[]={1,7};
  mReasonLayout=N>=0&&Cells(Fdt,N,"reg",Reg,2)&&Cells(Fdt,N,"bits",Bits,2);
  N=FdtPathOffset(Fdt,"/firmware/qcom_scm");INT32 Len=0;
  CONST UINT8 *P=N<0?NULL:FdtGetProp(Fdt,N,"qcom,dload-mode",&Len);
  if(P&&Len==8&&Be(P+4)==0x19000){
    N=FdtNodeOffsetByPhandle(Fdt,Be(P));CONST UINT32 Tcsr[]={0x1FC0000,0x30000};
    mDownloadLayout=N>=0&&Cells(Fdt,N,"reg",Tcsr,2);
  }
  return mReasonLayout&&mDownloadLayout?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS Wait(UINTN Base){
  for(UINTN I=0;I<1000;++I){UINT32 S=MmioRead32(Base+8);
    if(S&1)return S&0xE?EFI_DEVICE_ERROR:EFI_SUCCESS;gBS->Stall(1);}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Reason(UINT8 Value){
  if(!mReasonLayout)return EFI_UNSUPPORTED;
  UINT32 Version=MmioRead32(ARB_CORE),Count=MmioRead32(ARB_CORE+4)&0x7FF;
  if(Version<0x70000000||Version>=0x80000000||!Count||Count>1024)return EFI_UNSUPPORTED;
  UINTN Apid=MAX_UINTN;
  for(UINTN I=0;I<Count;++I){UINT32 Map=MmioRead32(ARB_CORE+0x2000+4*I);
    if(Map&&((Map>>8)&0xFFF)==0x071&&!(MmioRead32(ARB_CFG+4*I)&7))Apid=I;}
  if(Apid==MAX_UINTN)return EFI_ACCESS_DENIED;
  UINTN Read=ARB_OBS+0x20*Apid,Write=ARB_RW+0x1000*Apid;
  MmioWrite32(Read,(1U<<27)|(0x48U<<4));EFI_STATUS S=Wait(Read);if(S!=EFI_SUCCESS)return S;
  UINT8 Before=(UINT8)MmioRead32(Read+0x18),After=(Before&1)|(Value<<1);
  MmioWrite32(Write+0x10,After);MemoryFence();MmioWrite32(Write,0x48U<<4);
  S=Wait(Write);if(S!=EFI_SUCCESS)return S;
  MmioWrite32(Read,(1U<<27)|(0x48U<<4));S=Wait(Read);
  if(S==EFI_SUCCESS&&(UINT8)MmioRead32(Read+0x18)!=After)S=EFI_DEVICE_ERROR;
  DEBUG((DEBUG_WARN,"PIANO_RESET_REASON before=%02x after=%02x status=%r\n",Before,After,S));return S;
}
STATIC EFI_STATUS Scm(UINTN Function,UINTN Count,UINTN A,UINTN B,UINTN *Value){
  UINTN Call=Function,Token=0;
  for(UINTN I=0;I<8;++I){ARM_SMC_ARGS Args={0};Args.Arg0=Call;Args.Arg1=Count;Args.Arg2=A;Args.Arg3=B;Args.Arg6=Token;
    ArmCallSmc(&Args);if(Args.Arg0==1){Call=1;Token=Args.Arg6;continue;}
    if(Args.Arg0)return EFI_DEVICE_ERROR;if(Value)*Value=Args.Arg1;return EFI_SUCCESS;}
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Download(VOID){
  if(!mDownloadLayout)return EFI_UNSUPPORTED;
  UINTN Value=0;EFI_STATUS S=Scm(0xC2000501,1,0x1FD9000,0,&Value);if(S!=EFI_SUCCESS)return S;
  // Factory qcom_scm_set_download_mode(QCOM_DOWNLOAD_EDL) writes mode 1.
  S=Scm(0xC2000502,2,0x1FD9000,1,NULL);if(S!=EFI_SUCCESS)return S;
  S=Scm(0xC2000501,1,0x1FD9000,0,&Value);return S!=EFI_SUCCESS?S:Value==1?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
EFI_STATUS PianoResetExecute(CONST PIANO_PRODUCT_OWNERS *O){
  if(!O||!O->Report.ResetPrepared||O->Report.Busy||O->Report.ServicesLost||
     (O->Report.AllowedAction!=PianoUsbServiceActionReboot&&O->Report.AllowedAction!=PianoUsbServiceActionContinue))return EFI_ACCESS_DENIED;
  UINT32 Target=O->Report.Origin==PianoProductRequestUi?O->Report.ResetTarget:0;
  if(Target>PIANO_PRODUCT_RESET_EDL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=EFI_SUCCESS;
  if(Target==PIANO_PRODUCT_RESET_RECOVERY||Target==PIANO_PRODUCT_RESET_BOOTLOADER)
    S=Reason(Target==PIANO_PRODUCT_RESET_RECOVERY?1:2);
  else if(Target==PIANO_PRODUCT_RESET_EDL)S=Download();
  if(S!=EFI_SUCCESS)return S;
  if(!gRT || !gRT->ResetSystem)return EFI_UNSUPPORTED;
  DEBUG((DEBUG_WARN,"PIANO_RESET target=%u storage_quiesced=1 exit_boot_services=0\n",Target));
  // ResetSystem is valid while Boot Services are active. Keep every device's
  // DMA allocation alive until the platform reset stops the hardware.
  EFI_RESET_TYPE Type=Target==PIANO_PRODUCT_RESET_SHUTDOWN?EfiResetShutdown:EfiResetCold;
  gRT->ResetSystem(Type,EFI_SUCCESS,0,NULL);
  return EFI_DEVICE_ERROR;
}
