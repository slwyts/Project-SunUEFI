// SPDX-License-Identifier: BSD-2-Clause-Patent
// EP0 Chapter 9 and the standard fastboot FF/42/03 bulk interface.
#include "PianoUsbControl.h"
#include <Library/BaseMemoryLib.h>
#ifndef PIANO_USB_FASTBOOT
#define PIANO_USB_FASTBOOT 0
#endif
STATIC CONST UINT8 Device[]={18,1,0,2,0,0,0,64,0x09,0x12,0x50,0x87,0,1,1,2,3,1};
#if PIANO_USB_FASTBOOT
STATIC CONST UINT8 Config[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,0xFF,0x42,3,0,
  7,5,1,2,0,2,0,7,5,0x81,2,0,2,0};
#else
STATIC CONST UINT8 Config[]={9,2,18,0,1,1,0,0x80,50,9,4,0,0,0,0xFF,0,0,0};
#endif
STATIC UINT16 Le16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
EFI_STATUS PianoUsbControlSetup(PIANO_USB_CONTROL *S,CONST UINT8 U[8],UINT8 *Data,UINTN Capacity,UINTN *Bytes,PIANO_USB_CONTROL_ACTION *A) {
  if(S==NULL || U==NULL || Data==NULL || Bytes==NULL || A==NULL || Capacity<64)return EFI_INVALID_PARAMETER;
  *Bytes=0;*A=PianoUsbStall;S->SetAddress=S->SetConfiguration=FALSE;
  UINT16 Value=Le16(U+2),Index=Le16(U+4),Length=Le16(U+6);UINTN Size=0;
  if(U[0]==0x80 && U[1]==6 && Length) {
    UINT8 Type=(UINT8)(Value>>8),Id=(UINT8)Value;
    if(Type==1 && Id==0 && Index==0){CopyMem(Data,Device,sizeof(Device));if(S->SuperSpeed){Data[3]=3;Data[7]=9;}Size=sizeof(Device);}
    else if((Type==2 || Type==7) && Id==0 && Index==0) {
#if PIANO_USB_FASTBOOT
      if(Type==7 && S->SuperSpeed)return EFI_UNSUPPORTED;
      CopyMem(Data,Config,18);Data[1]=Type;Size=18;
      UINT16 Mps=S->SuperSpeed?1024:(S->Speed==1 || S->Speed==3?64:512);
      if(Type==7)Mps=Mps==64?512:64;
      for(UINTN Ep=0;Ep<2;++Ep) {
        CopyMem(Data+Size,Config+18+Ep*7,7);Data[Size+4]=(UINT8)Mps;Data[Size+5]=(UINT8)(Mps>>8);Size+=7;
        if(S->SuperSpeed){UINT8 Companion[]={6,48,0,0,0,0};CopyMem(Data+Size,Companion,6);Size+=6;}
      }
      Data[2]=(UINT8)Size;Data[3]=0;
#else
      CopyMem(Data,Config,sizeof(Config));Data[1]=Type;Size=sizeof(Config);
#endif
    }
    else if(Type==6 && Id==0 && Index==0){UINT8 Q[]={10,6,0,2,0,0,0,64,1,0};CopyMem(Data,Q,sizeof(Q));Size=sizeof(Q);}
    else if(Type==15 && Id==0 && Index==0 && S->SuperSpeed){UINT8 Bos[]={5,15,22,0,2,7,16,2,0,0,0,0,10,16,3,0,8,0,3,10,0,2};CopyMem(Data,Bos,sizeof(Bos));Size=sizeof(Bos);}
    else if(Type==3 && Id==0){UINT8 L[]={4,3,9,4};CopyMem(Data,L,sizeof(L));Size=sizeof(L);}
    else if(Type==3 && Id<=3 && Index==0x0409) {
      CONST CHAR8 *Text=Id==1?"SunUEFI":Id==2?(PIANO_USB_FASTBOOT?"piano fastboot debug":"piano EP0 debug"):"SunUEFI-piano";
      Size=2;while(*Text && Size+2<=Capacity){Data[Size++]=(UINT8)*Text++;Data[Size++]=0;}
      Data[0]=(UINT8)Size;Data[1]=3;
    } else return EFI_UNSUPPORTED;
  } else if(U[0]==0 && U[1]==5 && Index==0 && Length==0 && Value<=127) {
    S->PendingAddress=(UINT8)Value;S->SetAddress=TRUE;*A=PianoUsbStatusIn;return EFI_SUCCESS;
  } else if(U[0]==0 && U[1]==9 && Index==0 && Length==0 && Value<=1 && S->Address!=0) {
    S->PendingConfiguration=(UINT8)Value;S->SetConfiguration=TRUE;*A=PianoUsbStatusIn;return EFI_SUCCESS;
  } else if(U[0]==0x80 && U[1]==8 && Value==0 && Index==0 && Length==1) {Data[0]=S->Configuration;Size=1;}
  else if((U[0]==0x80 || U[0]==0x81 || U[0]==0x82) && U[1]==0 && Value==0 && Length==2 &&
          ((U[0]==0x80 && Index==0) || (U[0]==0x81 && Index==0 && S->Configuration) ||
           (U[0]==0x82 && (Index==0 || Index==0x80 || (PIANO_USB_FASTBOOT && S->Configuration && (Index==1 || Index==0x81)))))) {
    Data[0]=Data[1]=0;Size=2;
  } else if(U[0]==0x81 && U[1]==10 && Value==0 && Index==0 && Length==1 && S->Configuration) {
    Data[0]=0;Size=1;
  } else if(U[0]==1 && U[1]==11 && Value==0 && Index==0 && Length==0 && S->Configuration) {
    *A=PianoUsbStatusIn;return EFI_SUCCESS;
  } else if(U[0]==0xC0 && U[1]==0x5A && Value==0 && Index==0 && Length && S->Configuration==1) {
    CopyMem(Data,"SUNUEFI1",8);Data[8]=S->Address;Data[9]=S->Configuration;Data[10]=1;Data[11]=0x40;Size=12;
  } else return EFI_UNSUPPORTED;
  *Bytes=MIN(Size,(UINTN)Length);if(*Bytes>Capacity)return EFI_BAD_BUFFER_SIZE;
  *A=PianoUsbDataIn;return EFI_SUCCESS;
}
VOID PianoUsbControlStatusComplete(PIANO_USB_CONTROL *S) {
  if(S->SetAddress){S->Address=S->PendingAddress;S->Configuration=0;}
  if(S->SetConfiguration)S->Configuration=S->PendingConfiguration;
  S->SetAddress=S->SetConfiguration=FALSE;
}
