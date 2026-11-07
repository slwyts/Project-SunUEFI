// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
// Data excludes the SPI address and dummy byte. The original module reads
// 108 bytes, checks the first 65, and uses ten 6-byte 16-bit coordinate slots.
// A selected finger remains primary until lifted. Do not turn another finger
// into a drag across the display while the first is being released.
static inline int Y700TouchDecode(const uint8_t *data,size_t bytes,
  uint32_t max_x,uint32_t max_y,uint8_t *primary,
  uint32_t *x,uint32_t *y,int *pressed) {
  if(!data||bytes!=108||!max_x||!max_y||!primary||!x||!y||!pressed)return -1;
  uint8_t sum=0;
  for(size_t i=0;i<65;++i)sum=(uint8_t)(sum+data[i]);
  if(sum)return -1;
  int recovery=1;
  for(size_t i=0;i<6;++i)if(data[i]!=0xfd&&data[i]!=0xfe)recovery=0;
  if(recovery)return -1;
  uint8_t first=0;uint32_t fx=0,fy=0;
  for(size_t i=0;i<10;++i) {
    const uint8_t *p=data+6*i;uint8_t id=p[0]>>3,state=p[0]&7;
    if(!id||id>10||(state!=1&&state!=2))continue;
    uint32_t px=((uint32_t)p[1]<<8)|p[2],py=((uint32_t)p[3]<<8)|p[4];
    if(px>max_x||py>max_y)return -1;
    if(*primary==id){*x=px;*y=py;*pressed=1;return 0;}
    if(!first){first=id;fx=px;fy=py;}
  }
  if(*primary){*primary=0;*pressed=0;return 0;}
  *primary=first;*pressed=first!=0;*x=fx;*y=fy;return 0;
}
