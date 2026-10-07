// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoPanelSelection.h"
#include "PianoFactoryPanel.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>

UINT32 PianoProductBootObjectsFactoryPanel(VOID);
STATIC CONST CHAR8 *mPanelCompatible[]={"xiaomi,piano-boe-nt36532","xiaomi,piano-csot-nt36532"};
EFI_STATUS PianoPanelApplyFactory(VOID *Tree,UINTN Capacity,UINT32 Panel){
 if(!Tree||Capacity<40||Capacity>0x210000||FdtCheckHeader(Tree)||FdtTotalSize(Tree)>Capacity)return EFI_INVALID_PARAMETER;
 if(Panel==PianoFactoryPanelUnknown){DEBUG((DEBUG_WARN,"PIANO_PANEL_SELECT factory=unknown explicit_input_kept=1\n"));return EFI_SUCCESS;}
 if(Panel>PianoFactoryPanelCsot)return EFI_COMPROMISED_DATA;
 INT32 Node=-1;UINT32 Matches=0;
 for(UINTN I=0;I<2;++I){INT32 At=-1;while((At=FdtNodeOffsetByCompatible(Tree,At,mPanelCompatible[I]))>=0){Node=At;if(++Matches>1)return EFI_COMPROMISED_DATA;}}
 if(!Matches){DEBUG((DEBUG_WARN,"PIANO_PANEL_SELECT factory=%u target=not_piano explicit_input_kept=1\n",Panel));return EFI_SUCCESS;}
 INT32 Length=0;CONST CHAR8 *Input=FdtGetProp(Tree,Node,"compatible",&Length);
 CHAR8 Output[256];if(!Input||Length<=0||Length>(INT32)sizeof(Output)-1||Input[Length-1])return EFI_COMPROMISED_DATA;
 UINTN Used=0;UINT32 Replaced=0;
 for(UINTN At=0;At<(UINTN)Length;){UINTN End=At;while(End<(UINTN)Length&&Input[End])++End;if(End==(UINTN)Length)return EFI_COMPROMISED_DATA;
  CONST CHAR8 *Name=Input+At;UINTN Bytes=End-At+1;
  if(!AsciiStrCmp(Name,mPanelCompatible[0])||!AsciiStrCmp(Name,mPanelCompatible[1])){Name=mPanelCompatible[Panel-1];Bytes=AsciiStrLen(Name)+1;++Replaced;}
  if(Bytes>sizeof(Output)-Used)return EFI_BAD_BUFFER_SIZE;CopyMem(Output+Used,Name,Bytes);Used+=Bytes;At=End+1;
 }
 if(Replaced!=1)return EFI_COMPROMISED_DATA;
 if(Used==(UINTN)Length&&!CompareMem(Input,Output,Used)){DEBUG((DEBUG_WARN,"PIANO_PANEL_SELECT factory=%u unchanged=1\n",Panel));return EFI_SUCCESS;}
 INT32 E=FdtSetProp(Tree,Node,"compatible",Output,(UINT32)Used);
 DEBUG((DEBUG_WARN,"PIANO_PANEL_SELECT factory=%u node=%d old_bytes=%d new_bytes=%u fdt_status=%d\n",Panel,Node,Length,(UINT32)Used,E));
 return E==0?EFI_SUCCESS:E==-3?EFI_BUFFER_TOO_SMALL:EFI_COMPROMISED_DATA;
}
EFI_STATUS PianoPanelSelectDtb(VOID *Tree,UINTN Capacity){return PianoPanelApplyFactory(Tree,Capacity,PianoProductBootObjectsFactoryPanel());}
