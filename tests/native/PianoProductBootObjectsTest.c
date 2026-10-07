// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual DXE cache/validator. GUID-list and CPU lifetime boundaries are fixtures.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoProductBootObjects.h"
#include <Library/HobLib.h>
#include <Library/DebugLib.h>
static EFI_GUID Guid=PIANO_COLD_OBJECT_HOB_GUID;
static struct {EFI_HOB_GUID_TYPE G;PIANO_COLD_BOOT_OBJECT_REPORT R;} Hob;
static UINTN Case,First,Next,Copies,Logs;static BOOLEAN Live=TRUE;
static BOOLEAN Alive(VOID){return Live;}
VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *G){assert(Live&&!memcmp(G,&Guid,sizeof(Guid)));First++;if(Case==17)Live=FALSE;return Case==1?NULL:&Hob;}
VOID *EFIAPI GetNextGuidHob(CONST EFI_GUID *G,CONST VOID *P){assert(Live&&!memcmp(G,&Guid,sizeof(Guid))&&P==(UINT8*)&Hob+Hob.G.Header.HobLength);Next++;if(Case==18)Live=FALSE;return Case==2?&Hob:NULL;}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){if(S==&Hob.R&&N==sizeof(Hob.R)){Copies++;if(Case==16&&Copies==2)Hob.R.Reason=PianoColdReasonRead;if((Case==19&&Copies==1)||(Case==20&&Copies==2))Live=FALSE;}return memmove(D,S,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;assert(Live);Logs++;}
int main(int argc,char **argv){assert(argc==2);Case=strtoul(argv[1],NULL,10);Hob.G=(EFI_HOB_GUID_TYPE){.Header={.HobType=EFI_HOB_TYPE_GUID_EXTENSION,.HobLength=sizeof(Hob)},.Name=Guid};
 Hob.R=(PIANO_COLD_BOOT_OBJECT_REPORT){.Version=PIANO_COLD_OBJECT_VERSION,.Bytes=sizeof(Hob.R),.Status=EFI_NOT_READY,.PublishStatus=EFI_SUCCESS,.Reason=PianoColdReasonLegacyHandoff,.Attempted=TRUE,.Finished=TRUE,.Published=TRUE,.Count=1,.Objects={{PIANO_COLD_HANDOFF_ADDRESS,4096,PianoColdObjectHandoff,0}}};
 if(Case==3)Hob.G.Header.HobLength--;if(Case==4)Hob.G.Header.HobLength++;if(Case==5)Hob.G.Header.Reserved=1;if(Case==6)Hob.G.Name.Data1++;
 if(Case==8)Hob.R.Version++;if(Case==9)Hob.R.Bytes--;if(Case==10)Hob.R.AuthorityReady=TRUE;if(Case==11)Hob.R.Loads=PIANO_COLD_TOTAL_MAX/4+1;
 if(Case==12)Hob.R.Objects[0].Bytes=0;if(Case==13)Hob.R.Published=2;if(Case==14)Hob.R.Objects[0].Reserved=1;if(Case==15)Hob.R.Status=EFI_WARN_STALE_DATA;
 Hob.R.ReportCrc32=PianoColdObjectsCrc(&Hob.R);if(Case==7)Hob.R.ReportCrc32^=1;
 assert(PianoProductBootObjectsReemit(NULL)==EFI_NOT_READY&&!First);
 EFI_STATUS E=PianoProductBootObjectsReemit(Alive);
 if(Case==0){assert(E==EFI_NOT_READY&&PianoProductBootObjectsStatus()==EFI_SUCCESS&&PianoProductBootObjectsSnapshot()&&Logs>0&&First==1&&Next==1&&Copies==2);UINTN Before=Logs;memset(&Hob.R,0,sizeof(Hob.R));assert(PianoProductBootObjectsReemit(Alive)==EFI_NOT_READY&&Logs>Before&&First==1&&Copies==2);Live=FALSE;Before=Logs;assert(PianoProductBootObjectsReemit(Alive)==EFI_ABORTED&&Logs==Before);}
 else if(Case==1)assert(E==EFI_NOT_FOUND&&!Next&&!Copies&&!PianoProductBootObjectsSnapshot());
 else if(Case>=17)assert(E==EFI_ABORTED&&!Logs&&!PianoProductBootObjectsSnapshot());
 else assert(E==EFI_COMPROMISED_DATA&&!PianoProductBootObjectsSnapshot());
 puts("Actual DXE cold-object capture/replay case passed; no target/BS operations");return 0;
}
