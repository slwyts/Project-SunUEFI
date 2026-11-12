// SPDX-License-Identifier: BSD-2-Clause-Patent
// Raw display-space coordinates; LVGL performs display rotation afterwards.
#pragma once
#include <stdint.h>
static inline int32_t PianoTouchAxis(uint64_t Value,uint64_t Minimum,
                                    uint64_t Maximum,int32_t Extent) {
  if(Maximum<=Minimum || Extent<=1)return 0;
  if(Value<=Minimum)return 0;
  if(Value>=Maximum)return Extent-1;
  return (int32_t)(((double)(Value-Minimum)/(double)(Maximum-Minimum))*(Extent-1));
}
static inline int PianoTouchPressed(uint32_t Buttons) {return (Buttons&1)!=0;}
