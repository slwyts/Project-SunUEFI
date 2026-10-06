// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real no-target-load observer. Fixed physical pages are not mapped on the host.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#define PIANO_FB_MAPPING_HOST_TEST 1
#include "../bootprofiles/uefi-app/PianoFrameBufferMappingObserve.c"
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiGraphicsOutputProtocolGuid={.Data1=1};
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_DXE_SERVICES Ds;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
STATIC UINT32 Case,Calls,AtCalls,GcdCalls,CpuCalls,Logs,Longest;
STATIC BOOLEAN ServicesLive=TRUE;STATIC EFI_TPL Tpl=TPL_APPLICATION;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT64 EFIAPI DivU64x32Remainder(UINT64 D,UINT32 V,UINT32 *R){if(R)*R=(UINT32)(D%V);return D/V;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINTN EFIAPI StrnLenS(CONST CHAR16 *S,UINTN N){UINTN I=0;while(I<N&&S[I])++I;return I;}
UINT16 EFIAPI ReadUnaligned16(CONST VOID *P){UINT16 V;memcpy(&V,P,2);return V;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,4);return V;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"Print assert%s:%lu %s\n",F,(unsigned long)L,D);abort();}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){
  assert(ServicesLive&&L);CHAR8 Buffer[256];VA_LIST Args;VA_START(Args,Fmt);UINTN N=AsciiVSPrint(Buffer,sizeof(Buffer),Fmt,Args);VA_END(Args);
  assert(N&&N<180&&Buffer[N-1]=='\n');Logs++;if(N>Longest)Longest=(UINT32)N;
}
STATIC BOOLEAN EFIAPI Alive(VOID){return ServicesLive;}
STATIC VOID EndCall(VOID){Calls++;if(Case>=1&&Case<=10&&Calls==Case){ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;Gop.Mode=(VOID *)1;}}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){assert(ServicesLive&&New==TPL_HIGH_LEVEL);EFI_TPL Old=Tpl;EndCall();return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){assert(ServicesLive&&Old==Tpl);EndCall();}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Key,VOID **Out){assert(ServicesLive&&Guid==&gEfiGraphicsOutputProtocolGuid&&!Key);*Out=&Gop;EndCall();return Case==15?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  assert(ServicesLive&&Address==mPages[GcdCalls%3]);GcdCalls++;
  *D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=PIANO_FB_MAPPING_BASE,.Length=0x2b00000,.Capabilities=0x703c07,.Attributes=EFI_MEMORY_WT|EFI_MEMORY_XP,.GcdMemoryType=EfiGcdMemoryTypeReserved};
  if(Case==22&&GcdCalls>3)D->Attributes=EFI_MEMORY_WB;if(Case==29)D->Attributes|=EFI_MEMORY_RP;
  EndCall();return Case==16?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
