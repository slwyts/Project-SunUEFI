/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "piano_product_runtime.h"
#ifdef ENABLE_UEFI
#include <Uefi.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#endif
#if defined(ENABLE_UEFI) && defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
#include <Library/PianoProductPumpLib.h>
#include <Library/BaseLib.h>
// Existing cooperative exit, not direct StartImage/keyboard injection.
extern void gui_run_and_exit(int (*run)(void *));
#include <stdbool.h>
extern bool gui_run;
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

#ifdef ENABLE_UEFI
/* TimerLib owns counter calibration. Preserve fractional milliseconds and the
 * advertised counter direction/wrap; UI work between calls counts as time. */
uint64_t piano_product_gui_tick(void) {
  static BOOLEAN Initialized;
  static UINT64 Start,End,Last,Nanoseconds;
  UINT64 Now,Delta,Extra;
#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
  if(!PianoProductPumpBootServicesAlive()){CpuDeadLoop();return 0;}
#endif
  if(!Initialized){
    if(!GetPerformanceCounterProperties(&Start,&End)||Start==End){CpuDeadLoop();return 0;}
    Last=GetPerformanceCounter();Initialized=TRUE;
    if(Last<(Start<End?Start:End)||Last>(Start<End?End:Start)){CpuDeadLoop();return 0;}
    return 0;
  }
  Now=GetPerformanceCounter();
  if(Now<(Start<End?Start:End)||Now>(Start<End?End:Start)){CpuDeadLoop();return 0;}
  if(Start<End){
    if(Now>=Last)Delta=Now-Last;
    else{
      Delta=End-Last;Extra=Now-Start;
      if(Extra==MAX_UINT64||Delta>MAX_UINT64-Extra-1){CpuDeadLoop();return 0;}
      Delta+=Extra+1;
    }
  }else{
    if(Now<=Last)Delta=Last-Now;
    else{
      Delta=Last-End;Extra=Start-Now;
      if(Extra==MAX_UINT64||Delta>MAX_UINT64-Extra-1){CpuDeadLoop();return 0;}
      Delta+=Extra+1;
    }
  }
  Last=Now;Extra=GetTimeInNanoSecond(Delta);
  if(Nanoseconds>MAX_UINT64-Extra){CpuDeadLoop();return 0;}
  Nanoseconds+=Extra;
  return Nanoseconds/1000000;
}

uint32_t piano_product_gui_wait(uint32_t milliseconds) {
  UINT64 Before,After;
  UINT32 Slice;
  if(!milliseconds)return 0;
  /* LVGL can return UINT32_MAX when no timer is ready. Never turn it into an
   * unbounded Stall or synthetic advance of the animation clock. */
  if(milliseconds>30)milliseconds=30;
  Before=piano_product_gui_tick();
  for(Slice=0;Slice<milliseconds;Slice++){
#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
    if(!gui_run)break;
    piano_product_gui_pump();
    if(!gui_run)break;
#endif
    if(gBS->Stall(1000)!=EFI_SUCCESS){CpuDeadLoop();return 0;}
#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
    if(!PianoProductPumpBootServicesAlive()){CpuDeadLoop();return 0;}
#endif
  }
  After=piano_product_gui_tick();
  /* The old NO_TIMER diagnostic path uses only measured wait time. Product
   * custom_tick_get uses the complete elapsed clock directly. */
  return After-Before>MAX_UINT32?MAX_UINT32:(uint32_t)(After-Before);
}
#else
uint64_t piano_product_gui_tick(void) { return 0; }
uint32_t piano_product_gui_wait(uint32_t milliseconds) { (void)milliseconds;return 0; }
#endif
