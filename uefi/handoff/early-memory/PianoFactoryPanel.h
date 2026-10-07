// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define PIANO_FACTORY_BOOTARGS_MAX 8192U
#define PIANO_FACTORY_PANEL_ARGUMENT "msm_drm.dsi_display0="
#define PIANO_FACTORY_PANEL_BOE "qcom,mdss_dsi_p81_42_02_0a_dualdsi_dsc_vid"
#define PIANO_FACTORY_PANEL_CSOT "qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid"
typedef enum {PianoFactoryPanelUnknown=0,PianoFactoryPanelBoe,PianoFactoryPanelCsot} PIANO_FACTORY_PANEL;

// A complete bounded bootarg token, not a substring or a inferred GPIO ID.
STATIC inline BOOLEAN PianoFactoryPanelPrefix(CONST CHAR8 *Token,UINTN Bytes){
 CONST CHAR8 Prefix[]=PIANO_FACTORY_PANEL_ARGUMENT;
 if(Bytes<sizeof(Prefix)-1)return FALSE;
 for(UINTN I=0;I<sizeof(Prefix)-1;++I)if(Token[I]!=Prefix[I])return FALSE;
 return TRUE;
}
STATIC inline UINT32 PianoFactoryPanelToken(CONST CHAR8 *Token,UINTN Bytes){
 if(!PianoFactoryPanelPrefix(Token,Bytes))return PianoFactoryPanelUnknown;
 CONST CHAR8 *Names[]={PIANO_FACTORY_PANEL_BOE,PIANO_FACTORY_PANEL_CSOT};
 CONST UINTN Lengths[]={sizeof(PIANO_FACTORY_PANEL_BOE)-1,sizeof(PIANO_FACTORY_PANEL_CSOT)-1};
 UINTN Start=sizeof(PIANO_FACTORY_PANEL_ARGUMENT)-1;
 for(UINTN N=0;N<2;++N){
  UINTN Count=Lengths[N];if(Bytes<Start+Count)continue;BOOLEAN Same=TRUE;
  for(UINTN I=0;I<Count;++I)if(Token[Start+I]!=Names[N][I]){Same=FALSE;break;}
  if(Same&&(Bytes==Start+Count||Token[Start+Count]==':'))return (UINT32)N+1;
 }
 return PianoFactoryPanelUnknown;
}