EFI_STATUS PianoFbMappingHostCpu(PIANO_FB_CPU_STATE *S){
  assert(ServicesLive);CpuCalls++;*S=(PIANO_FB_CPU_STATE){4,0x30d00805,0x480803514ULL,0xd7200000,0,0xbbff4400,0};
  if(Case==20)S->El=8;if(Case==21&&CpuCalls>1)S->Mair^=1;
  if(Case==31)S->Tcr|=BIT39;if(Case==32)S->Tcr|=BIT40;if(Case==33)S->Tcr|=BIT59;if(Case==34)S->Sctlr&=~BIT0;
  if(Case==35&&GcdCalls)S->Tcr|=BIT39;if(Case==36)S->Tcr|=1ULL<<14;if(Case==37)S->Ttbr0|=0x10;
  return EFI_SUCCESS;
}
EFI_STATUS PianoFbMappingHostAt(UINT64 Address,UINT64 *Par){
  assert(ServicesLive&&Address==mPages[AtCalls%3]);AtCalls++;*Par=Address|(0xbbULL<<56);
  if(Case==17)*Par|=1;if(Case==18)*Par+=4096;if(Case==19)*Par=Address;
  return Case==23?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC VOID Setup(UINT32 N){Case=N;gBS=&Bs;gDS=&Ds;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Ds.GetMemorySpaceDescriptor=Gcd;
  Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=3200,.VerticalResolution=2136,.PixelsPerScanLine=3200,.PixelFormat=PixelBlueGreenRedReserved8BitPerColor};
  Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Mode=0,.Info=&Info,.SizeOfInfo=sizeof(Info),.FrameBufferBase=PIANO_FB_MAPPING_BASE,.FrameBufferSize=PIANO_FB_MAPPING_BYTES};Gop.Mode=&Mode;
  if(N==11)Mode.FrameBufferBase=0;if(N==12)Mode.FrameBufferBase+=4096;if(N==13)Mode.FrameBufferSize--;
  if(N==14)Gop.Mode=NULL;if(N==27)Tpl=TPL_CALLBACK;if(N==28){ServicesLive=FALSE;gBS=(VOID *)1;gDS=(VOID *)1;}
}
STATIC VOID Run(UINT32 N){
  Setup(N);if(N==26){assert(PianoFrameBufferMappingObserve(NULL,Alive)==EFI_INVALID_PARAMETER&&PianoFrameBufferMappingObserve("phase",NULL)==EFI_INVALID_PARAMETER&&PianoFrameBufferMappingObserve("12345678901234567890123456789012",Alive)==EFI_INVALID_PARAMETER&&!Calls);return;}
  if(N==24){for(UINT32 I=0;I<32;++I)assert(PianoFrameBufferMappingObserve("phase",Alive)==EFI_SUCCESS);UINT32 C=Calls;assert(PianoFrameBufferMappingObserve("overflow",Alive)==EFI_OUT_OF_RESOURCES&&Calls==C);return;}
  EFI_STATUS S=PianoFrameBufferMappingObserve("1234567890123456789012345678901",Alive);
  CONST PIANO_FB_MAPPING_REPORT *R=PianoFrameBufferMappingGetReport();assert(R->Count==1);CONST PIANO_FB_MAPPING_SNAPSHOT *P=&R->Snapshot[0];assert(P->Status==S);
  if(N>=1&&N<=10){assert(S==EFI_ABORTED&&Calls==N&&!Logs&&R->ServicesLost&&PianoFrameBufferMappingRetained());UINT32 C=Calls;assert(PianoFrameBufferMappingObserve("retry",Alive)==EFI_NOT_READY&&Calls==C&&PianoFrameBufferMappingReemit(Alive)==EFI_ABORTED);return;}
  if(N==0||N==19||N==25||N==29||N==30){assert(S==EFI_SUCCESS&&P->Coherent&&P->Rounds==2&&P->SampleCount==6&&AtCalls==6&&GcdCalls==6&&CpuCalls==10&&!R->ServicesLost);}
  if(N>=11&&N<=13)assert(S==EFI_SECURITY_VIOLATION&&!AtCalls&&!GcdCalls&&P->Round[0].CpuStatus==EFI_NOT_STARTED&&P->Round[0].Sample[0].GcdStatus==EFI_NOT_STARTED&&P->Round[0].Sample[0].AtStatus==EFI_NOT_STARTED);
  if(N==14)assert(S==EFI_COMPROMISED_DATA&&!AtCalls&&!GcdCalls);
  if(N==15)assert(S==EFI_DEVICE_ERROR&&!AtCalls&&!GcdCalls&&!R->ServicesLost);
  if(N==16)assert(S==EFI_DEVICE_ERROR&&AtCalls==6&&GcdCalls==6&&!R->ServicesLost);
  if(N==23)assert(S==EFI_DEVICE_ERROR&&AtCalls==1&&GcdCalls==1&&!R->ServicesLost);
  if(N==23)assert(!P->Round[0].Sample[0].Identity);
  if(N==17||N==18||N==22)assert(S==EFI_NOT_READY&&AtCalls==6&&GcdCalls==6&&!R->ServicesLost);
  if(N==21)assert(S==EFI_NOT_READY&&!AtCalls&&GcdCalls==1&&!R->ServicesLost);
  if(N==17)assert(P->Round[0].Sample[0].ParFault&&!P->Round[0].Sample[0].Identity);
  if(N==18)assert(!P->Round[0].Sample[0].ParFault&&!P->Round[0].Sample[0].Identity);
  if(N==19)assert(!P->Round[0].Sample[0].Attribute);
  if(N==20)assert(S==EFI_UNSUPPORTED&&!AtCalls&&!GcdCalls);
  if(N==21||N==22)assert(!P->Coherent);
  if(N==25){UINT32 C=Calls,A=AtCalls,G=GcdCalls;gBS=(VOID *)1;gDS=(VOID *)1;Gop.Mode=(VOID *)1;assert(PianoFrameBufferMappingReemit(Alive)==EFI_SUCCESS&&Calls==C&&AtCalls==A&&GcdCalls==G);}
  if(N==27)assert(S==EFI_UNSUPPORTED&&Calls==2&&!AtCalls&&!GcdCalls);
  if(N==28)assert(S==EFI_ABORTED&&!Calls&&R->ServicesLost);
  if(N==29)assert(P->Round[0].Sample[0].ReadProtected);
  if(N>=31&&N<=34)assert(S==EFI_UNSUPPORTED&&!AtCalls&&!GcdCalls&&CpuCalls==1&&!R->ServicesLost);
  if(N==35)assert(S==EFI_UNSUPPORTED&&!AtCalls&&GcdCalls==1&&(P->Round[0].After.Tcr&BIT39)&&!R->ServicesLost);
  if(N==36||N==37)assert(S==EFI_UNSUPPORTED&&!AtCalls&&!GcdCalls&&!R->ServicesLost);
  if(N==30){PIANO_FB_MAPPING_SNAPSHOT *W=(VOID *)P;W->Status=EFI_INCOMPATIBLE_VERSION;
    for(UINT32 J=0;J<2;++J){PIANO_FB_MAPPING_ROUND *Q=&W->Round[J];Q->MetadataStatus=Q->CpuStatus=EFI_INCOMPATIBLE_VERSION;Q->Before=Q->After=(PIANO_FB_CPU_STATE){MAX_UINT64,MAX_UINT64,MAX_UINT64,MAX_UINT64,MAX_UINT64,MAX_UINT64,MAX_UINT64};
      for(UINT32 I=0;I<3;++I){PIANO_FB_MAPPING_SAMPLE *T=&Q->Sample[I];T->GcdBase=T->GcdLength=T->GcdAttributes=T->GcdCapabilities=T->Par=T->PhysicalPage=MAX_UINT64;T->GcdType=MAX_UINT32;T->GcdStatus=T->AtStatus=EFI_INCOMPATIBLE_VERSION;}
    }assert(PianoFrameBufferMappingReemit(Alive)==EFI_SUCCESS);printf("Actual FB mapping AsciiVSPrint(256): max line%u completeCRLF\n",Longest);}
}
int main(VOID){for(UINT32 I=0;I<38;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);fflush(stdout);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"FB mapping case%u failed\n",I);return 1;}}puts("Actual no-target-load FB mapping:38 cases, fixedAT/GCD/CPU/TPL/EBS/32phase/replay/HA-HD-DS-MMUoff gates; no FB or PTE access");return 0;}
