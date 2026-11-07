// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <string.h>
#include "../../uefi/platforms/y700Pkg/Y700TouchDecode.h"
static void checksum(uint8_t *p){uint8_t s=0;for(int i=0;i<64;++i)s+=p[i];p[64]=(uint8_t)-s;}
int main(void) {
  // Original-module format: id=1, contact-enter, 16-bit BE corner19040/30400.
  uint8_t frame[108]={0x09,0x4a,0x60,0x76,0xc0,0x01};
  uint8_t primary=0;uint32_t x=0,y=0;int pressed=0;checksum(frame);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==0);
  assert(primary==1&&pressed&&x==19040&&y==30400);
  // Moving the existing contact to 0/0 preserves the contact identity.
  frame[0]=0x0a;memset(frame+1,0,4);checksum(frame);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==0);
  assert(primary==1&&pressed&&x==0&&y==0);
  // Losing finger1 while finger2 stays must deliver a release before a new press.
  memset(frame,0,sizeof(frame));frame[6]=0x11;frame[8]=100;frame[10]=200;checksum(frame);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==0);
  assert(primary==0&&!pressed);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==0);
  assert(primary==2&&pressed&&x==100&&y==200);
  // A corrupt transport sample must not synthesize a finger lift.
  frame[2]^=1;
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==-1);
  assert(primary==2);
  memset(frame,0,sizeof(frame));checksum(frame);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==0);
  assert(primary==0&&!pressed);
  frame[0]=9;frame[1]=0x4a;frame[2]=0x61;checksum(frame);
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==-1);
  memset(frame,0xff,sizeof(frame));
  assert(Y700TouchDecode(frame,108,19040,30400,&primary,&x,&y,&pressed)==-1);
  assert(Y700TouchDecode(frame,65,19040,30400,&primary,&x,&y,&pressed)==-1);
  return 0;
}
