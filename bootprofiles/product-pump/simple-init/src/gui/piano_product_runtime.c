/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "piano_product_runtime.h"
#if defined(ENABLE_UEFI) && defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
#include <Library/PianoProductPumpLib.h>
#include <Library/BaseLib.h>
// Existing cooperative exit, not direct StartImage/keyboard injection.
extern void gui_run_and_exit(int (*run)(void *));
void piano_product_gui_pump(void) {
  if(!PianoProductPumpBootServicesAlive()){CpuDeadLoop();return;}
  PianoProductPumpApplication(PIANO_PRODUCT_PUMP_GUI,1000);
  if(!PianoProductPumpBootServicesAlive()){CpuDeadLoop();return;}
  UINT32 Action=PIANO_PRODUCT_ACTION_NONE;UINT64 Sequence=0;
  EFI_STATUS Status=PianoProductGetPendingAction(&Action,&Sequence);
  if(!PianoProductPumpBootServicesAlive()){CpuDeadLoop();return;}
  if(Status==EFI_SUCCESS && Action!=PIANO_PRODUCT_ACTION_NONE)
    gui_run_and_exit(0); // parent consumes/acks; do not swallow a later sequence
}
#else
void piano_product_gui_pump(void) { }
#endif
