// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoUsbControl.c"
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
int main(void){
  PIANO_USB_CONTROL s={0};UINT8 data[256],u[8]={0x80,6,0,1,0,0,8,0};UINTN n;PIANO_USB_CONTROL_ACTION a;
  assert(PianoUsbControlSetup(&s,u,data,256,&n,&a)==EFI_SUCCESS && a==PianoUsbDataIn && n==8 && data[0]==18);
  u[3]=2;u[6]=255;assert(PianoUsbControlSetup(&s,u,data,256,&n,&a)==EFI_SUCCESS && n==18 && data[13]==0);
  u[3]=3;u[2]=3;u[4]=9;u[5]=4;assert(PianoUsbControlSetup(&s,u,data,256,&n,&a)==EFI_SUCCESS && data[1]==3);
  UINT8 addr[8]={0,5,17,0,0,0,0,0};assert(PianoUsbControlSetup(&s,addr,data,256,&n,&a)==EFI_SUCCESS && !s.Address);
  PianoUsbControlStatusComplete(&s);assert(s.Address==17);
  addr[1]=9;addr[2]=1;assert(PianoUsbControlSetup(&s,addr,data,256,&n,&a)==EFI_SUCCESS && !s.Configuration);
  PianoUsbControlStatusComplete(&s);assert(s.Configuration==1);
  UINT8 debug[8]={0xc0,0x5a,0,0,0,0,64,0};
  assert(PianoUsbControlSetup(&s,debug,data,256,&n,&a)==EFI_SUCCESS && n==12 && !memcmp(data,"SUNUEFI1",8));
  addr[1]=5;addr[2]=128;assert(PianoUsbControlSetup(&s,addr,data,256,&n,&a)==EFI_UNSUPPORTED && a==PianoUsbStall);
  s.SuperSpeed=TRUE;UINT8 ss[8]={0x80,6,0,1,0,0,18,0};
  assert(PianoUsbControlSetup(&s,ss,data,256,&n,&a)==EFI_SUCCESS && data[3]==3 && data[7]==9);
  ss[3]=15;ss[6]=64;assert(PianoUsbControlSetup(&s,ss,data,256,&n,&a)==EFI_SUCCESS && n==22 && data[4]==2);
  puts("USB EP0: descriptor truncation, strings, delayed address/configuration, configured read-only debug and rejected malformed requests passed.");
}
