// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "../bootprofiles/uefi-app/PianoTouchInput.h"
int main(void) {
  assert(PianoTouchAxis(0,0,3200,3200)==0);
  assert(PianoTouchAxis(3200,0,3200,3200)==3199);
  assert(PianoTouchAxis(99999,0,3200,3200)==3199);
  assert(PianoTouchAxis(999,1000,2000,2136)==0);
  assert(PianoTouchAxis(1500,1000,2000,2136)==1067);
  assert(PianoTouchAxis(2000,1000,2000,2136)==2135);
  assert(PianoTouchAxis(100,0,0,3200)==0);
  assert(PianoTouchAxis(100,0,3200,0)==0);
  assert(PianoTouchAxis(UINT64_MAX/2,0,UINT64_MAX,3200)==1599);
  // Releasing at the same coordinates changes input state.
  assert(PianoTouchPressed(1)==1 && PianoTouchPressed(0)==0);
  assert(PianoTouchPressed(2)==0 && PianoTouchPressed(3)==1);
  puts("AbsolutePointer range, clamping, full uint64 ranges and press/release conversion passed.");
  return 0;
}
